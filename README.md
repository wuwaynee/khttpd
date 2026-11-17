# kHTTPD: bounded kernel HTTP experiments

kHTTPD is an out-of-tree Linux module for studying a narrow question: **how do connection reuse, listener layout, and buffer allocation affect a tiny HTTP service when the protocol and worker model are controlled?**

This extension of the [NCKU/sysprog21 teaching project](NOTICE.md) provides a bounded kernel implementation, a Linux userspace control using the **same parser and connection engine**, and an automated correctness/evidence workflow. It serves a fixed 12-byte body. It does not serve files or replace a production web server.

**Performance status:** the historical 3.4k → 37.7k req/s logs lack a recoverable experiment manifest and version mapping. They are preserved as [unverified historical records](artifacts/legacy-2025/README.md). They are not a validated performance result for this revision. No replacement speedup has been measured or invented.

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
- [Interview and claim boundaries](docs/INTERVIEW.md)
- [Provenance and license](NOTICE.md)

Useful Linux engineering contributions here are the reviewable lifetime fixes, bounded protocol handling, regression cases, and controlled experiment machinery. Claims of higher throughput, lower CPU cost, production readiness, or upstream acceptance require their own evidence.
