#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <set>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace std;

const int LSP_PERIOD = 10;
const int PRINT_PERIOD = 30;

void send_lsp(int udp_fd, char origin, uint32_t seq,
              const map<char, int> &links,
              const map<char, tuple<string, uint16_t, int>> &flood_to) {
    char buf[1024];
    buf[0] = origin;
    uint32_t seq_net = htonl(seq);
    memcpy(buf + 1, &seq_net, 4);
    uint8_t num = links.size();
    buf[5] = num;
    int pos = 6;
    vector<char> link_keys;
    for (const auto &p : links)
        link_keys.push_back(p.first);
    sort(link_keys.begin(), link_keys.end());
    for (char link_key : link_keys) {
        buf[pos] = link_key;
        pos++;
        uint32_t cost_net = htonl(links.at(link_key));
        memcpy(buf + pos, &cost_net, 4);
        pos += 4;
    }
    for (const auto &p : flood_to) {
        char neigh = p.first;
        int cost = get<2>(p.second);
        if (cost == 0)
            continue;
        string ip = get<0>(p.second);
        uint16_t port = get<1>(p.second);
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &dest.sin_addr);
        for (int times = 0; times < 3; ++times)
            sendto(udp_fd, buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
    }
}

void print_graph(const map<char, pair<uint32_t, map<char, int>>> &lsps) {
    set<char> all_nodes;
    for (const auto &p : lsps) {
        all_nodes.insert(p.first);
        for (const auto &q : p.second.second)
            all_nodes.insert(q.first);
    }
    vector<char> sorted_nodes(all_nodes.begin(), all_nodes.end());
    map<char, int> node_idx;
    for (size_t i = 0; i < sorted_nodes.size(); ++i)
        node_idx[sorted_nodes[i]] = i;
    int m = sorted_nodes.size();
    vector<vector<int>> adj_matrix(m, vector<int>(m, -1));
    for (const auto &p : lsps) {
        int src = node_idx[p.first];
        for (const auto &q : p.second.second) {
            int dst = node_idx[q.first];
            adj_matrix[src][dst] = q.second;
            adj_matrix[dst][src] = q.second;
        }
    }
    cout<< "Current network topology:" << endl;
    for (int i = 0; i < m; ++i) {
        string line;
        for (int k = 0; k < i; ++k)
            line += " ";
        for (int j = i + 1; j < m; ++j)
            line += to_string(adj_matrix[i][j]) + " ";
        cout << line << endl;
    }
    cout << "---------------------------------" << endl;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        cerr << "Usage: " << argv[0] << " <ON_IP> <VN_IP> <VN_UDP_port>"
             << endl;
        return 1;
    }

    const char *on_ip = argv[1];
    const char *vn_ip_str = argv[2];
    uint16_t vn_udp_port = stoi(argv[3]);

    // TCP socket
    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        cerr << "tcp socket error" << endl;
        return 1;
    }
    struct sockaddr_in on_addr;
    memset(&on_addr, 0, sizeof(on_addr));
    on_addr.sin_family = AF_INET;
    on_addr.sin_port = htons(5000);
    if (inet_pton(AF_INET, on_ip, &on_addr.sin_addr) <= 0) {
        cerr << "Invalid ON IP" << endl;
        return 1;
    }
    if (connect(tcp_fd, (struct sockaddr *)&on_addr, sizeof(on_addr)) < 0) {
        cerr << "connect" << endl;
        close(tcp_fd);
        return 1;
    }

    // Send CONNECT
    struct in_addr vn_ip;
    if (inet_pton(AF_INET, vn_ip_str, &vn_ip) <= 0) {
        cerr << "Invalid own IP" << endl;
        return 1;
    }
    uint16_t vn_port_net = htons(vn_udp_port);
    char connect_msg[6];
    memcpy(connect_msg, &vn_ip.s_addr, 4);
    memcpy(connect_msg + 4, &vn_port_net, 2);
    if (send(tcp_fd, connect_msg, 6, 0) != 6) {
        cerr << "send connect" << endl;
        return 1;
    }

    // State
    char vn_alphabet = 0;
    map<char, tuple<string, uint16_t, int>> neighbors;
    map<char, pair<uint32_t, map<char, int>>> lsps;
    uint32_t lsp_seq = 0;
    int lsp_timer = 0;
    int print_timer = 0;

    // UDP socket
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("udp socket");
        return 1;
    }
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(vn_udp_port);
    if (bind(udp_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        cerr << "bind udp" << endl;
        close(udp_fd);
        return 1;
    }

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(tcp_fd, &readfds);
        FD_SET(udp_fd, &readfds);
        int nfds = max(tcp_fd, udp_fd) + 1;
        struct timeval timeout = {1, 0};
        int ret = select(nfds, &readfds, nullptr, nullptr, &timeout);
        if (ret < 0) {
            perror("select");
            break;
        } else if (ret == 0) {
            // timeout
            lsp_timer++;
            print_timer++;
            if (lsp_timer >= LSP_PERIOD && vn_alphabet != 0) {
                lsp_timer = 0;
                auto &own_links = lsps[vn_alphabet].second;
                send_lsp(udp_fd, vn_alphabet, lsp_seq, own_links, neighbors);
            }
            if (print_timer >= PRINT_PERIOD && vn_alphabet != 0) {
                print_timer = 0;
                print_graph(lsps);
            }
            continue;
        }

        if (FD_ISSET(tcp_fd, &readfds)) {
            char buffer[1024];
            ssize_t bytes = recv(tcp_fd, buffer, sizeof(buffer), 0);
            if (bytes == 0) {
                cout << "ON closed" << endl;
                break;
            }
            if (bytes % 11 != 0) {
                cerr << "Invalid LINK-STATE length" << endl;
                continue;
            }
            map<char, tuple<string, uint16_t, int>> new_neigh_info;
            char new_vn_alpha = 0;
            for (ssize_t i = 0; i < bytes; i += 11) {
                char alpha = buffer[i];
                struct in_addr ip;
                memcpy(&ip.s_addr, buffer + i + 1, 4);
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
                uint16_t port_net;
                memcpy(&port_net, buffer + i + 5, 2);
                uint16_t port = ntohs(port_net);
                uint32_t cost_net;
                memcpy(&cost_net, buffer + i + 7, 4);
                int cost = ntohl(cost_net);
                new_neigh_info[alpha] = make_tuple(string(ip_str), port, cost);
                if (cost == 0)
                    new_vn_alpha = alpha;
            }
            bool changed = (neighbors != new_neigh_info);
            neighbors = new_neigh_info;
            vn_alphabet = new_vn_alpha;

            string neigh_info_str;
            vector<char> neigh_alpha;
            for (const auto &p : neighbors) {
                int cost = get<2>(p.second);
                if (cost == 0) {
                    neigh_info_str += p.first;
                    neigh_info_str += "=0,";
                } else
                    neigh_alpha.push_back(p.first);
            }
            sort(neigh_alpha.begin(), neigh_alpha.end());
            for (char k : neigh_alpha) {
                neigh_info_str += k;
                neigh_info_str += "=" + to_string(get<2>(neighbors[k])) + ",";
            }
            if (!neigh_info_str.empty())
                neigh_info_str.pop_back(); // remove comma
            cout << neigh_info_str << endl;
            if (changed) {
                map<char, int> links;
                for (const auto &p : neighbors) {
                    int cost = get<2>(p.second);
                    if (cost != 0)
                        links[p.first] = cost;
                }
                lsps[vn_alphabet] = make_pair(++lsp_seq, links);
                send_lsp(udp_fd, vn_alphabet, lsp_seq, links, neighbors);
            }
        }

        if (FD_ISSET(udp_fd, &readfds)) {
            char buffer[1024];
            struct sockaddr_in src_addr;
            socklen_t len = sizeof(src_addr);
            ssize_t bytes = recvfrom(udp_fd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr *)&src_addr, &len);
            if (bytes < 6)
                continue;
            char origin = buffer[0];
            uint32_t seq_net;
            memcpy(&seq_net, buffer + 1, 4);
            uint32_t seq = ntohl(seq_net);
            uint8_t origin_neigh_cnt = buffer[5];
            if (6 + origin_neigh_cnt * 5 != static_cast<size_t>(bytes)) {
                cerr << "Invalid LSP length" << endl;
                continue;
            }
            map<char, int> origin_links;
            for (uint8_t k = 0; k < origin_neigh_cnt; ++k) {
                int off = 6 + k * 5;
                char neigh = buffer[off];
                uint32_t cost_net;
                memcpy(&cost_net, buffer + off + 1, 4);
                uint32_t cost = ntohl(cost_net);
                origin_links[neigh] = cost;
            }
            auto it = lsps.find(origin);
            if (it == lsps.end() || seq > it->second.first) {
                lsps[origin] = make_pair(seq, origin_links);
                char sender = 0;
                char src_ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &src_addr.sin_addr, src_ip_str,
                          sizeof(src_ip_str));
                uint16_t src_port = ntohs(src_addr.sin_port);
                for (const auto &p : neighbors) {
                    if (get<2>(p.second) != 0 &&
                        get<0>(p.second) == src_ip_str &&
                        get<1>(p.second) == src_port) {
                        sender = p.first;
                        break;
                    }
                }
                if (sender == 0)
                    continue;
                map<char, tuple<string, uint16_t, int>> flood_to = neighbors;
                flood_to.erase(sender);
                flood_to.erase(origin);
                flood_to.erase(vn_alphabet); // self
                send_lsp(udp_fd, origin, seq, origin_links, flood_to);
            }
        }
    }

    close(tcp_fd);
    close(udp_fd);
    return 0;
}
