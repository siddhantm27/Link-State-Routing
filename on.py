import sys
import os
import socket
import select
import time


def get_local_ip():
    ip = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
    except Exception:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip


def read_config(config_file):
    costs = []
    with open(config_file, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("#") or not line:
                continue
            parts = line.split()
            costs.append([int(p) for p in parts])
    return costs


def build_adj(costs):
    n = len(costs) + 1 if costs else 0
    adj = [[-1] * n for _ in range(n)]
    for i, row in enumerate(costs):
        for j, cost in enumerate(row):
            if cost != -1:
                adj[i][j + i + 1] = cost
                adj[j + i + 1][i] = cost
    return adj


class VirtualNode:
    def __init__(self, node_alphabet, ip_addr, udp_port, sock):
        self.node_alphabet = node_alphabet
        self.ip_addr = ip_addr
        self.udp_port = udp_port
        self.sock = sock


class OracleNode:
    def __init__(self, config_file, ip_addr, port=5000):
        self.config_file = config_file
        self.ip_addr = ip_addr
        self.port = port
        self.vns = []
        self.s = socket.socket()
        self.last_modified_time = os.stat(config_file).st_mtime
        costs = read_config(config_file)
        self.adj = build_adj(costs)
        self.num_nodes = len(costs) + 1 if costs else 0

    def init_socket(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.bind((self.ip_addr, self.port))
        self.s.listen()
        print(f"OracleNode listening on {self.ip_addr}:{self.port}")

    def handle_connections(self):
        sockets = [self.s]
        while True:
            readable, _, _ = select.select(sockets, [], [], 1.0)
            if readable:
                for r in readable:
                    if r is self.s:
                        conn, addr = self.s.accept()
                        print(f"Connection from {addr}")
                        sockets.append(conn)
                    else:
                        data = r.recv(1024)
                        if data:
                            self.process_data(data, r)
                        else:
                            print("Closing connection")
                            sockets.remove(r)
                            r.close()
            else:
                modified_time = os.stat(self.config_file).st_mtime
                if modified_time != self.last_modified_time:
                    # first detection
                    if not self.last_seen_change:
                        self.last_seen_change = time.time()
                    # check if stable for 5 seconds
                    elif time.time() - self.last_seen_change > 5:
                        print("Config file changed, reloading")
                        self.last_modified_time = modified_time
                        self.last_seen_change = None
                        costs = read_config(self.config_file)
                        self.adj = build_adj(costs)
                        self.num_nodes = len(costs) + 1 if costs else 0
                        for vn in self.vns:
                            self.send_link_state_to(vn)
                else:
                    # reset if no ongoing change
                    self.last_seen_change = None

    def process_data(self, data: bytes, sock):
        if len(data) != 6:
            print("CONNECT message should be of length 6")
            return
        ip = socket.inet_ntoa(data[:4])
        port = int.from_bytes(data[4:6], "big")
        alphabet = chr(ord("A") + len(self.vns))
        vn = VirtualNode(alphabet, ip, port, sock)
        self.vns.append(vn)
        print(f"Assigned {alphabet} to {ip}:{port}")
        if len(self.vns) == self.num_nodes:
            for v in self.vns[: self.num_nodes]:
                self.send_link_state_to(v)
        elif len(self.vns) > self.num_nodes:
            print(f"Disconnected island {alphabet}")
            self.send_link_state_to(vn)

    def send_link_state_to(self, vn: VirtualNode):
        i = ord(vn.node_alphabet) - ord("A")
        msg = b""
        neigh_list = []
        if i < self.num_nodes:
            for j in range(self.num_nodes):
                cost = self.adj[i][j]
                if cost != -1 and j != i:
                    neigh_vn = self.vns[j]
                    neigh_char = chr(ord("A") + j)
                    neigh_list.append(
                        (neigh_char, neigh_vn.ip_addr, neigh_vn.udp_port, cost)
                    )
        self_tuple = (vn.node_alphabet, vn.ip_addr, vn.udp_port, 0)
        all_tuples = [self_tuple] + neigh_list
        all_tuples.sort(key=lambda x: x[0])
        for char, ip, port, cost in all_tuples:
            msg += (
                bytes([ord(char)])
                + socket.inet_aton(ip)
                + port.to_bytes(2, "big")
                + cost.to_bytes(4, "big")
            )
        vn.sock.send(msg)
        print(f"Sent LINK-STATE to {vn.node_alphabet}")


def main():
    if len(sys.argv) < 2:
        print("usage: python3 on.py [config_file]")
        return
    config_file = sys.argv[1]
    # check if config_file exists
    if not os.path.exists(config_file):
        print(f"{config_file} does not exist", file=sys.stderr)
        return
    on = OracleNode(config_file, get_local_ip())
    on.init_socket()
    on.handle_connections()


if __name__ == "__main__":
    main()
