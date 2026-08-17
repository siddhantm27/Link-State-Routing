"""Oracle Node.

The ON is the controller of the emulator. It owns the ground-truth topology,
hands each virtual node its identity, and tells every node which neighbours it
can reach and at what cost. It never participates in routing itself: the VNs
discover paths on their own by flooding link state (see vn.cpp).

Wire protocol, all integers network byte order:

    CONNECT     VN  -> ON   6 bytes    4 IP | 2 UDP port
    LINK-STATE  ON  -> VN   11n bytes  per record: 1 letter | 4 IP | 2 port | 4 cost

A LINK-STATE record with cost 0 is the recipient's own identity; every other
record is a live neighbour. Sending a LINK-STATE with a neighbour removed is
how the ON signals a link failure, so the same message carries both the initial
topology and every later change.
"""

import os
import select
import socket
import sys
import time

LISTEN_PORT = 5000
CONNECT_LEN = 6
RECORD_LEN = 11
NO_LINK = -1

# A text editor writing a config file can produce several mtime bumps in quick
# succession. Wait for the file to stop changing before acting on it.
CONFIG_SETTLE_SECS = 2.0


def get_local_ip():
    """Best-effort primary interface address, without needing a route to exist."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def read_config(path):
    """Parse the upper-triangular cost matrix.

    Row i holds the costs from node i to nodes i+1 .. n-1, so the file has n-1
    rows for n nodes. -1 means the two nodes are not directly connected.
    """
    rows = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            try:
                rows.append([int(tok) for tok in line.split()])
            except ValueError as exc:
                raise ValueError(f"{path}:{lineno}: {exc}") from None

    for i, row in enumerate(rows):
        want = len(rows) - i
        if len(row) != want:
            raise ValueError(
                f"{path}: row {i + 1} has {len(row)} costs, expected {want} "
                "(the matrix must be upper triangular)"
            )
    return rows


def build_adj(rows):
    """Expand the triangular rows into a full symmetric adjacency matrix."""
    n = len(rows) + 1 if rows else 0
    adj = [[NO_LINK] * n for _ in range(n)]
    for i, row in enumerate(rows):
        for offset, cost in enumerate(row):
            j = i + offset + 1
            if cost != NO_LINK:
                adj[i][j] = adj[j][i] = cost
    return adj


def letter(index):
    return chr(ord("A") + index)


class VirtualNode:
    def __init__(self, index, ip, udp_port, sock):
        self.index = index
        self.letter = letter(index)
        self.ip = ip
        self.udp_port = udp_port
        self.sock = sock

    def __repr__(self):
        return f"{self.letter}@{self.ip}:{self.udp_port}"


class OracleNode:
    def __init__(self, config_path, ip, port=LISTEN_PORT):
        self.config_path = config_path
        self.ip = ip
        self.port = port

        self.adj = build_adj(read_config(config_path))
        self.num_nodes = len(self.adj)

        self.nodes = {}          # connected socket -> VirtualNode
        self.by_index = {}       # index -> VirtualNode
        self.next_index = 0

        self.config_mtime = os.stat(config_path).st_mtime
        self.pending_change_at = None

        self.listener = None

    # ---------------------------------------------------------------- startup

    def serve(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.setblocking(False)
        self.listener.bind((self.ip, self.port))
        self.listener.listen()
        print(f"[on] listening on {self.ip}:{self.port} for {self.num_nodes} nodes")

        try:
            self._loop()
        except KeyboardInterrupt:
            print("\n[on] shutting down")
        finally:
            for sock in list(self.nodes):
                sock.close()
            self.listener.close()

    # ------------------------------------------------------------- event loop

    def _loop(self):
        while True:
            watched = [self.listener, *self.nodes]
            readable, _, errored = select.select(watched, [], watched, 1.0)

            for sock in readable:
                if sock is self.listener:
                    self._accept()
                else:
                    self._on_readable(sock)

            for sock in errored:
                self._drop(sock, "socket error")

            self._poll_config()

    def _accept(self):
        try:
            conn, addr = self.listener.accept()
        except BlockingIOError:
            return
        conn.setblocking(False)
        self.nodes[conn] = None
        print(f"[on] connection from {addr[0]}:{addr[1]}")

    def _on_readable(self, sock):
        try:
            data = sock.recv(CONNECT_LEN)
        except (BlockingIOError, InterruptedError):
            return
        except OSError as exc:
            self._drop(sock, str(exc))
            return

        if not data:
            self._drop(sock, "peer closed")
            return
        if self.nodes[sock] is not None:
            # Registered nodes are not expected to speak again.
            return
        if len(data) != CONNECT_LEN:
            print(f"[on] malformed CONNECT ({len(data)} bytes), dropping")
            self._drop(sock, "bad CONNECT")
            return

        self._register(sock, data)

    def _register(self, sock, payload):
        ip = socket.inet_ntoa(payload[:4])
        udp_port = int.from_bytes(payload[4:6], "big")

        if self.next_index >= self.num_nodes:
            # More nodes than the topology describes. Such a node is reachable
            # by nobody, so tell it that and leave it isolated rather than
            # renumbering the nodes that are already wired up.
            stray = VirtualNode(self.next_index, ip, udp_port, sock)
            self.next_index += 1
            self.nodes[sock] = stray
            print(f"[on] {stray} exceeds the topology, marking it isolated")
            self._send_link_state(stray)
            return

        node = VirtualNode(self.next_index, ip, udp_port, sock)
        self.next_index += 1
        self.nodes[sock] = node
        self.by_index[node.index] = node
        print(f"[on] assigned {node.letter} to {ip}:{udp_port}")

        if len(self.by_index) == self.num_nodes:
            print("[on] all nodes present, distributing link state")
            self._broadcast()

    def _drop(self, sock, reason):
        node = self.nodes.pop(sock, None)
        sock.close()
        if node is None:
            print(f"[on] unregistered connection closed ({reason})")
            return

        self.by_index.pop(node.index, None)
        print(f"[on] {node.letter} disconnected ({reason})")
        # Every link incident on the departed node is now down. Re-sending link
        # state to its former neighbours is what lets them notice.
        self._broadcast()

    # ----------------------------------------------------------- link state

    def _neighbours_of(self, node):
        """Live neighbours of `node`: adjacent in the config and connected."""
        out = []
        if node.index >= self.num_nodes:
            return out
        for j, cost in enumerate(self.adj[node.index]):
            if cost == NO_LINK or j == node.index:
                continue
            peer = self.by_index.get(j)
            if peer is not None:
                out.append((peer.letter, peer.ip, peer.udp_port, cost))
        return out

    def _send_link_state(self, node):
        records = [(node.letter, node.ip, node.udp_port, 0)]
        records.extend(self._neighbours_of(node))
        records.sort(key=lambda r: r[0])

        msg = b"".join(
            bytes([ord(char)])
            + socket.inet_aton(ip)
            + port.to_bytes(2, "big")
            + cost.to_bytes(4, "big")
            for char, ip, port, cost in records
        )
        try:
            node.sock.sendall(msg)
        except OSError as exc:
            print(f"[on] failed to reach {node.letter}: {exc}")
            return
        peers = ",".join(r[0] for r in records[1:]) or "none"
        print(f"[on] -> {node.letter}: neighbours {peers}")

    def _broadcast(self):
        for node in sorted(self.by_index.values(), key=lambda n: n.index):
            self._send_link_state(node)

    # --------------------------------------------------------- config reload

    def _poll_config(self):
        try:
            mtime = os.stat(self.config_path).st_mtime
        except OSError:
            return

        if mtime == self.config_mtime:
            self.pending_change_at = None
            return

        now = time.monotonic()
        if self.pending_change_at is None:
            self.pending_change_at = now
            return
        if now - self.pending_change_at < CONFIG_SETTLE_SECS:
            return

        self.pending_change_at = None
        self.config_mtime = mtime
        try:
            adj = build_adj(read_config(self.config_path))
        except (OSError, ValueError) as exc:
            print(f"[on] keeping previous topology, reload failed: {exc}")
            return

        if len(adj) != self.num_nodes:
            print(
                f"[on] refusing reload: node count changed "
                f"{self.num_nodes} -> {len(adj)}; restart the emulator instead"
            )
            return

        if adj == self.adj:
            return

        self.adj = adj
        print("[on] topology changed, redistributing link state")
        self._broadcast()


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <config file>", file=sys.stderr)
        return 1
    path = sys.argv[1]
    try:
        OracleNode(path, get_local_ip()).serve()
    except (OSError, ValueError) as exc:
        print(f"[on] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
