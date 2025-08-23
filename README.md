# khttpd (kernel HTTP server)
- **Features**: HTTP/1.1 keep-alive (persistent connections), SO_REUSEPORT (multi-acceptors), per-CPU listener threads.
- **Benchmark**: wrk shows ~30–38K RPS on localhost; p50 latency ~3 ms; p99 ~12–16 ms.
- **Profiling**: perf (system-wide + comm filter), ftrace (optional), eBPF counters (bpftrace).
- **How to build**: `make`
- **How to run**: `sudo insmod khttpd.ko port=8081 backlog=512 workers=$(nproc)`
- **How to test**: `wrk -t4 -c256 -d30s --latency http://127.0.0.1:8081/`
- **How to collect artifacts**: `./scripts/bench_all.sh artifacts 30s 400`
