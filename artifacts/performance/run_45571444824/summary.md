# Matched performance measurements

Source commit: 0bfbe252a283102e5638b9004d5ece366e40a2f9
Kernel: Linux ip-10-0-129-120 7.0.0-1014-aws #14~24.04.1-Ubuntu SMP PREEMPT Fri Sep 18 21:24:59 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

Each cell: 5 runs, 30 seconds, wrk 4T/256C.
Primary measurement is plain wrk; a separate same-load phase validates every response.
Values below are medians across runs, not pooled percentiles.

| Profile | req/s | p50 ms | p99 ms | socket error events | min–max req/s |
|---|---:|---:|---:|---:|---:|
| legacy_optimized4 | 348935.93 | 0.332 | 1.020 | 0 | 346230.72–401914.49 |
| hardened4 | 356433.84 | 0.319 | 1.430 | 0 | 344017.39–394552.65 |
| matched_close512 | 14080.95 | 12.050 | 93.400 | 0 | 14027.20–14119.90 |

Ratios use the matched_close512 control in the same repetition.

- legacy_optimized4: median paired throughput 24.88x (range 24.61–28.46x); p50 reduction 97.2%; p99 reduction 98.9%.
- hardened4: median paired throughput 25.31x (range 24.46–27.94x); p50 reduction 97.4%; p99 reduction 98.4%.

historical_close112 preserves the valid 112-byte close/log/clear/single-listener behavior from source 3fbe63b. It matches the recorded old response-size shape.
matched_close512 raises its backlog to the optimized value to avoid attributing simple accept-queue capacity to the remaining optimizations. It is the conservative primary control.
Socket error events are reported, never omitted; latency percentiles are for completed requests. Server close-phase I/O counters can include peers terminating in-flight work after wrk stops and are not equivalent to load-generator errors.
These are measurements on the recorded hosted VM. They do not retroactively establish the exact hardware/configuration of the 2025 logs.
