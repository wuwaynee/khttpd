# Validation

## Correctness workflow

[Source a7b6a83](https://github.com/wuwaynee/khttpd/tree/a7b6a83d0d15385eb17a4d7aff8e59e81f669c25) passed [all four correctness jobs](https://github.com/wuwaynee/khttpd/actions/runs/44006305301): the shared/userspace suite and three x86-64 kernel-guest jobs. The [workflow](../.github/workflows/ci.yml) pins action revisions and fails on compilation or test errors.

| Check | Coverage |
|---|---|
| C compilation | C11 userspace builds with -Wall, -Wextra, -Werror; kernel modules with W=1 |
| Shared engine | Protocol syntax, framing, fragmented/pipelined streams, partial/zero/failed sends and simulated deadlines |
| Live socket integration | Shared/sharded listeners, both buffer policies, concurrent clients, half-close, idle/partial/slow-drip deadlines and active shutdown |
| Evidence pipeline | Python tests covering malformed/error logs, units, checksums, recomputation, missing/duplicate cells, invalid metrics, nonzero exit status and incomplete manifests |
| wrk capture smoke | One-second userspace status/body/count validation, manifest loading and summarization |
| ASan / UBSan | Shared engine unit suite compiled in userspace |
| libFuzzer | Time-limited parser smoke test, maximum input length 8193 |
| Kernel guests | Ubuntu generic Linux 5.15 and 6.8 families plus the runner's preinstalled kernel, with matching build headers |

The guest tests exercise (shards,prealloc) = (1,1), (2,0), (2,1); reject invalid parameters; inject partial-start failure after 0, 1 and 8 workers; unload while eight incomplete connections are live; check a two-response connection cap; test idle/partial-header/slow-drip deadlines; and repeat load/unload ten times. Guest logs require KH_VM_PASS and are checked for BUG/WARNING/panic messages.

The runner-kernel job additionally builds the historical adapter and tests its startup in the guest. Four client threads send traffic during twelve injected initialization failures (three rounds at 0, 1, 8 and 16 prepared acceptors). The test requires actual TCP connections, zero response bytes from failed loads, no remaining module/tasks, and successful load/serve/unload before and after the failures. Other kernel jobs exercise the bounded module independently.

[Archived guest excerpts](../artifacts/validation/README.md) retain output from [run 43988760634](https://github.com/wuwaynee/khttpd/actions/runs/43988760634), source fe02cfb0030c4f42de6134134af2ed3e040f6202. That run used Linux 5.15.0-203-generic, 6.8.0-149-generic and 7.0.0-1017-azure. Exact versions for other runs are in their job artifacts.

## Native performance evidence

The [five-repeat primary](https://github.com/wuwaynee/khttpd/actions/runs/44002074047) and [independent matched controls](https://github.com/wuwaynee/khttpd/actions/runs/44003646281) completed successfully on native Linux VMs. [PERFORMANCE.md](PERFORMANCE.md) records their sources, environments, results, variability and permanent raw archives.

Archive SHA-256 digests matched GitHub's artifact digests. Recomputing both summaries from the hash-checked raw logs produced the archived JSON exactly. These measurements are separate from the correctness workflow's one-second smoke test.

## Re-run

~~~bash
make test
make sanitize
clang -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined \
  tests/fuzz_protocol.c http_protocol.c -o build/fuzz
./build/fuzz -max_total_time=20 -max_len=8193
~~~

For an isolated module runtime check on an Ubuntu host:

~~~bash
sudo apt-get install -y build-essential linux-headers-generic linux-image-generic \
  busybox-static cpio qemu-system-x86 ripgrep
image=$(printf '%s\n' /boot/vmlinuz-*-generic | sort -V | tail -n 1)
release=$(basename "$image")
release=${release#vmlinuz-}
make module KDIR="/lib/modules/$release/build"
mkdir -p build
sudo install -m 0644 "$image" build/vmlinuz
./scripts/ci-vm.sh "$PWD/build/vmlinuz"
~~~

Only the guest loads khttpd.ko in this correctness check. The host needs sudo to copy the distro kernel image and create temporary initramfs device nodes. Native performance reproduction uses the separate commands in [PERFORMANCE_METHOD.md](PERFORMANCE_METHOD.md).

## Coverage limits

ASan/UBSan and fuzzing operate on the shared C engine in userspace. Guest tests use stock distribution kernels, without custom KASAN, KCSAN or lockdep instrumentation. fail_start_after tests deterministic partial-start cleanup rather than every allocator failure. Finite fuzzing and runtime tests do not establish race freedom or complete protocol conformance.

Optional perf/bpftrace captures were not executed for the published performance runs. The kernel/userspace performance matrix has not been run in full. Non-x86-64 architectures and kernels outside the recorded matrix need separate verification.
