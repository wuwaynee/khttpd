#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
pid=
cleanup() {
    if [[ -n "$pid" ]]; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" || true
    fi
}
trap cleanup EXIT
for config in "1 1" "2 0"; do
    read -r shards prealloc <<< "$config"
    : > build/reference-test.log
    ./build/reference_server 18081 32 "$shards" 1 "$prealloc" 2000 1024 \
        > build/reference-test.log 2>&1 &
    pid=$!
    ready=0
    for ((i=0; i<100; i++)); do
        if rg -q '^ready ' build/reference-test.log; then ready=1; break; fi
        kill -0 "$pid"
        sleep 0.05
    done
    [[ "$ready" == 1 ]]
    ./build/integration 18081
    ./build/integration 18081 hold > build/hold.log &
    holder=$!
    ready=0
    for ((i=0; i<100; i++)); do
        if rg -q HOLD_READY build/hold.log; then ready=1; break; fi
        sleep 0.05
    done
    [[ "$ready" == 1 ]]
    kill -TERM "$pid"
    wait "$pid"
    pid=
    wait "$holder"
done
