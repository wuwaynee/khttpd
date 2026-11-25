# Matched performance measurements

Source commit: c1dda1ae93c1deb3cf2cc258051a7113f13a0094
Kernel: Linux runnervm8axu0 7.0.0-1017-azure #17~24.04.1-Ubuntu SMP PREEMPT Thu Sep 24 18:46:57 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

Each cell: 5 runs, 30 seconds, wrk 4T/256C.
Primary measurement is plain wrk; a separate same-load phase validates every response.
Values below are medians across runs, not pooled percentiles.

| Profile | req/s | p50 ms | p99 ms | socket error events | min–max req/s |
|---|---:|---:|---:|---:|---:|
| legacy_optimized4 | 180608.67 | 0.684 | 1.600 | 0 | 176958.28–181909.15 |
| hardened4 | 178780.35 | 0.694 | 1.910 | 0 | 175509.41–179788.53 |
| matched_close512 | 13389.00 | 18.260 | 34.360 | 0 | 13363.29–13454.29 |

Ratios use the matched_close512 control in the same repetition.

- legacy_optimized4: median paired throughput 13.51x (range 13.15–13.61x); p50 reduction 96.3%; p99 reduction 95.3%.
- hardened4: median paired throughput 13.34x (range 13.04–13.43x); p50 reduction 96.2%; p99 reduction 94.4%.

historical_close112 preserves the valid 112-byte close/log/clear/single-listener behavior from source 3fbe63b. It matches the recorded old response-size shape.
matched_close512 raises its backlog to the optimized value to avoid attributing simple accept-queue capacity to the remaining optimizations. It is the conservative primary control.
Socket error events are reported, never omitted; latency percentiles are for completed requests. Server close-phase I/O counters can include peers terminating in-flight work after wrk stops and are not equivalent to load-generator errors.
These are measurements on the recorded hosted VM. They do not retroactively establish the exact hardware/configuration of the 2025 logs.
