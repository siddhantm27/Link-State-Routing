// Virtual Node.
//
// A VN learns only its own neighbours, from the Oracle Node over TCP. Everything
// else about the network it discovers by flooding: it advertises its adjacencies
// in a Link State Packet, forwards the LSPs it receives, and from the resulting
// database computes shortest paths to every node it has heard of.
//
// Wire protocol, all integers network byte order:
//
//   CONNECT     VN -> ON   6 bytes    4 IP | 2 UDP port
//   LINK-STATE  ON -> VN   11n bytes  per record: 1 letter | 4 IP | 2 port | 4 cost
//   LSP         VN -> VN   6+5k bytes 1 origin | 4 seq | 1 count, then per link:
//                                     1 letter | 4 cost
//
// In a LINK-STATE message the record with cost 0 is this node's own identity.
// Link failure is not signalled explicitly: an LSP that stops being refreshed
// ages out, and the topology it described disappears with it.

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kOraclePort = 5000;
constexpr int kConnectLen = 6;
constexpr int kRecordLen = 11;
constexpr int kLspHeaderLen = 6;
constexpr int kLspLinkLen = 5;
constexpr size_t kMaxDatagram = 2048;

// Re-advertise our own LSP this often. A node that goes quiet for kLspTimeout is
// treated as unreachable, so the timeout must clear several refresh periods to
// avoid tearing down a path over one dropped datagram.
constexpr auto kLspPeriod = std::chrono::seconds(10);
constexpr auto kLspTimeout = std::chrono::seconds(35);
constexpr auto kReportPeriod = std::chrono::seconds(30);

// LSPs are flooded unreliably, so send each one a few times.
constexpr int kFloodRepeats = 3;

using Clock = std::chrono::steady_clock;

struct Neighbour {
    std::string ip;
    uint16_t port = 0;
    int cost = 0;

    bool operator==(const Neighbour &o) const {
        return ip == o.ip && port == o.port && cost == o.cost;
    }
    bool operator!=(const Neighbour &o) const { return !(*this == o); }
};

struct LinkState {
    uint32_t seq = 0;
    std::map<char, int> links;
    Clock::time_point heard;
};

struct Route {
    int cost;
    char next_hop;

    bool operator==(const Route &o) const {
        return cost == o.cost && next_hop == o.next_hop;
    }
    bool operator!=(const Route &o) const { return !(*this == o); }
};

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// ---------------------------------------------------------------- shortest path

// Dijkstra over the flooded link state database. Returns, for every reachable
// node, its total cost and the neighbour to hand the packet to first.
//
// The database is built from advertisements that are not necessarily mutually
// consistent: a node may still be advertising a link its far end has already
// withdrawn. A link is therefore only usable when both endpoints advertise it,
// which keeps a half-torn-down link out of the routing table.
std::map<char, Route> shortest_paths(char self,
                                     const std::map<char, LinkState> &db) {
    auto advertised = [&db](char from, char to) -> const int * {
        auto it = db.find(from);
        if (it == db.end())
            return nullptr;
        auto link = it->second.links.find(to);
        return link == it->second.links.end() ? nullptr : &link->second;
    };

    struct Entry {
        int cost;
        char node;
        char first_hop;
        bool operator>(const Entry &o) const { return cost > o.cost; }
    };

    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
    std::map<char, Route> routes;

    frontier.push({0, self, 0});
    while (!frontier.empty()) {
        Entry cur = frontier.top();
        frontier.pop();
        if (routes.count(cur.node))
            continue;
        if (cur.node != self)
            routes[cur.node] = {cur.cost, cur.first_hop};

        auto it = db.find(cur.node);
        if (it == db.end())
            continue;
        for (const auto &[peer, cost] : it->second.links) {
            if (routes.count(peer))
                continue;
            const int *reverse = advertised(peer, cur.node);
            if (reverse == nullptr)
                continue; // only one end still claims this link
            // Disagreeing costs are possible mid-convergence; the higher one is
            // the safe choice, since it never makes a path look better than it is.
            int edge = std::max(cost, *reverse);
            char first = (cur.node == self) ? peer : cur.first_hop;
            frontier.push({cur.cost + edge, peer, first});
        }
    }
    return routes;
}

void print_routes(char self, const std::map<char, Route> &routes) {
    std::cout << "\n=== routing table for " << self << " ===\n";
    if (routes.empty()) {
        std::cout << "  (no reachable destinations)\n";
    } else {
        std::cout << "  dest  cost  via\n";
        for (const auto &[dest, route] : routes)
            std::cout << "   " << dest << "     " << route.cost << "     "
                      << route.next_hop << "\n";
    }
    std::cout << std::flush;
}

void print_topology(const std::map<char, LinkState> &db) {
    std::set<char> nodes;
    for (const auto &[origin, ls] : db) {
        nodes.insert(origin);
        for (const auto &[peer, _] : ls.links)
            nodes.insert(peer);
    }
    std::cout << "--- known topology (" << nodes.size() << " nodes) ---\n";
    for (const auto &[origin, ls] : db) {
        std::cout << "  " << origin << ":";
        for (const auto &[peer, cost] : ls.links)
            std::cout << " " << peer << "=" << cost;
        std::cout << "\n";
    }
    std::cout << std::flush;
}

