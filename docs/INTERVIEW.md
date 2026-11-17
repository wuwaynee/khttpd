# Interview preparation and claim boundaries

## A precise opening

"I optimized and hardened an existing NCKU kernel HTTP teaching project. The old server advertised Connection: close, so each request caused connection and kernel-thread churn. I recovered the before/after source, measured it repeatedly on the same Linux VM, and separated connection reuse, logging, listener layout and allocation effects. I also made worker/socket teardown bounded and testable."

Use this only after personally understanding and reproducing the corresponding changes. A repository can demonstrate implemented behavior; it cannot demonstrate that you can explain someone else's patch.

## Resume wording supported by the revised implementation

The [ready-to-use LaTeX snippet](RESUME_SNIPPET.tex) reports the new primary result: 13.3× median paired throughput, 13.4k → 178.8k req/s, p50 18.26 → 0.694 ms and p99 34.36 → 1.91 ms, five 30-second wrk 4T/256C loopback runs on a 4-vCPU Linux VM.

- Hardened an experimental in-kernel HTTP microserver with bounded workers, tracked socket ownership, deadline-based I/O, and strict request framing; added automated protocol and module-lifecycle tests in Linux VMs.
- Built a shared userspace control and randomized benchmark harness with CPU placement, response validation, versioned configurations, and raw-result integrity checks to isolate connection reuse, listener sharding, and buffer preallocation.

Use the quantified wording and measured revision in [PERFORMANCE.md](PERFORMANCE.md) alongside these engineering claims. The original title "Kernel-Resident HTTP Microserver" accurately describes the implementation.

The historical 11× observation and the new same-machine measurements are distinct environment-specific results. Report the old absolute values as historical observations, or use the new repeated measurements for the quantitative resume bullet. Never compute a speedup by mixing their rates. Credit inherited mempool work separately from personal changes.

## Evidence map

| Claim | What supports it | Boundary |
|---|---|---|
| Bounded application resources | Fixed workers, capped headers, bounded listener count, VM tests | TCP queues, stacks and kernel networking memory also consume resources |
| Safe owned-worker teardown | Stop flag, client mutex, shutdown, task references, joins; active-unload tests | Not a formal proof or a security audit |
| Correct subset framing | Shared tests, socket tests, sanitizer/fuzz runs | Not full HTTP conformance or arbitrary bodies |
| Repeated before/after performance | Recovered legacy behavior, matched backlog, randomized runs and raw artifact manifests | The exact original machine was not recovered; new absolute values belong to the recorded VM |
| Reduced allocations | One reusable header buffer per worker when enabled | No measured fragmentation or latency benefit |
| Linux relevance | Kernel lifecycle example, concrete regressions and reproducible tooling | No Linux upstream acceptance |

## Deep-dive questions you should be able to answer

**Why put HTTP in the kernel?**

To study a design point, not because production HTTP belongs there by default. Removing some userspace crossings may help a tiny workload, but kernel code increases the blast radius of parsing bugs and loses mature TLS, isolation and deployment tooling. Show controlled measurements before claiming a benefit.

**Is it zero-copy? Does it eliminate context switches?**

No. The code still uses the normal socket/TCP stack, receives into a buffer and transmits through kernel_sendmsg. Blocking workers can sleep and be rescheduled. Measure copies, cycles and context switches separately.

**What was the most serious original bug?**

Accepted connection threads were not tracked by module teardown. Stopping acceptors did not join those threads, so they could access freed module text or the destroyed buffer pool. There were also literal truncated source lines and an unbounded URL append.

**Why is a stop flag not enough?**

A thread can be asleep in accept/receive/send. Shutdown wakes the relevant socket operation; finite timeouts provide another opportunity to check the flag. Joins establish that worker code has finished before freeing resources.

**Why keep an explicit task reference?**

A created thread can return before teardown reaches it. A stable task_struct pointer needs an owned reference. That reference does not keep module code loaded; joining all workers before module exit returns is the module-lifetime invariant.

**What prevents a shutdown/publication race?**

The controller sets stopping first. Worker publication and controller shutdown use the same client_lock. A concurrently accepted socket is either published while protected and visible for shutdown, or rejected after observing stopping. Only the worker releases its client socket.

**Why fixed workers instead of unbounded per-connection threads?**

It makes task and buffer costs explicit and removes per-connection task creation. The tradeoff is idle-connection occupancy and queueing. An event-driven implementation may scale better but would require a different control and more complex lifetime reasoning.

**What exactly does SO_REUSEPORT shard?**

