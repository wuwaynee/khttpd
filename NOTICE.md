# Provenance and attribution

This repository extends the [NCKU/sysprog21 khttpd teaching project](https://github.com/sysprog21/khttpd). Its imported history includes work by the original project and multiple contributors; retaining that history does not make all of those changes Wayne Wu's original work.

- The MIT license retains the NCKU copyright notice alongside the notice for this extension.
- The inherited mempool change is commit 3213b2586c1397a56f1e3e0f5fddddcc2370c13c, signed off by Nick Huang. The current implementation replaces that mechanism with an explicit, bounded preallocation experiment.
- The inherited load generator htstress.c was derived from Roman Arutyunyan's htstress and carried a BSD-2-Clause notice. It was removed from the active tree during the reliability revision; its original notice remains in Git history. It is not the current benchmark tool.
- The old build downloaded nodejs/http-parser. The current build does not use or download that library. The bounded parser in http_protocol.c handles only the subset documented in docs/DESIGN.md.
- The removed interactive commit hook credited tommarshall/git-good-commit; that source and its notice remain in Git history.

The module's "Dual MIT/GPL" MODULE_LICENSE metadata declares GPL compatibility to the Linux module loader. The distributed project source is under the MIT license in LICENSE.

This is an independent, out-of-tree Linux experiment. There is no claim that these changes have been accepted into Linux upstream. Describe personal contributions using the diff and tested revision, rather than the entire inherited history.
