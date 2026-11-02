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
- Commit locally when the requested topic is complete. Do not push, publish or
  create a release unless the user explicitly asks for that separate action.

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

- Build CDE and the latest CK Core Motif as one matched native stack. Use an
  isolated Motif prefix and verify that runtime processes map the intended
  `libXm.so.9`, not a system substitute.
- Add or update focused regression coverage for behavior changes, then run the
  relevant component checks. For cross-cutting changes, run the full
  `make check` suite and the desktop smoke test where the target supports it.
- A successful compile alone is not sufficient for UI work. Inspect the actual
  user-facing window from the freshly built process before reporting a visual
  change as complete.
- Use at most six parallel build or test jobs per task.
