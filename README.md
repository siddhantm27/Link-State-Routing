# Distributed Link-State Routing

A small network emulator. A central **Oracle Node** owns the real topology and
tells each **Virtual Node** only who its immediate neighbours are; the virtual
nodes then discover the rest of the network themselves by flooding link state,
and compute shortest paths from what they learn.

The split matters: no node is ever handed the global graph. Everything beyond
one hop is inferred from flooded advertisements, which is what makes it a
link-state protocol rather than a lookup.

```
        ┌──────────────┐   TCP 5000: identity + neighbours
        │ Oracle Node  │◄──────────────┬───────────────┐
        │   (on.py)    │               │               │
        └──────────────┘               │               │
                                  ┌────┴───┐      ┌────┴───┐
                                  │  VN A  │◄────►│  VN B  │
                                  │(vn.cpp)│ UDP  │(vn.cpp)│
                                  └────────┘ LSPs └────────┘
```

## Build and run

```bash
make
```

Start the controller with a topology, then one virtual node per node in it:

```bash
python3 on.py topologies/diamond.txt
./vn <ON_IP> <own_IP> 6000
./vn <ON_IP> <own_IP> 6001
./vn <ON_IP> <own_IP> 6002
./vn <ON_IP> <own_IP> 6003
```

`run_demo.sh` does all of that on one machine, waits for convergence, then cuts
a link and prints the routing tables before and after:

```bash
./run_demo.sh topologies/diamond.txt
```

## Topology files

An upper-triangular cost matrix. Row *i* lists the cost from node *i* to nodes
*i+1 … n-1*, so *n* nodes need *n-1* rows. `-1` means no direct link. Nodes are
named `A`, `B`, `C` … in the order they connect.

```
#     B   C   D
1   4  -1     # A: A-B costs 1, A-C costs 4, no A-D link
-1   2        # B: no B-C link, B-D costs 2
1             # C: C-D costs 1
```

Three are included: `line.txt` (forced paths, clearest for watching a flood
propagate), `triangle.txt` (fully connected), and `diamond.txt` (two routes of
different cost, so cutting one visibly reroutes).

## Protocol

All integers are network byte order.

| Message | Direction | Layout |
| --- | --- | --- |
| `CONNECT` | VN → ON | 6 bytes: 4 IP, 2 UDP port |
| `LINK-STATE` | ON → VN | 11 bytes per record: 1 letter, 4 IP, 2 port, 4 cost |
| `LSP` | VN → VN | 1 origin, 4 sequence, 1 count, then per link: 1 letter, 4 cost |

A `LINK-STATE` record with cost 0 is the recipient's own identity; the others
are its live neighbours, with the addresses needed to reach them over UDP.

## How it works

**Identity and neighbours.** Each VN opens a TCP connection and sends its UDP
endpoint. The ON assigns it the next free letter and, once every node has
appeared, sends each one its neighbour list. That list is the only thing a node
is ever told directly.

**Flooding.** A node advertises its own adjacencies in an LSP carrying a
monotonic sequence number. On receiving one, a node accepts it only if the
sequence is newer than what it holds, then forwards it to every neighbour except
the sender and the originator. The sequence check is what terminates the flood;
without it the packet would circulate forever around any cycle. Each LSP is sent
several times because UDP will drop some.

**Shortest paths.** Dijkstra over the accumulated database, recomputed whenever
the topology actually changes rather than on every packet. The result is a
routing table of destination, total cost, and the *first* neighbour on the path
— the next hop is what a node needs, not the full route.

One subtlety: the database is assembled from advertisements that need not agree,
since a node may still be advertising a link whose far end has already withdrawn
it. A link is only usable when **both** endpoints advertise it, which keeps a
half-torn-down link out of the routing table. Where the two ends disagree on
cost, the higher is used, so a path never looks better than it is.

**Failure.** Two paths, because failures arrive two ways. A node that disappears
takes its TCP connection with it, and the ON notices and re-sends link state to
everyone still connected. A link that goes away silently is caught by aging:
LSPs are refreshed every 10 seconds and expire after 35, so an advertisement
that stops being renewed is dropped along with everything reachable only through
it. The timeout clears several refresh periods deliberately — tearing down a
path because one datagram went missing would be worse than converging slowly.

**Reconfiguration.** The ON watches its config file and reloads on change,
waiting for writes to settle first so a mid-save file is never parsed. A reload
that fails to parse, or that changes the node count, is refused and the previous
topology kept. Otherwise the new neighbour lists go out and the nodes reflood
and recompute on their own.

Both programs are single-threaded around `select()` with non-blocking sockets,
so a slow or dead peer never stalls the others.
