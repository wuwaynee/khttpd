# Provenance and attribution

This repository extends the [NCKU/sysprog21 khttpd teaching project](https://github.com/sysprog21/khttpd). Its Git history preserves the original project and subsequent contributor changes.

- The MIT license retains the NCKU copyright notice alongside the notice for this extension.
- The inherited mempool change is commit 3213b2586c1397a56f1e3e0f5fddddcc2370c13c, signed off by Nick Huang. The bounded runtime uses explicit per-worker preallocation; the historical experiment adapter retains the mempool control.
- The former htstress.c load generator was derived from Roman Arutyunyan's htstress and carried a BSD-2-Clause notice. Its original notice remains in Git history. Current benchmarks use wrk.
- The hardened module uses the bounded parser in http_protocol.c and does not download a parser during build. The separate historical adapter vendors nodejs/http-parser 2.9.4 at commit 2343fd6b5214b2ded2cdcf76de2bf60903bb90cd; its MIT license is retained in bench/diagnose/legacy/LICENSE-http-parser.
- The removed interactive commit hook credited tommarshall/git-good-commit; that source and its notice remain in Git history.

The module's Dual MIT/GPL MODULE_LICENSE metadata declares GPL compatibility to the Linux module loader. The distributed project source is under the MIT license in LICENSE. kHTTPD is maintained as an independent, out-of-tree module.
