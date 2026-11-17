# Repository audit and remediation

Scope: all **28 tracked files** at commit [e9540ba4f49701e8f42460a39c9961d13bd19dd6](https://github.com/wuwaynee/khttpd/tree/e9540ba4f49701e8f42460a39c9961d13bd19dd6), plus relevant Git history. The GitHub snapshot is the source of this audit. The attached ZIP could not be extracted in this session's hosted environment; byte-equivalence between that archive and this snapshot was not established.

This audit distinguishes observed defects from performance hypotheses and missing evidence. It is not a formal verification or an external security audit.

## Most consequential findings

| Severity | Observed issue in the original snapshot | Remediation |
|---|---|---|
| Build blocker | http_server.c lines 57, 70 and 237 literally end with truncated declarations/call text | Replace the broken implementation; compile the full module in CI |
| Critical lifetime risk | Per-connection kthreads are created but not tracked or joined during exit; the module destroys the shared mempool after stopping only acceptors | Fixed owned worker pool, explicit task references, client publication locks, socket shutdown, and joins before any free |
| Critical memory risk | request_url[128] is appended to with strncat using a fragment length, without checking remaining capacity | No URL copy; parse bounded slices with an explicit target/header cap |
| High correctness risk | Parser consumption/error status is ignored; persistent connection behavior assumes a successful parser path | Shared strict framing parser and stream engine; reject ambiguous input and close |
| High availability risk | Unbounded per-connection task creation and no receive/send deadline | Capped persistent tasks, finite socket waits, absolute header/send deadlines and response cap |
| High correctness risk | A zero-length send never advances the send loop; partial/failed writes are not propagated to response accounting | Shared checked send-all loop with terminal zero handling and deadline tests |
| High evidence risk | Single baseline/after logs have no environment or exact source mapping | Preserve records as unverified; replace claims with a controlled, versioned measurement protocol |
| High workflow gap | CI always succeeds on format complaints and never builds or exercises the module | Failing compilation/tests, sanitizers/fuzzing, userspace integration and isolated kernel VM tests |
| Attribution gap | Inherited code and another contributor's mempool change are described too broadly as personal engineering | Restore upstream notice, document provenance, narrow resume claims |

The original source links are immutable: [truncation and URL append](https://github.com/wuwaynee/khttpd/blob/e9540ba4f49701e8f42460a39c9961d13bd19dd6/http_server.c), [module cleanup](https://github.com/wuwaynee/khttpd/blob/e9540ba4f49701e8f42460a39c9961d13bd19dd6/main.c), [non-blocking lint](https://github.com/wuwaynee/khttpd/blob/e9540ba4f49701e8f42460a39c9961d13bd19dd6/.github/workflows/ci.yml).

## Complete original-file inventory

| Original file | Review outcome and disposition |
|---|---|
| .clang-format | Reviewed; retained as an optional formatting preference. Style checking alone is not a correctness gate. |
| .github/workflows/ci.yml | Replaced non-blocking lint with mandatory build/core/socket/evidence/sanitizer/fuzz and generic and runner-kernel guest jobs. Actions pinned to inspected commits; token permissions limited to contents:read. |
| .gitignore | Removed generated-parser assumptions; ignore kernel products, userspace build directory, Python cache and opt-in run captures. |
| LICENSE | Retained MIT terms; restored NCKU's upstream copyright notice and kept extension attribution. |
| Makefile | Removed automatic hook installation and unpinned downloads. Separate Kbuild, userspace, test and sanitizer targets. Works from an extracted source tree without .git for ordinary builds. Benchmark capture still requires Git source identity. |
| README.md | Reframed the research problem, stated protocol/resource limits, corrected build/run behavior and withdrew unverified quantitative claims. |
| artifacts/README.bench.md | Replaced an apparent reproduction recipe with an evidence index. The old --latency omission and claim of default percentile output were wrong. |
| artifacts/bpf.after.txt | Preserved unchanged under legacy-2025; system-wide attempted bytes and interval resets cannot substantiate attributed service behavior. |
| artifacts/perf.after.report.txt | Preserved unchanged; no symbol rows/call graph, so no supported bottleneck inference. |
| artifacts/wrk.after.txt | Preserved unchanged and labeled unverified, not discarded or promoted to current evidence. |
| artifacts/wrk.base.txt | Same; missing exact baseline code/build/environment prevents a controlled comparison. |
| compat/assert.h | Removed with the old parser adapter. The inherited assert was a no-op and hid assertions rather than providing kernel checks. |
| compat/ctype.h | Removed dummy portability shim with unused dependency. |
| compat/limits.h | Removed dummy portability shim with unused dependency. |
| compat/stddef.h | Removed unused Linux-header wrapper. |
| compat/stdint.h | Removed dummy portability shim with unused dependency. |
| compat/string.h | Removed unused adapter and unnecessary linux/memory.h inclusion. |
| htstress.c | Retired from the active tree; use wrk with explicit validation instead. See concrete defects below; original BSD notice remains in history. |
| http_server.c | Replaced truncated/unbounded callbacks and untracked task creation with a finite socket adapter and tracked workers. |
| http_server.h | Replaced implicit header dependencies and global mempool with explicit lifecycle/configuration interfaces. |
| main.c | Validate all parameters, bind loopback by default, allocate a bounded pool, track sockets/tasks, use symmetric initialization/unwind/exit. Remove speculative old-kernel compatibility branches. |
| scripts/aspell-pws | Removed unused dictionary after retiring interactive commit-message tooling. |
| scripts/commit-msg.hook | Removed the interactive terminal/editor dependency from the project workflow. It could hang noninteractive commits and was not part of program verification. |
| scripts/install-git-hooks | Removed automatic mutation of .git/hooks and assumptions that .git is a directory. A ZIP or worktree should not break the build. |
| scripts/pre-commit.hook | Removed tool/dependency requirements unrelated to building, shell word splitting, mixed staged/worktree analysis and suppressions that masked useful results. Compiler/core tests are now explicit. |
| scripts/pre-push.hook | Removed stale master-only check in a main-branch repository. Remote CI validates the actual pushed revision. |
| scripts/test.sh | Replaced forced module removal and unchecked shell commands with a failing userspace test entry point. Module testing happens in a disposable guest. |
| tools/tcp_counters.bt | Replace global entry-argument counts with kernel-worker-filtered successful return-byte counts and local-port retransmit events. Label as optional/unverified on arbitrary probe configurations. |

## Why the old load generator was retired

The inherited htstress implementation was unsuitable as trustworthy evidence machinery without substantial independent repair:

- --threads was declared as taking no long-option argument, although its handler read optarg.
- EPOLLERR passed the epoll descriptor, rather than the connection socket, to getsockopt(SO_ERROR).
- "Concurrency" was instantiated per thread, making total concurrency num_threads × concurrency.
- Shared termination/time state had data races; worker threads were not all joined before output/exit.
- Wall-clock timing and requested-count throughput could misstate actual completed work.
- Atomic 64-bit counts were converted to int in request-limit checks.
- A connection closing could count as success without validating a complete HTTP response/body.
- Error paths and signal handlers were inconsistent; unused counters and resource cleanup added noise.

Replacing it with a maintained load generator and status/body validation is a measurement-design decision. It does not establish that wrk is bias-free; closed-loop load and client CPU limits remain explicit.

## Experimental-design corrections

The old host/compiler/load-generator fields were placeholders. No per-run mapping connected the numbers to exact buildable baseline/optimized commits, configuration, warmup, affinity, repetitions or correctness checks. There was no userspace control, resource budget, or ablation establishing which change caused the difference.

The revised protocol uses a shared engine and matched worker/policy budget; factorial feature combinations; randomized order within repeated rounds; separate restarts/warmups; exact commands; source and binary identities; raw output and checksums; and failure-on-incomplete evidence. It keeps historical logs separate. Nothing retroactively validates those logs.

Statements about fragmentation, context switches, zero-copy behavior, CPU cost or kernel superiority need direct measurements. A Linux mempool reserve is not proof of fragmentation reduction. SO_REUSEPORT may help or hurt depending on scheduling and listener skew. Keep-alive can change TCP setup costs far more than a tiny handler optimization.

## Validation boundaries and remaining work

See docs/VALIDATION.md for actual passing runs. Shared-engine sanitizer/fuzz tests exercise user-compiled code; the stock guest kernels are not KASAN-instrumented. VM protocol/lifecycle tests are real module execution, not a sanitizer proof of kernel memory safety.

Optional BPF instrumentation, kernels outside the tested matrix, IPv6, network namespaces, production hardening and quantitative performance claims remain outside verified scope. The current build is self-contained; distro/toolchain packages are version-recorded rather than bit-reproducibly pinned in a full hermetic image.

The concrete Linux value is a reviewable out-of-tree example with regression tests and controlled evidence capture. Upstream acceptance and a real server-performance improvement remain claims to earn separately.
