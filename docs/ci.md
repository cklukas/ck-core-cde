---
title: CI and documentation
description: Continuous builds and documentation publishing.
---

# CI and documentation

GitHub Actions checks out the batch revision in `.ckgit/motif-revision`, builds
an isolated ABI 9 toolkit prefix, and compiles and tests CDE against that exact
prefix with GCC 14 and Clang. The CDE configure checks prevent a system Motif
library from satisfying the build accidentally. The optional CK Core FS
consumer reconfigures the completed compiler-specific CDE tree in both GitHub
and rpi4 lanes, so its focused dtfile qualification retains the generated tools
and libraries established by the full build.

The rpi4 sister checkout is pinned to the same revision, and its build checks
that the pin matches the recorded file. Advance both pins only when starting
the next repair batch, so concurrent toolkit changes do not alter validation
of a batch already in progress. Each compiler lane also generates its required
`en_US.UTF-8` document locale inside the isolated build root; a minimal runner
does not need that locale installed system-wide.

The installed filesystem qualification deliberately runs its preference worker
under a unique directory beneath the runner's private home. The rpi4 driver
first verifies that ckgit's dedicated ephemeral home belongs to the lane user
and normalizes it to mode `0700`; this avoids inheriting a group-writable runner
umask. Both hosts therefore provide an ownership-enforcing, non-group-writable
ancestor chain while leaving the production refusal of unsafe shared temporary
storage intact. The fixture is removed when the lane exits.

Documentation builds use published ckdocs `master` revision
`7e3ee5eb57a2cef2e07f9e9fe5c4e2822502d322` on both GitHub and rpi4. A push to
`master` publishes the verified site to GitHub Pages and the rpi4 Pages service.

Pull requests build the same documentation without deploying it.

## Sanitizers and fuzz corpus replay

Two further GitHub jobs run next to the compiler lanes.

The `sanitizers` job builds the batch CK Core Motif revision and CDE with
`--enable-sanitizers` (AddressSanitizer and UndefinedBehaviorSanitizer) using
GCC 14 and Clang 19, with at most six parallel jobs, and runs `make check`. A
sanitized toolkit needs a sanitized consumer, so Motif and CDE use the same
switch and list. `ASAN_OPTIONS=detect_leaks=0` keeps the build-time generators
from reporting leaks as findings, and `UBSAN_OPTIONS=print_stacktrace=1` prints
the stack of each undefined-behavior report. Clang's `-fsanitize=function`
check is suppressed with `function:*`, because Xt and Motif call callbacks
through cast function pointers by design; every other check stays on. A failed
run uploads the configure, build and test logs as the artifact
`sanitizer-logs-<compiler>`.

The `fuzz-replay` job runs `python3 cde/fuzz/run-fuzz.py --all --replay` with
Clang 19 and libFuzzer. Each target runs once over every file of its corpus
directory, without mutation. The corpora hold the regression input of every
fixed defect, so a failure means a fixed defect has returned. The same job runs
on rpi4. Run it locally the same way; see `cde/fuzz/README` for the host
requirements. On Linux a target whose libraries are missing is skipped with a
message instead of failing.

## Topic integration

Group related fixes into cohesive topic commits, including their regression
tests, changelog entries and relevant documentation. After validation, integrate
through the shared primary branch and push to both rpi4 (`ckgit`) and GitHub
(`origin`). Coordinate with concurrent work, preserve unrelated changes, avoid
force-pushes, and verify that both remotes contain the integrated revision.
Creating a release requires a separate explicit request.
