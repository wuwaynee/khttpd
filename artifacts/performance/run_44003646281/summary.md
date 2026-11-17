# Matched performance measurements

Source commit: 9ee238ad74fb65368d876fd56228a0944c307147
Kernel: Linux ip-10-0-143-146 7.0.0-1014-aws #14~24.04.1-Ubuntu SMP PREEMPT Fri Sep 18 21:24:59 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

Each cell: 3 runs, 10 seconds, wrk 4T/256C.
Primary measurement is plain wrk; a separate same-load phase validates every response.
Values below are medians across runs, not pooled percentiles.

| Profile | req/s | p50 ms | p99 ms | socket error events | min–max req/s |
|---|---:|---:|---:|---:|---:|
| hardened4 | 384955.94 | 0.326 | 0.638 | 0 | 332283.17–394071.66 |
| legacy_no_pool | 397541.68 | 0.330 | 0.558 | 0 | 327991.81–400008.20 |
| hardened_alloc | 390437.86 | 0.329 | 0.581 | 0 | 388018.10–394762.84 |
| legacy_optimized4 | 394148.29 | 0.327 | 0.559 | 0 | 322751.89–400116.54 |
| hardened_close | 81298.41 | 0.537 | 1.600 | 0 | 79451.41–81405.15 |
| hardened1 | 372757.43 | 0.337 | 0.760 | 0 | 363535.96–390044.10 |
| keepalive_logged512 | 177273.44 | 0.496 | 20.980 | 0 | 163700.77–178312.75 |
| legacy_optimized1 | 390614.29 | 0.330 | 0.634 | 0 | 327332.97–399413.25 |
| matched_close512 | 14241.08 | 10.620 | 104.530 | 0 | 14172.56–14287.35 |
| keepalive_quiet512 | 347342.40 | 0.339 | 0.847 | 0 | 323598.31–385474.83 |

Ratios use the matched_close512 control in the same repetition.

- legacy_optimized1: median paired throughput 27.34x (range 23.10–28.05x); p50 reduction 96.8%; p99 reduction 99.4%.
- legacy_optimized4: median paired throughput 27.59x (range 22.77–28.10x); p50 reduction 96.9%; p99 reduction 99.4%.
- hardened1: median paired throughput 26.30x (range 25.44–27.39x); p50 reduction 96.8%; p99 reduction 99.3%.
- hardened4: median paired throughput 27.03x (range 23.26–27.81x); p50 reduction 97.1%; p99 reduction 99.4%.

historical_close112 preserves the valid 112-byte close/log/clear/single-listener behavior from source 3fbe63b. It matches the recorded old response-size shape.
matched_close512 raises its backlog to the optimized value to avoid attributing simple accept-queue capacity to the remaining optimizations. It is the conservative primary control.
Socket error events are reported, never omitted; latency percentiles are for completed requests. Server close-phase I/O counters can include peers terminating in-flight work after wrk stops and are not equivalent to load-generator errors.
These are measurements on the recorded hosted VM. They do not retroactively establish the exact hardware/configuration of the 2025 logs.
