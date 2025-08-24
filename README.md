# khttpd — An experimental HTTP server inside the Linux kernel

> **Status:** Research project for systems programming and performance exploration.  
> **Goal:** Learn, measure and reason about the trade‑offs of handling HTTP requests in kernel space.

---

## Why this project?

Most HTTP servers run in user space, with mature ecosystems and great tooling. **khttpd** explores the opposite design point: what happens if we move a tiny HTTP stack into the Linux kernel and minimize context switches and data copies? The project is deliberately small so that the design can be audited, reasoned about, and profiled.

---

## Highlights

- **Tiny kernel module** that serves a minimal HTTP/1.1 subset (GET) with straightforward request handling.
- **Back-to-basics flow:** socket setup, accept loop, parse, respond, teardown.
- **Observability first:** BPF scripts (e.g., `tools/tcp_counters.bt`), printk traces, and preserved bench artifacts in `artifacts/`.
- **Clean boundaries:** clear code layout; easy to read and profile.

---


## Directory Layout
.

├─ artifacts/ # Benchmark outputs (e.g., wrk baselines from previous runs)

├─ compat/ # Placeholder for portability or vendored code (optional)

├─ scripts/ # Dev scripts and Git hooks (pre-commit, pre-push, etc.)

├─ tools/ # BPF scripts and diagnostics (e.g., tcp_counters.bt)

├─ .clang-format # C/C++ style rules

├─ .gitignore

├─ LICENSE

├─ Makefile

├─ README.md

├─ http_server.c/.h # Minimal HTTP request/response logic

└─ main.c # Module init/exit and wiring


If you use the userspace tool `htstress.c`, it lives at the repository root and can be built independently for quick smoke tests.


## Prerequisites

- Linux headers for your running kernel
- `make`, `gcc`/`clang`
- Optional: [`wrk`](https://github.com/wg/wrk) for micro-benchmarks
- Optional: root privileges (to load/unload the kernel module)

---

## Build

```bash
make -j"$(nproc)"
```
This should produce the kernel module (e.g., `khttpd.ko`).
If your output name differs, adjust the commands below accordingly.

---

## Load & Test
```bash
# Load (default port 8081 unless you changed the module parameter)
sudo insmod khttpd.ko port=8081

# Basic check
curl -v http://127.0.0.1:8081/

# Kernel log
dmesg -w
```

## Unload
```bash
sudo rmmod khttpd
```
If your environment or module name differs, check the `Makefile` and module parameters.

---

## Quick Bench
```bash
# Example (adjust threads/conn/duration to your machine)
wrk -t4 -c128 -d30s http://127.0.0.1:8081/
```
Previous run artifacts (if any) are kept under `artifacts/` for traceability.

---

## Observability

- `printk` traces in the module for control-flow and error paths.
- `tools/tcp_counters.bt`: simple BPF helper to peek at TCP counters while serving requests.
- Keep your own notes and flamegraphs alongside `artifacts/` when you profile.

---

## Development
```bash
# Install Git hooks (format/basic checks)
./scripts/install-git-hooks

# Format C/C++ files
clang-format -i $(git ls-files '*.c' '*.h')

# Build
make -j"$(nproc)"
```
CI is kept minimal to avoid noise on public runners; see `.github/workflows/ci.yml`

---

## Roadmap
- Tighten resource lifetime around connection close.
- Experiment with queueing models (e.g., CMWQ).
- Improve memory management for request/response buffers.
- Optional: add a small request queue or canned-response cache.

---

## License
MIT, see `LICENSE`