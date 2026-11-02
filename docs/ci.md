---
title: CI and documentation
description: Continuous builds and documentation publishing.
---

# CI and documentation

GitHub Actions checks out the current `master` branch of CK Core Motif, builds
an isolated ABI 9 toolkit prefix, and compiles and tests CDE against that exact
prefix. The CDE configure checks prevent a system Motif library from satisfying
the build accidentally.

Documentation builds use published ckdocs `master` revision
`1f46455b702bc955a2eab63824afe2e062fb04a4` on both GitHub and rpi4. A push to
`master` publishes the verified site to GitHub Pages and the rpi4 Pages service.

Pull requests build the same documentation without deploying it.

## Topic integration

Group related fixes into cohesive topic commits, including their regression
tests, changelog entries and relevant documentation. After validation, integrate
through the shared primary branch and push to both rpi4 (`ckgit`) and GitHub
(`origin`). Coordinate with concurrent work, preserve unrelated changes, avoid
force-pushes, and verify that both remotes contain the integrated revision.
Creating a release requires a separate explicit request.
