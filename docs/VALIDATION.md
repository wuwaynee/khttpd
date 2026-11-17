# Validation record

The fully passing four-job engineering run verified commit **fe02cfb0030c4f42de6134134af2ed3e040f6202**:

[GitHub Actions run 43988760634](https://github.com/wuwaynee/khttpd/actions/runs/43988760634)

| Check | Actual result |
|---|---|
| Userspace C compilation | -std=c11, -Wall, -Wextra, -Werror; pass |
| Shared engine unit tests | Protocol syntax, framing, fragmented/pipelined streams, partial/zero/failed sends, deadline simulation; pass |
| Userspace socket integration | Shared and sharded listeners; both buffer policies; request/response checks, concurrency, half-close, real idle/partial/slow-drip deadlines and active shutdown; pass |
| Evidence parser tests | 4 Python test methods, including malformed/incomplete/error logs, units, checksums and missing cells; pass |
| Actual wrk pipeline smoke | One-second userspace capture, status/body/count validation, manifest loading and summarization; pass; not performance evidence |
| ASan and UBSan | Shared engine unit suite; pass |
| libFuzzer smoke | 3,018,675 executions in the time-limited run, maximum input length 8193; pass; finite smoke test, not exhaustive |
| Ubuntu 22.04 guest | Linux 5.15.0-203-generic x86-64; module build and QEMU runtime pass |
| Ubuntu 24.04 generic guest | Linux 6.8.0-149-generic x86-64; module build and QEMU runtime pass |
| Ubuntu 24.04 runner-kernel guest | Linux 7.0.0-1017-azure x86-64; module build and QEMU runtime pass |

Each guest executed protocol/integration checks with configurations (shards,prealloc) = (1,1), (2,0), (2,1); rejected invalid parameters; exercised initialization failure after 0, 1 and 8 workers; unloaded while eight incomplete connections were live; checked a two-response connection cap; exercised real idle, partial-header and slow-drip deadlines; and repeated load/unload ten times. All three logs ended with KH_VM_PASS and the guest dmesg checks found no BUG/WARNING/panic matches.

[Guest runtime excerpts](../artifacts/validation/README.md) preserve actual output from this run. Full job logs and artifacts are linked from GitHub Actions. The checked workflow also pins action revisions. The later documentation/evidence update does not alter this tested C implementation.

## Latest source and evidence checks

The later source **9ee238ad74fb65368d876fd56228a0944c307147** also passed all four [correctness jobs](https://github.com/wuwaynee/khttpd/actions/runs/44003653925): shared/userspace checks plus the three guest-kernel matrix jobs. The Python suite now has seven test methods, including the diagnostic summarizer's recomputation, tamper/missing/duplicate/incomplete rejection and retained socket-error accounting. Synthetic fixture values are not performance measurements.

The native [five-repeat primary](https://github.com/wuwaynee/khttpd/actions/runs/44002074047) and [independent matched controls](https://github.com/wuwaynee/khttpd/actions/runs/44003646281) both completed successfully. Their published raw archive SHA-256 digests matched GitHub's artifact digests, and local recomputation of both summaries matched the archived JSON exactly. Exact performance environments and scope are in [PERFORMANCE.md](PERFORMANCE.md).

## Re-run

~~~bash
make test
make sanitize
# Optional bounded fuzz smoke with clang:
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

Only the guest loads khttpd.ko. The host needs sudo to copy the distro kernel image and create temporary initramfs device nodes.

## Limits

The separate [native historical-performance reproduction](PERFORMANCE.md) now runs repeated before/after throughput and latency measurements. Its source, hardware, raw data and method are distinct from the correctness jobs recorded above.

These are correctness and workflow checks. The real wrk smoke is too short and runs on shared CI hardware, so it supports no throughput/latency improvement claim. No controlled kernel/user performance matrix has been run during this revision.

ASan/UBSan/fuzzing operate on the shared C engine in userspace. The guest uses stock distribution kernels, not KASAN, KCSAN, lockdep-enabled custom kernels or allocation fault-injection builds. fail_start_after checks deterministic partial-start cleanup; it does not simulate every allocator failure.

Optional bpftrace/perf captures were not executed. Non-x86-64 architectures and kernels outside the recorded matrix are not verified. The source's acceptance by a compiler and the finite tests do not prove security or race freedom.
