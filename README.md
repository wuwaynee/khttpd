# khttpd — An experimental HTTP server inside the Linux kernel

> **Status:** Research project for systems programming and performance exploration.  
> **Goal:** Learn, measure, and reason about the trade‑offs of handling HTTP requests in kernel space.

---

## Why this project?

Most HTTP servers run in user space, with mature ecosystems and great tooling. **khttpd** explores the opposite design point: what happens if we move a tiny HTTP stack into the Linux kernel and minimize context switches and data copies? The project is deliberately small so that the design can be audited, reasoned about, and profiled.

---

## Highlights

- **Minimal kernel module** implementing a tiny HTTP/1.1 subset (GET) with straightforward request parsing.
- **Focused on fundamentals:** socket setup, accept loop, parsing, response writeout, teardown.
- **Strong observability hooks:** BPF scripts (e.g., `tools/tcp_counters.bt`) and simple bench artifacts in `artifacts/`.
- **Clear guardrails:** Not production‑ready; designed for controlled experiments and interviews/demos.

---

## Quick Start

### Prerequisites
- Linux headers for your running kernel
- `make`, `gcc`/`clang`
- Optional: [`wrk`](https://github.com/wg/wrk) for micro-benchmarks

### Build

```bash
make

This should produce the kernel module (e.g., `khttpd.ko`).  
If your output name differs, adjust the commands below accordingly.

---

## Load & Test

```bash
# Load the module on port 8081 by default (adjust parameter if your module uses a different name)
sudo insmod khttpd.ko port=8081

# Basic check
curl -v http://127.0.0.1:8081/

# Inspect logs
dmesg -w

## Unload
```bash
sudo rmmod khttpd

If your environment or module name differs, please check the Makefile and module parameters.

## Directory Layout
.
├─ artifacts/          # Benchmark outputs (e.g., wrk baselines)
├─ compat/             # Portability or vendored code (e.g., http_parser.c/.h if vendored)
├─ scripts/            # Dev scripts and Git hooks (pre-commit, pre-push, etc.)
├─ tools/              # BPF scripts and diagnostics (e.g., tcp_counters.bt)
├─ .clang-format       # Style rules
├─ .gitignore
├─ LICENSE
├─ Makefile
├─ README.md
├─ http_server.c/.h    # Minimal HTTP flow
└─ main.c              # Module init/exit and wiring


## Run a quick bench
```bash
# Example (adjust threads/conn/duration to your machine)
wrk -t4 -c128 -d30s http://127.0.0.1:8081/

Artifacts from previous runs live in artifacts/ for traceability.


## Architecture (high level)
- Accept path: minimal socket accept loop, per-connection worker

- Parsing: a tiny HTTP/1.1 GET parser (either vendored http_parser.[ch] or a compact internal parser)

- Response path: simple static reply, short-circuit I/O

- Observability: printk traces and BPF helpers for latency/counters

Focus is clarity over features to keep reasoning and profiling simple.


## Development Workflow
```bash
# Install Git hooks (format, basic checks)
./scripts/install-git-hooks

# Format
clang-format -i $(git ls-files '*.c' '*.h')

# Build
make -j$(nproc)

CI: see .github/workflows/lint.yml for format check and build.

## Roadmap
- Resource lifetime fixes around connection close

- Queueing model improvements (e.g., CMWQ)

- Smarter memory management

- Simple request queue or cache for canned responses


## License
MIT. See LICENSE.

## Acknowledgements
- http_parser
 (Node.js contributors)

- hstress
 (Roman Arutyunyan) – basis for hstress.c