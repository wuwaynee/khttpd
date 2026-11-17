# kHTTPD: bounded kernel HTTP experiments

kHTTPD is an out-of-tree Linux HTTP microserver that measures and reduces connection, thread-creation and allocation overhead in a fixed-response request path. It includes recoverable before/after configurations, a bounded implementation, and repeatable throughput/latency experiments.

This extension of the [NCKU/sysprog21 teaching project](NOTICE.md) provides a bounded kernel implementation, a Linux userspace control using the **same parser and connection engine**, and an automated correctness/evidence workflow. It serves a fixed 12-byte body. It does not serve files or replace a production web server.

The historical measurement was **3.4k → 37.7k req/s (~11×)**, p50 **74.8 → 2.99 ms**, and p99 **113.04 → 12.06 ms**. The original logs are preserved. A new [same-machine reproduction](docs/PERFORMANCE.md) freezes the recovered historical behavior and hardened implementation, with repeated runs, raw output, response validation, source/binary identities and explicit error counts. The original machine is not available; the historical and newly measured absolute values are reported separately.

New primary experiment: **5 × 30 seconds per configuration, same 4-vCPU AMD Linux VM, wrk 4T/256C over loopback**. Values are medians across runs.

| Configuration | Completed req/s | p50 | p99 |
|---|---:|---:|---:|
| Recovered close baseline, backlog 512 | 13,389 | 18.260 ms | 34.360 ms |
| Recovered optimized path, 4 listeners | 180,609 | 0.684 ms | 1.600 ms |
| Hardened implementation, 512 workers / 4 listeners | **178,780** | **0.694 ms** | **1.910 ms** |

The hardened median paired throughput gain was **13.34×**, with every repetition **13.04–13.43×**. All 15 primary cells had zero wrk socket errors; separate status/body validation checked 3.44 million responses with zero invalid. [Raw files, observed ranges, independent feature controls and scope](docs/PERFORMANCE.md) are published together. [繁體中文解說](docs/PERFORMANCE_ZH.md) explains the diagnosis.

Read the [measurement and reconstruction method](docs/PERFORMANCE_METHOD.md) for the exact baseline and conditions. The principal effect is avoiding per-request connection and kernel-thread churn. Feature controls evaluate logging, listener sharding and buffer policy separately.

## Start without loading a module

On Ubuntu, install make, a C compiler, Python 3, and ripgrep:

~~~bash
sudo apt-get update
sudo apt-get install -y build-essential python3 ripgrep
make test
make sanitize
~~~

These commands build the shared engine, a bounded pthread control server, and socket tests. They do not require kernel headers or root. No build target downloads code, installs Git hooks, or changes Git configuration.

To run the control server manually:

~~~bash
# PORT WORKERS SHARDS KEEPALIVE PREALLOC TIMEOUT_MS MAX_REQUESTS
./build/reference_server 8081 64 1 1 1 5000 1024
# In another terminal:
curl --fail http://127.0.0.1:8081/
# Stop the server with Ctrl-C.
~~~

## Build and run the kernel variant

Use a disposable Linux VM. The module runs privileged networking code in the kernel; Secure Boot/lockdown may reject an unsigned module. Docker is not an isolated guest kernel.

The automated x86-64 guest matrix includes Ubuntu generic Linux 5.15 and 6.8, plus the CI runner's preinstalled kernel. Exact package versions and test output are captured by each [CI run](https://github.com/wuwaynee/khttpd/actions). Other kernels and architectures need separate build/runtime verification.

~~~bash
sudo apt-get install -y build-essential "linux-headers-$(uname -r)"
make module
sudo insmod khttpd.ko port=8081 workers=64 shards=1
curl --fail http://127.0.0.1:8081/
cat /sys/module/khttpd/parameters/stats
sudo rmmod khttpd
~~~

The default bind address is **127.0.0.1 in the initial network namespace**. An explicit bind_addr=0.0.0.0 exposes the service on IPv4 interfaces in that namespace; it is unnecessary for local experiments. Unloading shuts down sockets and joins every worker before freeing storage. Never force unload.

For another prepared kernel build tree, use make module KDIR=/path/to/kernel/build. The headers and Module.symvers must match the kernel that will load the module.

