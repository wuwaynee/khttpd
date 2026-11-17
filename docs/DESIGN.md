# Design and invariants

## Research question and scope

A fixed-response microservice isolates a small portion of server behavior: accept, parse headers, select a fixed response, transmit, and reuse or close the connection. We vary connection reuse, the number of listeners, and buffer allocation with the same payload, parser, connection engine, and bounded blocking-worker model on both sides.

The kernel implementation uses Linux kernel sockets and kthreads. The control uses Linux sockets and pthreads. They still differ in scheduling, runtime, accounting, and implementation details of their transport adapters. This is a controlled comparison of these two implementations, not proof of a universal kernel/userspace advantage.

Kernel placement does not bypass TCP/IP. kernel_recvmsg copies into the receive buffer; kernel_sendmsg does not establish zero-copy transmission. Fewer userspace syscall crossings are a hypothesis to measure. CPU cycles, context switches, allocator behavior, and security costs cannot be inferred from throughput alone.

## Components

| File | Responsibility |
|---|---|
| main.c | Validate immutable parameters; allocate listeners, worker records, and optional buffers; create/join tasks; unwind partial initialization |
| http_server.c | Kernel socket adapter, client publication, timeout setup, per-connection accounting |
| http_protocol.c/.h | Bounded header delimiter scan, strict subset parsing, immutable responses |
| http_connection.c/.h | Shared stream handling, ordered responses, partial writes, absolute deadlines |
| tools/reference_server.c | Comparable Linux pthread/socket adapter |
| tests/ | Shared-core, fake-transport, live-socket, evidence-parser and fuzz tests |
| scripts/ci-vm.sh | Guest boot and kernel lifecycle tests |
| bench/ | Real measurement capture and strict artifact summarization |

## Ownership and shutdown

A listener belongs to the module controller. Workers borrow it. A worker exclusively owns its accepted socket and releases it once. The controller can call shutdown on that socket while holding client_lock; it never releases a worker-owned socket.

The client_lock protects publication and removal of worker.client. It is not held across normal receive/send operations. The controller sets the global stopping flag first. A worker that accepts concurrently takes the lock and checks that flag before publishing the socket. After that point either the controller sees the socket and shuts it down, or the worker discards it without serving.

Shutdown order:

1. Set stopping, preventing new published work.
2. Shut down listeners to wake accepts.
3. Under each client_lock, shut down any published client to wake reads/writes.
4. Call kthread_stop for every created worker and release the controller's task reference.
5. Free worker buffers and records, then release listeners.

No module-owned memory or listener is freed before all workers are joined. Partial initialization calls the same shutdown routine. Every worker record's mutex is initialized before any operation can fail through that routine.

Tasks are created stopped with kthread_create. The controller obtains a task_struct reference before waking each task. This reference keeps a task pointer valid even if its function returns before the controller reaches kthread_stop. A task reference is different from a module reference: this design relies on joining all owned tasks before module exit returns.

The implementation does not install custom socket callbacks or schedule deferred work containing module text. Kernel TCP resources may have their own post-close lifetime, but the module's worker code is finished before unload.

The userspace adapter follows the same ownership pattern. Signals are blocked before thread creation; the main thread uses sigwait, sets an atomic stop flag, shuts down sockets, then joins workers.

## Bounds and failure behavior

| Resource | Bound / policy |
|---|---|
| Workers | 1–512 persistent tasks, default 64 |
| Listeners | 1–min(workers,64), default 1 |
| Accepted active sockets | At most workers |
| Pending accepted queues | Configured backlog per listener, 1–4096; kernel/network policy may clamp it |
| Header storage | One 8192-byte buffer per active worker at most |
| Request target | At most 2048 bytes, never copied into a URL array |
| Header fields | At most 64 |
| Header/idle deadline | 100–60000 ms, default 5000, absolute per request |
| Response send deadline | The same configured duration, absolute per response |
| Socket blocking interval | 250 ms before a retry/deadline check |
| Responses per connection | 1–1000000, default 1024 |

Preallocation moves header-buffer allocation to module startup and reuses the worker's buffer. It does not preallocate TCP socket buffers, thread stacks, accept queues, or all networking memory. TCP memory bounds remain governed by kernel networking policies. The total cost must include those resources and task stacks, not just workers × 8 KiB.

With prealloc=0, allocation failure closes that connection and increments alloc_errors. There is no on-demand thread creation. Slow clients can consume the available workers; the deadline limits one incomplete header but does not provide global fairness. A full connection can also spend one deadline receiving and another sending. Scheduler stalls can delay either.

