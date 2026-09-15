# Contributing to CWIST

## Branch your work off `dev`, not `main`

`dev` is the integration branch: this is where feature work, fixes, and
most day-to-day changes land. `main` is the release branch: it only
receives changes via a maintainer cherry-picking specific, already-reviewed
commits over from `dev`.

**Open pull requests against `dev`.** Do not open a PR against `main`
directly, even for something that looks like a small, obviously-correct
fix.

This isn't just a style preference: `main` and `dev` have diverged to the
point of sharing **no common git history** (no merge-base between them).
That has a concrete consequence: once a PR is opened against the wrong
base, GitHub cannot simply change its base branch afterward. Attempting to
retarget a `main`-rooted PR onto `dev` silently **closes the PR** instead
of moving it, because there's no shared ancestry for the two branches to
be diffed against. Recovering from this means recreating the PR's commits
from scratch against `dev` (`git cherry-pick`, preserving the original
author) and opening a new PR: extra work for both the contributor and
whoever reviews it, entirely avoidable by branching from the right place
the first time.

A CI workflow (`.github/workflows/redirect-main-prs-to-dev.yml`) will
attempt this recreation automatically for any PR opened against `main`: it
cherry-picks your commits onto a fresh branch off `dev`, opens a new PR
there, and closes the original with a link to it, preserving your
authorship. If your commits don't cherry-pick cleanly against `dev`'s
current state, the bot labels the PR `needs-manual-dev-redirect` and asks
a maintainer to sort it out by hand. Either way, this is a safety net for
mistakes, not a substitute for branching from `dev` correctly.

```sh
git clone https://github.com/c4punks/CWIST.git
cd cwist
git checkout dev
git checkout -b your-branch-name dev
# ... make changes ...
```

## Before opening a PR

- Build and run the tests that touch what you changed at minimum;
  `make test` runs the full suite (a `SANITIZE=address,undefined` build is
  also worth running for anything touching memory or concurrency:
  `make SANITIZE=address,undefined test`).
- Match the existing commit style: `type(scope): short description`. See
  `git log` for examples (`fix(http2): ...`, `docs(gc): ...`,
  `chore(deps): ...`).
- Reference the issue number your change addresses, if there is one.
- Keep comments and documentation ASCII. Write RFC references as
  `RFC 6455 section 5.4`, not with a section sign, and use a plain `-` or
  a restructured sentence instead of an em dash.

## A closed PR is not always a rejected one

Some accepted changes land by cherry-pick rather than the merge button.
GitHub then marks the PR **Closed**, not Merged, because the commit that
shipped has a different hash from the one on your branch.

This happens when `dev` has moved since you branched and your change needs
a conflict resolution the merge button cannot perform on its own. Cherry-
picking keeps you as the commit author, which a squash merge in that
situation would not do as cleanly.

When it happens you get:

- a comment naming the commit on both `dev` and `main`,
- the `merged-via-cherry-pick` label, so these are searchable,
- a note of anything that was changed while resolving, so you are not
  surprised by a difference between your patch and what shipped.

If a PR of yours is closed without that comment and label, it was not
taken. Ask in the issue or on Discord if it is unclear which happened.

## Reporting a vulnerability

Open an issue titled `[CVE/<component>]` describing the finding (see
existing issues tagged that way for the expected level of detail -
affected file/line, reproduction, suggested fix), or reach out in the
[Discord](https://discord.gg/6F8HDmNAPg).
