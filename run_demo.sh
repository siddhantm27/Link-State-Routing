#!/bin/bash
# Brings up the Oracle Node and four Virtual Nodes on one machine, lets the
# flood converge, then cuts a link and shows the routing tables change.
set -u
TOPO=${1:-topologies/diamond.txt}
IP=$(python3 -c "import on; print(on.get_local_ip())")
WORK=$(mktemp -d)
cp "$TOPO" "$WORK/topo.txt"

cleanup() { kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

python3 on.py "$WORK/topo.txt" > "$WORK/on.log" 2>&1 &
sleep 1
for i in 0 1 2 3; do
    ./vn "$IP" "$IP" $((6000 + i)) > "$WORK/vn$i.log" 2>&1 &
    sleep 0.3
done

echo "converging..."; sleep 12
echo "=== routing tables after convergence ==="
for i in 0 1 2 3; do
    echo "--- node $i ---"
    awk '/routing table/{f=1} f' "$WORK/vn$i.log" | tail -8
done

echo
echo "=== cutting link A-B ==="
sed -i 's/^1   4  -1/-1   4  -1/' "$WORK/topo.txt"
sleep 14
echo "=== routing tables after the cut ==="
for i in 0 1 2 3; do
    echo "--- node $i ---"
    awk '/routing table/{f=1;buf=""} f{buf=buf"\n"$0} END{print buf}' "$WORK/vn$i.log" | tail -8
done
echo
echo "logs in $WORK"
