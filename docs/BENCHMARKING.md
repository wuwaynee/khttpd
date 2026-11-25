# Benchmark protocol

## Evidence status

The historical optimization now has a separate [native same-machine reproduction](PERFORMANCE.md), described in [PERFORMANCE_METHOD.md](PERFORMANCE_METHOD.md). That protocol measures the before/after throughput and latency change. The remainder of this document specifies the kernel/userspace control experiment, which answers a different question.

The kernel/userspace factorial experiment below has not been run in full. Its one-second CI captures test the measurement code only and are not representative steady-load results. The new native historical comparison is reported separately, with its own frozen configurations and evidence.

A result is meaningful only with a source mapping, explicit control, correct responses, environment, repeated runs, and the limits below. Reproducing a procedure does not promise identical numbers on different hardware.

## Controlled comparison

The kernel module and tools/reference_server.c share http_protocol.c and http_connection.c. They use the same fixed body, response headers, maximum header size, deadlines, maximum requests per connection, TCP_NODELAY policy, bounded blocking-worker model, and optional listener sharding. Both default to a backlog of 512 per listener.

The primary full experiment is a 2 × 2 × 2 factorial ablation for **each** implementation:

| Variable | Values | Purpose |
|---|---|---|
| Connection reuse | keep_alive=0 / 1 | Separate TCP reconnect costs from persistent request costs |
| Listeners | shards=1 / 4 by default | Compare shared accept queue with SO_REUSEPORT listener groups |
| Buffer policy | prealloc=0 / 1 | Compare one allocation per connection with startup allocation/reuse |

When reuse is enabled, per-connection allocation is already amortized across requests. Throughput differences and memory-fragmentation effects are separate measurements; a small or neutral throughput difference is a valid outcome.

A combined kernel/user comparison changes execution domain and runtime. An ablation within one implementation changes one factor at a time. Report interactions as well as main effects; do not ascribe a combined multi-factor delta to SO_REUSEPORT or kernel placement alone.

## Prepare

Use a dedicated supported Linux VM or test machine for module experiments. Dedicated physical cores and a separate load host provide stronger isolation. The published native-VM before/after measurements record shared-host conditions and use within-host ratios; CPU contention and client saturation limit their scope. CPU-emulated QEMU guests are used for correctness checks rather than throughput measurements.

~~~bash
sudo apt-get update
sudo apt-get install -y build-essential python3 ripgrep wrk util-linux \
  "linux-headers-$(uname -r)"
make test
make sanitize
make module
# Inspect available CPUs and choose disjoint server/client sets.
lscpu
# Cache sudo credentials for insmod/rmmod and kernel-worker affinity changes.
sudo -v
~~~

Commit source changes first. The runner rejects a dirty checkout and an already-loaded khttpd module, and refuses an occupied port. It never force-unloads a module. It restarts its own server for each cell/run.

Build outputs must come from the recorded clean checkout. The manifest records binary hashes and compiler/kernel information; hashes establish the identity of the recorded files. Rebuilding with a different compiler or configuration is a new experiment. The kernel image/headers must match.

## Run

For an eight-logical-CPU machine, after choosing appropriate CPU placement:

~~~bash
python3 bench/run.py --implementation both \
  --server-cpus 0-3 --client-cpus 4-7 \
  --threads 4 --connections 256 --workers 512 --shards 4 \
  --duration 30 --warmup 5 --repetitions 5 \
  --output artifacts/runs/full-local
python3 bench/summarize.py artifacts/runs/full-local --publish \
  > artifacts/runs/full-local/summary.csv
~~~

The full matrix has 16 cells including the execution-domain factor, with five separately restarted repetitions each: roughly 48 minutes plus startup overhead. Change CPU sets for your machine. Logical siblings are not isolated physical cores; record topology and avoid overlapping siblings if possible.

On a smaller host begin with --implementation userspace --single, fewer workers/connections, and existing disjoint CPU sets. --single uses keep-alive=1, shards=1, prealloc=1. A single-cell run validates that configuration; it does not establish an improvement.

The total worker count must be identical across compared cells. Keep connections comfortably below total workers for non-overload comparisons. In sharded mode, workers per shard also matter; hash imbalance can cause queueing despite aggregate spare capacity. Deliberate overload runs need their own experiment label, connection sweep, error reporting, and interpretation.

