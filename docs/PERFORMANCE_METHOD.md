# Reconstructing and measuring the historical optimization

The historical resume observation was 3,436.83 → 37,739.84 requests/s, p50 74.80 → 2.99 ms, and p99 113.04 → 12.06 ms with wrk 4 threads and 256 connections. Those values are retained as historical observations. The current reconstruction runs executable before/after configurations on one recorded Linux VM and publishes every repetition. See PERFORMANCE.md for the resulting measurements and scope.

## Recoverable source and behavior

| Evidence | Meaning |
|---|---|
| [3fbe63b](https://github.com/wuwaynee/khttpd/commit/3fbe63b16956dad8a05dfa087af932c283cfdcd0) | The nominal keep-alive response was changed to Connection: close. This version logs each URL, clears the receive buffer, and creates a new kernel thread for every accepted connection. |
| [c745737](https://github.com/wuwaynee/khttpd/commit/c745737c59476688755ca171da084a9670c5d9db) | The retained feature branch has complete, untruncated keep-alive/server code, multiple SO_REUSEPORT listeners, removed per-request URL logging/buffer clearing, and a benchmark script. |
| [1ca8a87](https://github.com/wuwaynee/khttpd/commit/1ca8a87e228afe0a8885880f03089fd0d42bb713) | The later web edit introduced three literally truncated lines into main. It cannot represent the executable used to obtain the earlier numbers. |
| Recorded read bytes / completed requests | The baseline rounds to 112 application bytes/response and the optimized log to 117. These match the valid close and keep-alive responses. The runtime probe now checks these lengths directly. |

The record is consistent with measurements from a working tree or retained feature branch followed by an incomplete source upload. The exact old executable and old machine are not available, so this is an evidence-based reconstruction rather than a claim to have recovered that machine bit-for-bit. The 112-byte historical frame needs no extra trailing CRLF; an earlier pre-fix source did oversend CRLF, but that is a separate diagnostic profile.

## Primary comparison

All requests are GET / HTTP/1.1 with the same 12-byte Hello World! body. Both implementations use the kernel TCP stack. The baseline's close header makes wrk reconnect; the optimized response permits persistent connections. This is a comparison of versions of this project. The separate userspace control answers a different execution-domain question.

| Profile | Listener backlog | Connection/thread behavior | URL logging / buffer clearing |
|---|---:|---|---|
| matched_close512 | 512 | Historical close-per-request behavior; new kthread per accepted connection | Enabled / enabled |
| legacy_optimized4 | 512 per listener | Recovered persistent-connection behavior; 4 listeners; one kthread per connection | Disabled / disabled |
| hardened4 | 512 per listener | Persistent connections; 4 listeners; 512 tracked reusable workers | Disabled / no per-request zeroing |

The conservative control raises the old backlog to the same value as the optimized version. The old default of 100 is retained as a diagnostic configuration: at 256 offered connections it can produce socket timeouts and much longer tails. Those failures remain visible rather than being silently removed from a favorable table.

Each primary cell runs five times for a requested 30 seconds. Every repetition shuffles the entire profile order using a recorded seed. Each cell has a fresh module load, a separate 2-second response-validation phase, 5 seconds of warmup, the timed phase, and module cleanup. The primary command is:

~~~bash
wrk -t4 -c256 -d30s --latency --timeout 2s http://127.0.0.1:8081/
~~~

The separate validation phase uses bench/validate.lua to check status and body for every response. The primary throughput phase is plain wrk, matching the original measurement style and avoiding the callback's client CPU overhead. wrk still parses all primary HTTP responses and reports status/socket errors. This distinction is intentional and is recorded in each manifest.

## Historical adapter repairs

bench/diagnose/legacy keeps the recovered HTTP hot path and feature switches. It vendors http-parser 2.9.4 at [2343fd6](https://github.com/nodejs/http-parser/tree/2343fd6b5214b2ded2cdcf76de2bf60903bb90cd), with its license, kernel-header adaptations, and explicit fallthrough markers. It does not download an unpinned parser at build time.

Common repairs apply to every historical configuration: bounded URL copying, terminal zero-send handling, current socket APIs, stable acceptor task references, and connection-owned module references. Workers exit through module_put_and_kthread_exit so their final instructions execute safely after dropping the module reference. The module binds loopback only.

A long experiment also reproduced a real old lifecycle issue: an accepted connection can outlive the load generator and remain blocked in receive. The diagnostic module now gives accepted sockets a 10-second blocking timeout, longer than wrk's 2-second request timeout. This bounds orphan cleanup without reducing the timeout of a successful measured request. The runner waits for references to drain and never force-unloads the module. The hardened server already has explicit socket ownership, absolute deadlines, shutdown and joins.

These repairs are disclosed because the reconstruction is not byte-identical to an old binary. There is no inserted per-request delay, rate limiter, or target-dependent baseline slowdown.

## Ablations and accounting

The independent matched experiment repeats ten configurations three times for 10 seconds on a second VM, with 2-second validation and warmup phases. It includes the close baseline and the same one-switch controls: keep-alive with and without logging, buffer clearing on/off, one versus four listeners, mempool versus allocation, and hardened preallocation/close policies. Compare matched configurations within that dataset; effects need not multiply independently. The primary archive also contains exploratory 3 × 10-second controls from the primary VM; they are not substituted for the matched matrix or compared to 30-second cells as isolated effects.

Legacy per-CPU counters record accepted connections, created kthreads, completed response sends, application bytes and buffer-allocation calls. These establish how much connection/thread/allocation work is amortized. A mempool allocation call is not proof of reuse from the reserve, reduced fragmentation, or a measured allocator-latency reduction.

System CPU time and context-switch snapshots include wrk, server workers and other VM activity. They are workload-level diagnostics, not server-exclusive CPU counters. Kernel send/read error counters sampled after wrk exits can include termination of in-flight work; load-generator socket errors are reported separately. Throughput uses client-completed requests. Latency percentiles describe completed requests, so error counts must be read alongside them.

The summary recomputes metrics from hash-checked raw files, requires every configured repetition exactly once, rejects incomplete experiments and body/status validation failures, and preserves socket-error counts. It reports medians and observed ranges; run-level p99 medians are not pooled p99 values or a statistical significance claim.

## Run the reproduction

Use a disposable x86-64 Linux VM with matching headers and module-loading privileges. The recorded native measurements use the entire 4-vCPU VM for both wrk and the servers; there is no emulated CPU or separately scheduled QEMU guest in this throughput test. The historical diagnostic adapter is tested on the native kernel recorded in the result; the hardened module also has its separate kernel compatibility matrix.

~~~bash
sudo apt-get install -y build-essential python3 ripgrep wrk \
  "linux-headers-$(uname -r)"
make module
make -C "/lib/modules/$(uname -r)/build" \
  M="$PWD/bench/diagnose/legacy" W=1 modules
sudo -v
python3 bench/diagnose/run.py --output build/my-performance \
  --duration 30 --warmup 5 --repetitions 5 \
  --profiles matched_close512,legacy_optimized4,hardened4
python3 bench/diagnose/summarize.py build/my-performance
~~~

For the primary frozen implementation, clone the repository and check out c1dda1ae93c1deb3cf2cc258051a7113f13a0094 before the commands above. To re-run the independent matched matrix, use the exact profile list in .github/workflows/ablation.yml at 9ee238ad74fb65368d876fd56228a0944c307147.

The checkout must be clean and committed. The runner refuses preexisting modules or an occupied port. Source commit, binary hashes, wrk/compiler identities, kernel, CPU topology, memory, kernel command line, CPU snapshots, module arguments, actual run order, raw output, validation and file checksums are captured. The performance workflow automates this on a fresh hosted VM.

To inspect just the hardened endpoint under the resume workload:

~~~bash
sudo insmod khttpd.ko workers=512 shards=4 prealloc=1 keep_alive=1 \
  max_requests=1000000 timeout_ms=5000
wrk -t4 -c256 -d30s --latency --timeout 2s http://127.0.0.1:8081/
sudo rmmod khttpd
~~~

The parameter meaning changed during hardening. In the original implementation workers counted acceptors/listeners, with additional connection threads created dynamically. In the hardened implementation shards counts listeners and workers is the active-connection capacity. Reusing an old workers=4 command would cap the hardened server at four concurrent connections; it does not reproduce the recorded 256-connection experiment.

Absolute numbers depend on CPU, kernel, virtualization and background load. Reproduction means the frozen experiment can be rerun and its observed improvement evaluated with its ranges and errors. It is not a guarantee that all machines will print the same req/s or p99. These fixed-body loopback results do not establish TLS, filesystem or general production-server performance, or a universal kernel/userspace advantage.
