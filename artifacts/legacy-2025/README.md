# Historical measurements (2025)

The four neighboring text files are preserved verbatim from repository commit e9540ba4f49701e8f42460a39c9961d13bd19dd6. No number has been replaced or retroactively assigned to the revised implementation.

| Recorded metric | wrk.base.txt | wrk.after.txt |
|---|---:|---:|
| Requests/sec | 3436.83 | 37739.84 |
| p50 latency | 74.80 ms | 2.99 ms |
| p99 latency | 113.04 ms | 12.06 ms |

The arithmetic throughput ratio is about 10.98. A new [same-machine reproduction](../../docs/PERFORMANCE.md) evaluates the recovered close/keep-alive behavior with repeated measurements. The historical machine and exact original binary are not available, so its absolute numbers remain historical observations rather than the result of the current experiment.

Missing evidence includes the CPU/RAM/distro/kernel/compiler, wrk identity, CPU placement, original command including --latency, module parameters, mapping of each run to an exact buildable baseline/optimized source revision, repeated trials, and correctness validation. The checked-in source at the audit snapshot was truncated and cannot reproduce these records as-is. This does not establish that the logs were fabricated; it establishes that their provenance cannot be recovered from the available files.

perf.after.report.txt contains report metadata with no symbol rows or call graph. A sample count and zero lost samples do not identify a bottleneck or establish a valid optimization mechanism.

bpf.after.txt came from a script that counted attempted tcp_sendmsg argument sizes system-wide, cleared counters each interval, and had no flow/process filter. It cannot establish khttpd-specific delivered bytes or clean end-to-end service.

For new before/after measurements use [the reconstruction protocol](../../docs/PERFORMANCE_METHOD.md). Keep the old and new environment-specific observations distinct; do not compute a speedup by mixing their absolute rates.
