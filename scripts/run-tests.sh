#!/bin/bash
set -e
cd /workspace/build
echo "=== Starting server ==="
./server/sync_server 50051 &
SERVER_PID=$!
sleep 2
echo "=== Running bond request test ==="
./examples/bond_request 10
kill $SERVER_PID 2>/dev/null || true
echo "=== Tests passed! ==="