SO_REUSEPORT selects among listeners, not individual idle workers. Workers are assigned round-robin to listeners. Unequal hash distribution can queue a connection behind busy workers while another shard has spare capacity. The one-listener configuration is an essential control.

## Protocol contract and deliberate restrictions

The delimiter scanner retains a cursor across reads, touching each incoming byte a constant number of times. Parsing occurs once after a complete header is found. Header scanning and parsing are O(n) within a hard 8 KiB limit. Pipelined headers remain in the same bounded buffer and are processed in order.

The parser does not use NUL termination, strncat, URL decoding, filesystem lookup, or request-body buffering.

| Input | Result |
|---|---|
| GET /, supported HTTP/1.1, one supported Host | 200, fixed 12-byte body, persistent if policy permits |
| HEAD / | 200 with Content-Length: 12 and no response body |
| HTTP/1.0 GET / | 200, close regardless of a keep-alive token |
| Another origin-form target, including /?x=1 | 404, close |
| Another syntactically valid method | 405, Allow: GET, HEAD, close |
| Unsupported eight-byte HTTP version | 505, close |
| Invalid syntax, absent/duplicate Host, duplicate CL, TE + CL | 400, close |
| Nonzero decimal Content-Length | 413, close; body is not consumed |
| Target beyond cap | 414, close |
| Any Expect header | 417, close; no 100-continue handshake |
| Header beyond cap or too many fields | 431, close |
| Any Transfer-Encoding or Upgrade | 501, close |
| Partial header past deadline | Attempt 408, close |
| Idle socket with no partial header | Close on deadline without a response |
| Client EOF | Finish already-read complete requests, then close |

Header names are ASCII tokens and compared case-insensitively. Methods are case-sensitive. Request lines use single spaces; obs-fold, controls, bare LF, and non-ASCII input are outside the accepted subset. Host is a nonempty restricted ASCII value, used only to reject obvious ambiguity; it is not a full RFC authority validator or virtual-host router. Valid general-purpose HTTP requests may therefore be rejected. The project makes no complete HTTP/1.1 compliance claim.

Connection tokens are parsed as a comma-separated list. close wins. Nominating Host, Content-Length, or Transfer-Encoding as hop-by-hop fields is rejected. Content-Length is validated as a bounded decimal string without integer conversion; only all-zero values are accepted. All error responses carry zero body length and close. Unknown response status fallback is 400.

After Connection: close or the configured response cap, the engine sends one closing response and stops, even if further request bytes were already received. Rejected bodies are not drained; Linux may reset a connection with unread incoming bytes. Persistent reuse is never attempted after ambiguous framing.

## I/O and metrics

A send succeeds only after every response byte is accepted by the transport. Partial sends advance an offset; zero-length sends and terminal errors stop the connection. Retryable interruptions/timeouts are checked against an absolute deadline. A stalled sender cannot produce an infinite zero-progress loop.

Kernel and userspace adapters use finite socket timeouts, so the engine can recheck stop/deadline conditions. The configured deadline can be exceeded by roughly one blocking interval plus scheduling delay; it is not a real-time guarantee.

Counters aggregate on connection close. responses counts fully transmitted responses, ok counts transmitted 200 responses, protocol_errors includes rejected requests/routes, io_errors includes unsuccessful sends/reads, timeouts includes receive/idle deadline expiry, and alloc_errors covers per-connection buffer allocation failure. Send deadline failure contributes to io_errors. A successful send means acceptance by the TCP stack, not proof that a peer application received or consumed it.

## Kernel API compatibility

The socket-address pointer used by kernel_bind changed from sockaddr to sockaddr_unsized in newer kernels. This server passes the same properly aligned sockaddr_in storage and its explicit size through a void pointer conversion, so it does not guess a version or backport boundary. Build and runtime checks cover the actual generic and runner kernels recorded in CI. Linux internal APIs are not a stable out-of-tree module ABI; this conversion is not a promise of compatibility with every future kernel.

## Limitations and extensions

There is no TLS, IPv6 listener, filesystem service, authentication, proxy support, arbitrary request body, chunked decoder, or production denial-of-service guarantee. The module operates in init_net rather than implementing per-network-namespace lifecycle. The finite worker model is a conscious simplification.

A future event-driven or queued implementation needs new ownership invariants and controls; it should not be introduced as an unmeasured speed improvement. Upstream-worthy work would require a specific Linux subsystem problem, demonstrated benefit, maintainership review, and accepted patches. This repository currently provides an independent research artifact.
