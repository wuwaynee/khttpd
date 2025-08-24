# Bench Notes

**Host:** <CPU / RAM / distro / kernel version>  
**Module:** khttpd.ko (built with <gcc/clang version>)  
**Loadgen:** wrk <version>

---

## Commands

```bash
# Load module (default port 8081)
sudo insmod khttpd.ko port=8081

# Benchmark (example)
wrk -t4 -c256 -d30s http://127.0.0.1:8081/

# Kernel log (optional)
dmesg -w

# Optional TCP counters while serving
sudo bpftrace tools/tcp_counters.bt

# Unload
sudo rmmod khttpd
```

---

## Representative Results

### Before (baseline)
Source: `artifacts/wrk.base.txt`
- Threads: 4, Connections: 256, Duration: 30s
- Requests/sec: **3436.83**
- Latency (p50/p75/p90/p99): **74.80ms / 80.92ms / 86.60ms / 113.04ms**
- Throughput: **375.90 KB/s**
- Total: 103436 requests, 11.05 MB read

### After (optimized)
Source: `artifacts/wrk.after.txt`
- Threads: 4, Connections: 256, Duration: 30s
- Requests/sec: **37739.84**
- Latency (p50/p75/p90/p99): **2.99ms / 4.31ms / 6.08ms / 12.06ms**
- Throughput: **4.21 MB/s**
- Total: 1134637 requests, 126.60 MB read

### Delta (after vs before)
- Requests/sec: **~11.0**× (37739.84 / 3436.83)
- Latency speedup: **p50 ~25.0**×, **p75 ~18.8**×, **p90 ~14.2**×, **p99 ~9.37**×
> Note: `wrk` reports p50/p75/p90/p99 by default (no p95). The above values are taken directly from the logs.

---

## Perf / BPF Notes

### perf
Source: `artifacts/perf.after.report.txt`
- `perf record` event: `cpu-clock:ppp`, **50K samples, 0 lost samples**
- Use `perf report` to drill down hot symbols, consider `-g` for callgraphs and `--no-children` for leaf views.

### bpftrace
Source: `artifacts/bpf.after.txt`
- Sample output shows retransmits `retrans=0` while bytes climb (e.g., `@bytes[0]: 24277095`), indicating clean steady serving without TCP retransmissions under this load.

---

## How to Reproduce
1. Build the module, load with `port=8081`.
2. Run the exact `wrk` command shown above for 30s with 4 threads / 256 connections.
3. Capture raw logs into `artifacts/`:
    - `wrk.base.txt`, `wrk.after.txt`
    - Optional: `perf.data` + `perf.after.report.txt`
    - Optional: `bpf.after.txt` from `sudo bpftrace tools/tcp_counters.bt`
4. Summarize here and update the Delta section when results change.