Listeners and their accept queues. Linux chooses a listener; it does not choose any idle worker across the entire pool. Uneven flow distribution can hurt latency. One listener is a required control, not an assumed inferior design.

**What did you learn about mempool?**

A Linux mempool primarily reserves objects to support allocation progress under pressure. It is not proof that every request uses a recycled buffer or that fragmentation decreased. The matched preallocation experiment found no stable throughput advantage for this persistent-connection workload. It remains an explicit ownership policy, not a demonstrated fragmentation or allocator-latency improvement.

**How does TCP affect parser design?**

Read boundaries are unrelated to request boundaries. Headers can span reads, multiple requests can arrive in one read, and writes can be partial. Keep leftovers, track consumed bytes, preserve response order, and stop after a closing response. The shared engine tests those cases directly.

**How do you avoid request smuggling here?**

Use one bounded parser, reject TE plus CL, all transfer codings, duplicate CL, obsolete folding and invalid framing, and close after errors. Request bodies are outside the supported subset. This is a restrictive policy, not a claim to implement all HTTP safely.

**Can a sender keep a worker forever by dripping bytes?**

The deadline is absolute per header, not reset by each received byte. It is checked between finite blocking operations. It can be exceeded by one blocking interval and scheduling delay, and a client sending complete requests can still occupy a worker until the configured response cap.

**What is the baseline?**

For the historical speedup, the recovered kernel server's close/log/clear behavior, with backlog matched to 512 as a conservative control. The modern server and recovered keep-alive version run the same HTTP/1.1 GET / workload on the same VM. The shared pthread control answers the separate kernel/userspace execution-domain question; it is not the denominator of the historical 11× claim.

**What exactly changed in keep-alive?**

The historical response macro's name suggested keep-alive, but its actual header said Connection: close. The client therefore reconnected. In a thread-per-connection server that also means a new kernel thread for almost every request. Correct persistence lets approximately 256 connections/threads process millions of requests; the counters make this mechanism observable.

**Are all three optimizations responsible for the whole speedup?**

Use the matched feature results in PERFORMANCE.md. Listener sharding changes connection acceptance; it need not improve steady-state requests once all persistent connections exist. Buffer allocation is already amortized over requests on a persistent connection. Do not assign the entire throughput multiplier to sharding or mempool without the corresponding evidence.

**Why does workers=4 not reproduce the revised workload?**

The original parameter counted acceptors and listeners while additional connection threads were created dynamically. The revised workers parameter bounds active connection handling, and shards counts listeners. The measured hardened configuration uses workers=512 and shards=4 for 256 offered connections.

**How do you know the body or response size did not manufacture the gain?**

Both serve the same 12-byte body and are checked by a status/body validation phase. The runtime probe confirms 112-byte close frames and 117-byte keep-alive frames. The optimized response is slightly larger, with the difference in the Connection header. Raw wrk output and source/binary identities are retained.

**Why can server I/O counters differ from wrk errors?**

wrk stops with some requests in flight. A server may finish a send whose response the client no longer processes, then observe the peer resetting or closing the socket. The benchmark reports client-completed requests and wrk socket errors, and keeps the later server counters separately. A successful kernel send is not proof that a client application consumed the response.

**What does wrk p99 mean?**

The completed requests under a closed-loop offered load. It is not a guarantee about an independent fixed arrival process. Errors, overload, client CPU saturation and coordinated omission affect interpretation. A median of run-level p99 values is not a pooled p99.

**Why might preallocation show little benefit?**

With keep-alive, allocating once per connection is already amortized over many requests. Network/scheduling costs may dominate. A null result is useful if the experiment is controlled and the uncertainty is reported.

**Which kernel versions did you actually test?**

Use the exact versions in VALIDATION.md. The matrix includes two generic LTS families and the runner kernel. The newer socket-address API required an explicit compatibility decision. Building against matching headers and symbols matters; Linux internal APIs do not promise a stable external-module ABI.

**What is the actual contribution to Linux?**

An independently reviewable out-of-tree correctness/evaluation artifact. If you want to claim an upstream contribution, identify a real subsystem issue, submit a justified patch with evidence, and link the accepted change. This repository alone does not establish that.

## Before interviewing

Clone the exact tested revision into a supported VM. Run make test and make sanitize, build/load the module, inspect stats, and walk through an active-connection unload. Explain every ownership transition without reading this document. Run the frozen benchmark yourself and compare your result with the published ranges before using the quantitative resume bullet. Revisit the baseline, payload, CPU topology, errors and variability before memorizing any speedup.
