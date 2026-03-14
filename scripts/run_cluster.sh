#!/usr/bin/env bash
# scripts/run_cluster.sh
# Starts a 3-node CoDB cluster locally for testing (Phase 3+)

set -e

BINARY_DIR="${1:-./build}"
DATA_DIR="./data"

mkdir -p "$DATA_DIR/node1" "$DATA_DIR/node2" "$DATA_DIR/node3"

echo "[codb] Starting 3-node cluster..."

# Node 1 — Leader
"$BINARY_DIR/codb_server" \
  --id=node1 \
  --port=50051 \
  --leader \
  --data-dir="$DATA_DIR/node1" \
  --peers=localhost:50052,localhost:50053 &
NODE1_PID=$!

sleep 0.5

# Node 2 — Follower
"$BINARY_DIR/codb_server" \
  --id=node2 \
  --port=50052 \
  --data-dir="$DATA_DIR/node2" \
  --leader-addr=localhost:50051 &
NODE2_PID=$!

# Node 3 — Follower
"$BINARY_DIR/codb_server" \
  --id=node3 \
  --port=50053 \
  --data-dir="$DATA_DIR/node3" \
  --leader-addr=localhost:50051 &
NODE3_PID=$!

echo "[codb] Cluster running:"
echo "  node1 (leader): localhost:50051  PID=$NODE1_PID"
echo "  node2:          localhost:50052  PID=$NODE2_PID"
echo "  node3:          localhost:50053  PID=$NODE3_PID"
echo ""
echo "Press Ctrl+C to stop all nodes."

trap "kill $NODE1_PID $NODE2_PID $NODE3_PID 2>/dev/null; echo 'Cluster stopped.'" EXIT
wait
