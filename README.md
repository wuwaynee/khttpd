# khttpd — An experimental HTTP server inside the Linux kernel

> **Status:** Research project for systems programming and performance exploration.  
> **Goal:** Learn, measure and reason about the trade‑offs of handling HTTP requests in kernel space.

---

## Why this project?

Most HTTP servers run in user space, with mature ecosystems and great tooling. **khttpd** explores the opposite design point: what happens if we move a tiny HTTP stack into the Linux kernel and minimize context switches and data copies? The codebase is intentionally small so that the design can be audited, reasoned about, and profiled.

---

## Highlights

- **Minimal kernel module:** a tiny HTTP/1.1 (GET) path with straightforward parsing and response.
- **Performance-minded experiments:** `keep-alive`, `SO_REUSEPORT`, and memory pooling to reduce fragmentation and allocation latency.
- **Observability first:** `printk` traces, a simple BPF script (`tools/tcp_counters.bt`), and saved run artifacts under `artifacts/`.
- **Clean code layout:** each piece is easy to read, modify, and profile.

---

## Results (Summary)
On a local 4T/256C `wrk` run, requests/sec improved from **3.4k → 37.7k (~11×)**, median latency from **74.8ms → 3.0ms (~25×)**, p99 from **113ms → 12ms (~9.4×)**. See `artifacts/README.bench.md` for commands and raw outputs.

---

## Directory Layout
```text
.
├─ artifacts/       # Benchmark outputs (e.g., wrk baselines and notes)
├─ compat/          # Optional portability / vendored code (kept minimal)
├─ scripts/         # Dev scripts and Git hooks (pre-commit, pre-push, etc.)
├─ tools/           # Diagnostics (e.g., tcp_counters.bt for BPF)
├─ .clang-format    # C style rules
├─ .gitignore
├─ LICENSE
├─ Makefile
├─ README.md
├─ http_server.c/.h # Minimal HTTP request/response logic
└─ main.c           # Module init/exit and wiring
```
If you use the userspace probe `htstress.c`, it lives at the repository root and can be built independently for smoke tests.


## Prerequisites

- Linux headers that match your running kernel
- `make`, `gcc`/`clang`
- Optional: [`wrk`](https://github.com/wg/wrk) for micro-benchmarks
- Root privileges to load/unload the module

---

## Build

```bash
make -j"$(nproc)"
```
This should produce the kernel module (e.g., `khttpd.ko`).

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

---

## Quick Bench
```bash
# Example (adjust threads/conn/duration to your machine)
wrk -t4 -c128 -d30s http://127.0.0.1:8081/
```

---

## Observability

- `printk` traces in the module for control-flow and error paths.
- `tools/tcp_counters.bt`: a simple BPF helper to peek at TCP counters while serving requests.
- Keep flamegraphs and notes under `artifacts/` when profiling.

---

## Reproducibility

All commands and raw outputs live under `artifacts/`. A compact walk-through is in `artifacts/README.bench.md`.

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
CI is kept minimal to avoid noise on public runners. See `.github/workflows/ci.yml`

---

## Roadmap
- Tighten resource lifetime around connection close.
- Experiment with queueing models (e.g., CMWQ).
- Improve memory management for request/response buffers.
- Optional: add a small request queue or canned-response cache.

---

## Safety & Scope
This module is research-only and not production-hardened. It intentionally trades completeness and safety checks for clarity and controlled experiments.

---

## License
MIT, see `LICENSE`