# Agent instructions (CLAUDE.md = AGENTS.md)

If STATE.md exists in this checkout (maintainer machines only — it is not
published), always read it completely before planning or making changes. Do
not rely only on its Current state or milestone sections: read the entire
append-only Log, and treat the newest log entries as authoritative when
older sections are stale.

Before making fundamental changes, always check whether someone on the
internet has already done what we're trying to do.

Use every available means instead of guessing. Instead of brute-forcing
parameters against an unknown library, clone it and check directly.

Instead of hacks and crutches, solve problems fundamentally.

Use every available resource — sources, etc. If some command is used very
often via sudo mcp, get NOPASSWD rights for it.

## Two different "issue #11" — always qualify the repo

This fork has its own issues **and** the upstream has its own, and the numbers
collide. `Shiro836/waydroid-nvidia#11` is "Waydroid UI crashing when opening
certain apps" (a LINEAR dma_buf / `CoglTexture2D` bind problem).
`quinovax/waydroid-nvidia#11` is "Launching a GLES game triggers Xid-69
(0xC997/0x274) and the device-lost path leaves the guest in an unrecoverable
SurfaceFlinger crash-loop". Same number, unrelated bugs.

Note also that the issue numbers in this fork's commit history (`#7`, `#9`,
`#10`, ...) are **this fork's** numbers, not upstream's.

Therefore:

- Remotes follow the git convention: `origin` = `quinovax/waydroid-nvidia`
  (this repo), `upstream` = `Shiro836/waydroid-nvidia`. Do not swap them.
- `gh` has its default repository set to `quinovax/waydroid-nvidia`, so a bare
  `gh issue view 11` resolves here. Keep it that way (`gh repo set-default
  quinovax/waydroid-nvidia`).
- In commit messages, issues, docs and user-facing text, **always write the
  fully qualified reference** — `quinovax/waydroid-nvidia#11` or
  `Shiro836/waydroid-nvidia#11` — never a bare `#11`. A bare number is
  ambiguous here and is the single most likely way to work on the wrong bug.

## Branch model — do not push daily work to main

- **`dev` is the working branch.** Every ordinary commit lands here, and `dev` is
  expected to be ahead of `main`.
- **`main` is the release branch.** It only moves when a release is cut: `dev` is
  merged/ff-integrated into `main` and a tag is pushed. Between releases `main`
  sits on the last release commit.
- So the normal push is `git push fork dev`. **Never `git push fork dev:dev
  dev:main`** — that publishes unreleased work and breaks the release branch.
- Before touching a remote branch, check what it actually points at:
  `git ls-remote --heads fork`, `git log --oneline <tag>..<branch>`.
  `v0.1.2` is an annotated tag, so compare with `git rev-parse v0.1.2^{commit}`
  rather than `git rev-parse v0.1.2` (the latter yields the tag object).
- If `main` was pushed by mistake, restore it to the last release commit with a
  lease-protected force push:
  `git push --force-with-lease=main:<current-remote-sha> fork <tag>^{commit}:refs/heads/main`
  (plain `--force` risks clobbering someone else's push; the bare
  `--force-with-lease=<name>:<sha>` form is rejected as non-fast-forward here).

## For agents working from a public clone

- Orientation: README.md, then docs/building.md (repo layout, per-component
  recipes, pins), docs/architecture.md, docs/transport-design.md.
- Everything builds locally: `packaging/reproduce.sh` runs the same
  clone→apply→build path CI does. Validate hermetically (a container with
  CI parity) before pushing — do NOT use CI as a remote grep/build/debug
  service.
- patches/ is generated from working trees (dev/sync-patches on the
  maintainer box) — edit patches only by regenerating, never by hand.
- src/ files are canonical even when build scripts copy them into upstream
  trees (build/*/build.sh may overwrite tree copies at build time).
- Tests select GPUs by vendor/driver-id, never by /dev/dri node paths;
  tests/ is a portable regression suite, keep it host-agnostic.

Working recipes (dev loop, guest commands, benches, deploy rules):
docs/dev-workflow.md.
