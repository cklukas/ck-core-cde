# CK Core CDE agent rules

These instructions apply to the entire repository. More specific `AGENTS.md`
files may add rules for their subtree, but must not weaken these requirements.

## Keep changes topical

- Treat one independently reviewable concern as one topic. A topic includes its
  implementation, tests, changelog entry, documentation and directly related
  build or CI changes.
- Commit completed topics separately. Do not mix unrelated fixes, broad
  formatting, opportunistic cleanup or several independent features in one
  commit.
- Keep a topic together: do not put its tests, documentation or changelog entry
  into later follow-up commits merely to make the implementation commit smaller.
- Use an imperative commit subject that describes the outcome. Do not create
  checkpoint, partial, temporary or known-broken commits.
- Before committing, review the complete staged diff, run the relevant checks
  and confirm that unrelated working-tree changes remain untouched.
- Commit completed, verified topics locally, then integrate through the shared
  primary branch and push to both rpi4 (`ckgit`) and GitHub (`origin`). Coordinate
  integration with other work, preserve unrelated changes, and never force-push.
  Related fixes may share one cohesive topic commit; do not split every small
  fix into its own commit. Record validation and confirm both remote revisions.
  Release creation remains a separate action requiring an explicit request.
- Publish the primary branch (`master`) with these commands and confirm each
  result:
  - rpi4: `ckgit publish --branch master --no-tags --dry-run` shows the plan; it
    must update only `refs/heads/master`. Run the same command with `--yes`
    instead of `--dry-run`, then compare `git ls-remote ckgit refs/heads/master`
    with `git rev-parse master`. `ckgit publish` never forces or deletes server
    refs, and the branch choice is not remembered.
  - GitHub: `git push origin master`, then compare
    `git ls-remote origin refs/heads/master` with `git rev-parse master`.
  - `ckgit sync --repo .` is a different operation: it uploads every local branch
    and tag, including task branches, worktree branches and branches that belong
    to other sessions, so it is not how a finished batch is published. rpi4 is
    the owner's backup of the repositories, so run `ckgit sync --repo . --dry-run`
    and then `ckgit sync --repo .` when the owner asks for a full backup, and
    report what it uploaded. It never forces, and it skips a project whose local
    branch has diverged from the server (for example after a rebase of a branch
    that was already uploaded): upload the other branches with
    `ckgit publish --branch NAME ...` and report the diverged one. Publish a
    single other branch only when asked to, with `ckgit publish --branch NAME`.
    Do not rewrite a branch that is already on rpi4.
  - If a push is refused before Git reports anything (for example "permission
    denied" from the tool that runs the command), the cause is a permission rule
    of that machine, not Git or the remote. Report it and ask for the rule to be
    changed or for the push to be run by hand; do not look for another way to
    run the same push.

## Maintain the changelog

- `cde/CHANGES` is this repository's canonical changelog (the equivalent of a
  root `CHANGES.md`). Update it in every topic commit that changes behavior,
  user-visible output, dependencies, supported platforms, build or test
  behavior, CI, packaging, or substantial documentation.
- Add entries under the current date. Describe the user- or developer-visible
  result and the affected component; avoid implementation trivia and promises
  of unfinished work.
- A narrowly editorial typo correction may omit a changelog entry. Policy,
  workflow and documentation-structure changes are not typo corrections.

## Keep major changes documented

- A major update includes a new feature, a changed workflow or UI interaction,
  a new configuration or resource, an architectural or dependency change, and
  a material build, test, CI or platform change.
- Document major updates in the same topic commit, on the most relevant page
  below `docs/`. Update overview pages only with the concise value proposition;
  put behavior, configuration, limitations and examples on the appropriate
  detail page.
- Add a new detail page when no existing page owns the topic. Wire every new
  page into `ckdocs.yml` navigation and link it from the relevant overview or
  comparison table.
- Keep documentation factual and current. Do not market a fix for a regression
  introduced only by CK Core CDE as a product advantage.
- Use screenshots only when they clarify visible behavior. Capture the real,
  freshly built CDE session, inspect the result, exclude automation artifacts
  and retain enough provenance to identify the CDE and CK Core Motif revisions.
- Validate documentation with the repository-pinned/latest ckdocs using
  `ckdocs check --config ckdocs.yml --strict`. Build the rendered site once
  after a major documentation topic is complete and inspect the affected pages.

## Build and validation expectations

- At the start of each ten-round repair batch, fetch the latest published
  CK Core Motif primary branch into its own checkout and record the selected
  full revision in `.ckgit/motif-revision` and the rpi4 sister pin. Build one
  isolated prefix for that revision. Keep its source, headers, libraries and
  tools fixed throughout the batch, including final validation and publication.
  Concurrent toolkit updates are adopted only at the start of the next batch;
  integration failures from those updates belong to that later checkpoint.
  Do not repeatedly rebuild or revalidate a batch merely because Motif moves.
- Build CDE and the selected CK Core Motif as one matched native stack. Verify
  that runtime processes map the recorded prefix's `libXm.so.9`. CI must use the
  same recorded revision. Do not pull into another task's active checkout.
- Focus this audit on CDE. Treat the selected Motif revision as a dependency;
  do not review the toolkit's incoming changes or conduct a parallel Motif
  audit. If CDE behavior supplies evidence of a defect owned by Motif, send the
  `motif-bug-fix` peer a focused reproducer, expected behavior and pinned revision,
  and coordinate ownership instead of implementing a toolkit fix here.
- Retain this task's configured CDE and Motif build trees between batches.
  Rebuild incrementally: compile/link only changed inputs and their dependency
  closure, using generated header dependencies. Do not run routine `make clean`,
  recreate build directories, or rebuild the whole stack at each checkpoint.
  Keep the SDK prefix path stable. When updating its staged installation,
  preserve unchanged file bytes and timestamps so reinstalling identical
  headers does not invalidate unrelated consumers.
- Reconfigure only when configuration inputs/settings require it. If an ABI,
  compiler or configuration change invalidates a broad set of objects, establish
  and record that dependency reason before rebuilding that set. A new Motif
  commit alone is not a reason to clean CDE or rebuild every component.
- Work in validation batches of ten repair rounds. In each round, inspect the
  complete affected path, implement related fixes, and add focused regression
  coverage and documentation. Record the scope and outstanding questions, but
  defer compilation, test execution and documentation validation until the ten
  rounds are complete. Do not run a full build or suite after each small fix.
- After round ten, compile and relink all affected components, run the focused
  regressions together, and run the relevant checkpoint suite. For cross-cutting
  changes, run the full `make check` suite and the desktop smoke test where the
  target supports it. Investigate failures and repeat only the affected checks
  unless another broad checkpoint is warranted. Never report untested work as
  verified or publish it as an accepted fix.
- After successful batch validation, commit related changes in cohesive topic
  commits and integrate/push them as described above. Then immediately begin
  the next batch of repair rounds while the overall audit goal remains active.
- A successful compile alone is not sufficient for UI work. Inspect the actual
  user-facing window from the freshly built process before reporting a visual
  change as complete.
- Use at most six parallel build or test jobs per task.