## Explicit protocol contract

- GET / returns Hello World! with Content-Length: 12.
- HEAD / returns the corresponding headers without a body.
- HTTP/1.1 requires one nonempty supported Host value; connections persist unless closed by policy. HTTP/1.0 always closes.
- TCP fragmentation and ordered pipelined requests are handled. Connection: close stops processing further queued requests.
- The entire header is limited to 8 KiB, the target to 2 KiB, and fields to 64. Unsupported routes return 404; unsupported methods return 405.
- Request bodies, Transfer-Encoding, Expect, Upgrade, obsolete folding, duplicate Content-Length, and ambiguous framing are rejected and the connection closes.

This is a restrictive HTTP/1.x subset, not a general HTTP implementation. See the exact [contract and ownership design](docs/DESIGN.md).

## Resource and scheduling model

The default is 64 persistent workers and one listener, with at most one active connection per worker. Each worker accepts and serves synchronously. Optional SO_REUSEPORT listeners divide those workers into shards. Preallocation uses one 8 KiB buffer per worker; disabling it allocates that buffer per accepted connection.

Idle connections occupy workers. Additional connections wait in bounded TCP accept queues; shard imbalance can make that wait worse. Deadlines and a per-connection response limit bound individual occupancy, but this is not an event-driven high-concurrency architecture.

Inspect all limits with modinfo khttpd.ko. The stats parameter is updated when connections close, so it is not an instantaneous per-request counter.

## Reproducible experiments

For the historical optimization and resume workload, use the [native before/after reproduction](docs/PERFORMANCE_METHOD.md). Its primary comparison uses HTTP/1.1 GET /, the same 12-byte body, wrk 4T/256C, and five 30-second runs per setting. The [performance workflow](.github/workflows/performance.yml) runs it on a fresh Linux VM; the [matched feature workflow](.github/workflows/ablation.yml) repeats the mechanism controls independently.

The hardened 256-connection configuration is workers=512, shards=4, keep_alive=1, prealloc=1, max_requests=1000000. In this revision workers bounds active connection handling; shards counts listeners. The older workers parameter counted acceptors, so the two meanings are not interchangeable.

The separate protocol below compares kernel and userspace implementations. It answers a different question from the project's historical before/after speedup.

Read [the benchmark protocol](docs/BENCHMARKING.md) before quoting numbers. The runner records a clean source commit/tree, binary hashes, environment, CPU placement, randomized run order, raw wrk output, and response validation. It refuses failed or incomplete evidence.

~~~bash
sudo apt-get install -y wrk util-linux
# Choose disjoint CPU sets that exist on your machine.
python3 bench/run.py --implementation userspace --single \
  --server-cpus 0-1 --client-cpus 2-3 \
  --output artifacts/runs/control
python3 bench/summarize.py artifacts/runs/control --publish
~~~

The full ablation uses --implementation both without --single, built kernel and userspace binaries, and sudo credentials for module lifecycle/worker affinity. It compares all combinations of keep-alive, one versus multiple listeners, and per-connection versus preallocated buffers. It does not attribute a multi-change speedup to any single mechanism.

## Validation and review

[CI](.github/workflows/ci.yml) performs C warning checks, shared-engine tests, userspace socket integration, ASan/UBSan, a bounded fuzz smoke test, and isolated QEMU kernel lifecycle/protocol tests on the generic and runner kernels. A one-second wrk capture checks the evidence pipeline; it is not performance evidence. CI failures fail the workflow.

- [Design and invariants](docs/DESIGN.md)
- [Original 28-file audit](docs/AUDIT.md)
- [Experimental protocol](docs/BENCHMARKING.md)
- [Measured performance](docs/PERFORMANCE.md)
- [Historical reconstruction method](docs/PERFORMANCE_METHOD.md)
- [Interview and claim boundaries](docs/INTERVIEW.md)
- [Provenance and license](NOTICE.md)

Useful Linux engineering contributions here are the reviewable lifetime fixes, bounded protocol handling, regression cases, and controlled experiment machinery. The measured before/after gain applies to the recorded workload. A kernel/userspace advantage, production readiness and upstream acceptance would require separate evidence.