// ------------------------------------------------------------------- flooding

// Serialise an LSP and send it to each of `targets`.
void flood(int udp_fd, char origin, uint32_t seq,
           const std::map<char, int> &links,
           const std::map<char, Neighbour> &targets) {
    std::vector<uint8_t> buf;
    buf.reserve(kLspHeaderLen + links.size() * kLspLinkLen);

    buf.push_back(static_cast<uint8_t>(origin));
    uint32_t seq_net = htonl(seq);
    const auto *seq_bytes = reinterpret_cast<const uint8_t *>(&seq_net);
    buf.insert(buf.end(), seq_bytes, seq_bytes + 4);
    buf.push_back(static_cast<uint8_t>(links.size()));

    for (const auto &[peer, cost] : links) { // std::map iterates sorted
        buf.push_back(static_cast<uint8_t>(peer));
        uint32_t cost_net = htonl(static_cast<uint32_t>(cost));
        const auto *cost_bytes = reinterpret_cast<const uint8_t *>(&cost_net);
        buf.insert(buf.end(), cost_bytes, cost_bytes + 4);
    }

    for (const auto &[peer, info] : targets) {
        if (info.cost == 0)
            continue; // that record is ourselves
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(info.port);
        if (inet_pton(AF_INET, info.ip.c_str(), &dest.sin_addr) != 1)
            continue;
        for (int i = 0; i < kFloodRepeats; ++i)
            sendto(udp_fd, buf.data(), buf.size(), 0,
                   reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
    }
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " <ON IP> <own IP> <own UDP port>\n";
        return 1;
    }
    const char *oracle_ip = argv[1];
    const char *own_ip = argv[2];
    uint16_t own_port = 0;
    try {
        own_port = static_cast<uint16_t>(std::stoi(argv[3]));
    } catch (const std::exception &) {
        std::cerr << "bad UDP port: " << argv[3] << "\n";
        return 1;
    }

    // ---- control channel to the Oracle Node
    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        perror("tcp socket");
        return 1;
    }
    sockaddr_in oracle{};
    oracle.sin_family = AF_INET;
    oracle.sin_port = htons(kOraclePort);
    if (inet_pton(AF_INET, oracle_ip, &oracle.sin_addr) != 1) {
        std::cerr << "bad ON address: " << oracle_ip << "\n";
        return 1;
    }
    if (connect(tcp_fd, reinterpret_cast<sockaddr *>(&oracle), sizeof(oracle)) < 0) {
        perror("connect");
        close(tcp_fd);
        return 1;
    }

    in_addr parsed_ip{};
    if (inet_pton(AF_INET, own_ip, &parsed_ip) != 1) {
        std::cerr << "bad own address: " << own_ip << "\n";
        close(tcp_fd);
        return 1;
    }
    uint8_t hello[kConnectLen];
    std::memcpy(hello, &parsed_ip.s_addr, 4);
    uint16_t port_net = htons(own_port);
    std::memcpy(hello + 4, &port_net, 2);
    if (send(tcp_fd, hello, sizeof(hello), 0) != kConnectLen) {
        perror("send CONNECT");
        close(tcp_fd);
        return 1;
    }

    // ---- data channel for flooding
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("udp socket");
        close(tcp_fd);
        return 1;
    }
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(own_port);
    if (bind(udp_fd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
        perror("bind udp");
        close(tcp_fd);
        close(udp_fd);
        return 1;
    }
    if (!set_nonblocking(tcp_fd) || !set_nonblocking(udp_fd)) {
        perror("fcntl");
        close(tcp_fd);
        close(udp_fd);
        return 1;
    }

    char self = 0;
    uint32_t own_seq = 0;
    std::map<char, Neighbour> neighbours;
    std::map<char, LinkState> db;
    std::map<char, Route> routes;

    auto last_advert = Clock::now();
    auto last_report = Clock::now();
    bool running = true;

    auto recompute = [&](const char *why) {
        auto fresh = shortest_paths(self, db);
        if (fresh != routes) {
            routes = std::move(fresh);
            std::cout << "[" << self << "] routes recomputed (" << why << ")\n";
            print_routes(self, routes);
        }
    };

    while (running) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(tcp_fd, &readable);
        FD_SET(udp_fd, &readable);
        timeval timeout{1, 0};

        int ready = select(std::max(tcp_fd, udp_fd) + 1, &readable, nullptr,
                           nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        // ---- topology from the Oracle Node
        if (ready > 0 && FD_ISSET(tcp_fd, &readable)) {
            uint8_t buf[kMaxDatagram];
            ssize_t n = recv(tcp_fd, buf, sizeof(buf), 0);
            if (n == 0) {
                std::cout << "[vn] Oracle Node closed the connection\n";
                break;
            }
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    perror("recv tcp");
                    break;
                }
            } else if (n % kRecordLen != 0) {
                std::cerr << "[vn] malformed LINK-STATE (" << n << " bytes)\n";
            } else {
                std::map<char, Neighbour> fresh;
                char identity = 0;
                for (ssize_t off = 0; off < n; off += kRecordLen) {
                    char letter = static_cast<char>(buf[off]);
                    in_addr ip{};
                    std::memcpy(&ip.s_addr, buf + off + 1, 4);
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
                    uint16_t port_be;
                    std::memcpy(&port_be, buf + off + 5, 2);
                    uint32_t cost_be;
                    std::memcpy(&cost_be, buf + off + 7, 4);

                    Neighbour info;
                    info.ip = ip_str;
                    info.port = ntohs(port_be);
                    info.cost = static_cast<int>(ntohl(cost_be));
                    if (info.cost == 0)
                        identity = letter;
                    fresh[letter] = std::move(info);
                }

                bool changed = (fresh != neighbours);
                neighbours = std::move(fresh);
                if (identity != 0 && identity != self) {
                    self = identity;
                    std::cout << "[vn] Oracle Node assigned identity " << self << "\n";
                }

                if (changed && self != 0) {
                    std::map<char, int> links;
                    for (const auto &[letter, info] : neighbours)
                        if (info.cost != 0)
                            links[letter] = info.cost;

                    std::cout << "[" << self << "] adjacency:";
                    for (const auto &[letter, cost] : links)
                        std::cout << " " << letter << "=" << cost;
                    if (links.empty())
                        std::cout << " (isolated)";
                    std::cout << "\n";

                    db[self] = {++own_seq, links, Clock::now()};
                    flood(udp_fd, self, own_seq, links, neighbours);
                    last_advert = Clock::now();
                    recompute("local links changed");
                }
            }
        }

        // ---- LSPs from other nodes
        if (ready > 0 && FD_ISSET(udp_fd, &readable)) {
            uint8_t buf[kMaxDatagram];
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr *>(&from), &from_len);
            if (n >= kLspHeaderLen) {
                char origin = static_cast<char>(buf[0]);
                uint32_t seq_be;
                std::memcpy(&seq_be, buf + 1, 4);
                uint32_t seq = ntohl(seq_be);
                uint8_t count = buf[5];

                if (kLspHeaderLen + count * kLspLinkLen != n) {
                    std::cerr << "[vn] malformed LSP from " << origin << "\n";
                } else if (origin != self) {
                    std::map<char, int> links;
                    for (uint8_t k = 0; k < count; ++k) {
                        int off = kLspHeaderLen + k * kLspLinkLen;
                        uint32_t cost_be;
                        std::memcpy(&cost_be, buf + off + 1, 4);
                        links[static_cast<char>(buf[off])] =
                            static_cast<int>(ntohl(cost_be));
                    }

                    auto known = db.find(origin);
                    bool newer = (known == db.end() || seq > known->second.seq);
                    if (newer) {
                        bool topology_changed =
                            (known == db.end() || known->second.links != links);
                        db[origin] = {seq, links, Clock::now()};

                        // Forward to every neighbour except the one that sent it
                        // and the node that originated it.
                        char sender = 0;
                        char sender_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &from.sin_addr, sender_ip, sizeof(sender_ip));
                        uint16_t sender_port = ntohs(from.sin_port);
                        for (const auto &[letter, info] : neighbours)
                            if (info.cost != 0 && info.ip == sender_ip &&
                                info.port == sender_port) {
                                sender = letter;
                                break;
                            }

                        auto targets = neighbours;
                        targets.erase(sender);
                        targets.erase(origin);
                        targets.erase(self);
                        flood(udp_fd, origin, seq, links, targets);

                        if (topology_changed)
                            recompute("LSP from a remote node");
                    } else if (known != db.end() && seq == known->second.seq) {
                        known->second.heard = Clock::now(); // still alive
                    }
                }
            }
        }

        // ---- periodic work
        auto now = Clock::now();

        if (self != 0 && now - last_advert >= kLspPeriod) {
            last_advert = now;
            auto it = db.find(self);
            if (it != db.end()) {
                it->second.seq = ++own_seq;
                it->second.heard = now;
                flood(udp_fd, self, own_seq, it->second.links, neighbours);
            }
        }

        // A node whose LSP has not been refreshed is gone, and so is everything
        // only reachable through it. This is what turns a silent link failure
        // into a routing change.
        std::vector<char> expired;
        for (const auto &[origin, ls] : db)
            if (origin != self && now - ls.heard > kLspTimeout)
                expired.push_back(origin);
        if (!expired.empty()) {
            for (char origin : expired) {
                std::cout << "[" << self << "] " << origin
                          << " timed out, dropping its link state\n";
                db.erase(origin);
            }
            recompute("neighbour timeout");
        }

        if (self != 0 && now - last_report >= kReportPeriod) {
            last_report = now;
            print_topology(db);
            print_routes(self, routes);
        }
    }

    close(tcp_fd);
    close(udp_fd);
    return 0;
}
