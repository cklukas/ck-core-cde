---
title: CI and documentation
description: Continuous builds and documentation publishing.
---

# CI and documentation

GitHub Actions checks out the current `master` branch of CK Core Motif, builds
an isolated ABI 9 toolkit prefix, and compiles and tests CDE against that exact
prefix. The CDE configure checks prevent a system Motif library from satisfying
the build accidentally.

Documentation builds use the latest CKDocs version, 0.9.0. GitHub pins current
source revision `1f46455b702bc955a2eab63824afe2e062fb04a4`; rpi4 pins the 0.9.0
release revision `05d5c029c7535411531c4d5a5cb6867bfd641385` available from its local
CK Git Hosting sister repository. A push to `master` publishes the verified
site to both GitHub Pages and the rpi4 Pages service.

Pull requests build the same documentation without deploying it.