For scaling, repeat complete manifests at 1, 8, 32, 64, and 256 connections where the host and worker limits permit. Do not combine different concurrency or CPU assignments into one speedup.

## What is captured

The runner fixes TCP/HTTP policy and uses a seeded, randomized cell order inside each repetition. Every cell has its own server startup, optional same-load warmup, measured phase, and shutdown. The manifest retains:

- Git commit and tree; clean-source check; executable SHA-256 identities.
- Arguments, ordered run configurations and repetitions, and exact wrk argument vectors.
- Kernel/distro, compiler/wrk version output, CPU topology, governors, boost and vulnerability state when available.
- Per-run timestamps, /proc/stat, load averages, network-device counters and thread snapshots.
- Raw wrk stdout and stderr, return status, and SHA-256 hashes.
- Warmup output, server output and final kernel connection-close counters.

All measurements use wrk --latency and bench/validate.lua. The Lua hook checks **every measured response status and body**. Its checked count must equal wrk's completed request count and invalid must be zero. This adds load-generator CPU overhead equally across cells; the load generator may still bottleneck. Inspect client CPU headroom and repeat with another client placement or remote load host before attributing a plateau to the server.

The script does not alter sysctls, governors, power settings, IRQ affinity, or mitigations. Network softirq/IRQ work is not necessarily confined to the worker CPU set. Record any external changes and preserve them across cells. Save CPU time, perf counters and memory evidence separately if making efficiency or allocation claims.

## Failure and summarization rules

Any nonzero load-generator status, socket error, timeout, unexpected status/body, missing percentile, checksum mismatch, duplicate run, missing experiment cell, or incomplete run manifest prevents normal summarization. A failed run stays visible in its manifest. Diagnose it; retain the failed experiment rather than silently deleting an inconvenient repetition.

The summary reports median requests/sec and minimum/maximum across runs, and the median of each run's p50 and p99. These are **medians of run-level percentiles**, not pooled response percentiles. Five repetitions are a publication floor, not proof of statistical significance. Show variability and avoid precise claims unsupported by the spread.

Before quoting a relative improvement, require identical response semantics, CPU placement, concurrency, worker budget, payload, timeouts and version-controlled configurations. Show the baseline's absolute measurements and limitations. A local 12-byte loopback microbenchmark says nothing by itself about TLS, file serving, Internet traffic, or a mature event-driven server.

## Profiling and optional BPF

Run diagnostic captures separately from the uninstrumented throughput matrix because sampling changes overhead. For example, perf stat can count cycles, instructions and context switches over a clearly defined workload; CPU affinity and process/system scope must be written down. A system-wide capture includes the load generator and unrelated tasks, so it cannot be called server CPU cost without attribution.

~~~bash
# Availability depends on kernel config, BTF/probes, permissions and bpftrace.
sudo bpftrace -l 'kretprobe:tcp_sendmsg'
sudo bpftrace -l 'tracepoint:tcp:tcp_retransmit_skb'
sudo bpftrace tools/tcp_counters.bt 8081
~~~

The script filters successful tcp_sendmsg returns to khttpd worker names. Retransmit tracepoint events are filtered by local source port. Counts are cumulative, not five-second rates. A return value measures bytes accepted by the stack, not packets delivered over a physical network. Zero observed retransmit events is not proof of a valid experiment or end-to-end delivery. This optional script is not covered by the normal VM CI; check probe availability before using it.

wrk is a closed-loop load generator. As a server slows, the client offers less work; its latency distribution does not model an independent fixed arrival process. For tail-latency/SLO claims, design a separate constant-rate experiment, include failures, and address coordinated omission. See [wrk](https://github.com/wg/wrk), [wrk2](https://github.com/giltene/wrk2), [Linux external modules](https://docs.kernel.org/kbuild/modules.html), and [HTTP/1.1 framing](https://www.rfc-editor.org/rfc/rfc9112.html).

## Publication checklist

Publish the experiment directory and its source commit together after review. Generated run directories are ignored by default, so add a selected evidence directory explicitly with git add -f, including failed runs relevant to the comparison. Explain the experiment in prose; do not submit only a table of favorable medians.

The before/after results in [PERFORMANCE.md](PERFORMANCE.md) are independent of this kernel/userspace protocol. The latter has not established a kernel/userspace performance difference or an allocator-latency result.
