# Incomplete predecessor — excluded from all headline results

[Workflow run 44000139898](https://github.com/wuwaynee/khttpd/actions/runs/44000139898), source c3dcc62427164aa72c0856c00afcaf7245aef13f, completed 15 individual timed cells but failed module cleanup. The backlog-100 historical diagnostic left idle legacy connection workers holding module references. The runner refused to force-unload; manifest complete is false, and the summarizer rejects this dataset.

A common 10-second accepted-socket timeout now bounds idle legacy workers; it is longer than wrk's 2-second timeout and applies to every recovered historical configuration. The primary experiment was restarted in full at c1dda1a. No partial cell here contributes to its medians. The failure is preserved to make the lifecycle diagnosis and experiment selection auditable.

[Original raw artifact](raw.zip) and [SHA-256](raw.zip.sha256) were downloaded from GitHub Actions artifact 14292865691. The digest matched the artifact API. Extract the archive to inspect its manifest and individual logs; it deliberately has no valid aggregate headline summary.
