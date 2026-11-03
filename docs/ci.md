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

Documentation builds use published ckdocs `master` revision
`7e3ee5eb57a2cef2e07f9e9fe5c4e2822502d322` on both GitHub and rpi4. A push to
`master` publishes the verified site to GitHub Pages and the rpi4 Pages service.

Pull requests build the same documentation without deploying it.

## Topic integration

Group related fixes into cohesive topic commits, including their regression
tests, changelog entries and relevant documentation. After validation, integrate
through the shared primary branch and push to both rpi4 (`ckgit`) and GitHub
(`origin`). Coordinate with concurrent work, preserve unrelated changes, avoid
force-pushes, and verify that both remotes contain the integrated revision.
Creating a release requires a separate explicit request.
