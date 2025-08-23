#!/usr/bin/env bash
set -euo pipefail

OUTDIR="${1:-artifacts}"
DUR="${2:-30s}"
FREQ="${3:-400}"

mkdir -p "$OUTDIR"

echo "[i] warming sudo cache..."
sudo -v

echo "[i] start perf (system-wide)..."
sudo -n perf record -a -g -F "$FREQ" -- sleep 35 &
PERFJOB=$!

echo "[i] start bpftrace counters..."
sudo -n timeout 40s bpftrace -o "$OUTDIR/bpf.after.txt" ./tools/tcp_counters.bt &
BPFJOB=$!

sleep 2
echo "[i] run wrk..."
wrk -t4 -c256 -d"$DUR" --latency http://127.0.0.1:8081/ | tee "$OUTDIR/wrk.after.txt"

echo "[i] wait jobs..."
wait $PERFJOB || true
wait $BPFJOB  || true

echo "[i] perf report..."
sudo perf report --comms khttpd --stdio | sed -n '1,200p' > "$OUTDIR/perf.after.report.txt"

echo "[i] done; artifacts in $OUTDIR"
