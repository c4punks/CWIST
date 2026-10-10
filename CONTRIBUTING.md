# Contributing to CWIST

## Branch your work off `dev`, not `main`

`dev` is the integration branch: this is where feature work, fixes, and
most day-to-day changes land. `main` is the release branch: it only
receives changes via a maintainer cherry-picking specific, already-reviewed
commits over from `dev`.

**Open pull requests against `dev`.** Do not open a PR against `main`
directly, even for something that looks like a small, obviously-correct
fix.

This isn't just a style preference: `main` carries the release line and
receives changes only as cherry-picked, already-reviewed commits from
`dev`, so a PR opened against the wrong base cannot simply be retargeted.

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
- Follow the writing rules below for issues, PRs, and docs.

## Writing rules for issues, PRs, and docs

These apply to anything a reviewer or user will read: issue bodies, PR
descriptions, commit messages, and files under `docs/`.

**Plain text, ASCII first.** No em dashes: use a plain `-` between
spaces or restructure the sentence. Write RFC references as
`RFC 6455 section 5.4`, not with a section sign. Avoid other non-ASCII
punctuation (smart quotes, arrows, ellipsis characters) and non-ASCII
characters in prose unless there is a concrete reason; source code
identifiers stay as they are.

**Match the artifact to what it describes.**
- An issue describes the problem: observed behavior, how to reproduce
  it, and what you expected instead.
- A PR describes the change: what it does, how it was verified
  (commands run, numbers measured), and what it deliberately does not
  do.
- A doc page describes the current state of the code as it exists in
  the tree it ships with, not the state it may reach later.

**No speculation presented as fact.** Do not fill an issue, PR, or doc
with hypotheses about root causes, future performance, or planned work
that has not been done and measured. If a hypothesis is worth recording,
label it as one ("unverified hypothesis: ...", "possible follow-up:
...") and keep it clearly separated from what was observed. Anything
stated as a result must come from an actual run, test, or build that
anyone can repeat.

An example of the bar to hit: "c=512, wrk t4, 3 runs, p99.999 went
from 60.4 ms to 26.2 ms, zero errors on both sides" is a PR sentence.
"This should improve tail latency under load" is not, until the runs
exist.

## AI assistance policy

The early CWIST codebase was written by hand, quickly, with AI as an
assistant after the fact. The project rule going forward: design first,
write the code, then use AI to assist. Starting from AI output and
hoping to shape it into a design is not how changes are made here.

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

## Releases and patch releases

- Milestone releases (`vX.Y`) follow `ROADMAP.md`.
- Patch releases (`vX.Y.Z`) are cut from `main`. `main` only receives
  reviewed changes cherry-picked from `dev`, so a patch release carries
  everything merged since the previous tag: bug fixes, and any experimental
  work already on `main`.
- **Emergency patch:** a serious bug in a released version is fixed and
  released from `main` right away instead of waiting for the next milestone.
  Serious means any of: a crash or hang in the default configuration, data
  loss or corruption, a security vulnerability, a build that produces a
  broken binary, or a severe availability regression (for example, a server
  that stops serving connections under normal load). The fix lands on `dev`
  and `main` as usual.
- A release is tagged only when:
  - every CI workflow on the commit to be tagged has passed, and
  - the source archive builds and tests clean without Git metadata:
    `make dist VERSION=X.Y.Z` from a checkout whose submodules match the
    recorded gitlinks, then `make WERROR=1` and `make WERROR=1 test` in the
    unpacked tree.
- **Public API baseline (required for every major release, starting with
  v4.0):** regenerate `docs/api/v4.0-api-baseline.txt` with
  `make api-baseline` and review the resulting diff as an intentional API
  change. Any symbol addition, removal, or rename in the diff must be a
  deliberate, recorded API decision before the cut — an unexpected diff is
  a release blocker.
- **Soak run (required before a major-release cut, starting with v4.0):**
  the release commit must pass the soak defined in `docs/soak-testing.md`
  (`scripts/ci/soak.sh` with the release-candidate duration); see ROADMAP.md
  "CWIST v4.0 Readiness" for the exit criteria.
- Release notes list every change since the previous tag: each fix with its
  impact and the affected versions, every new API or knob, and every
  feature included, marked experimental when it is. A release that carries
  features is not described as bug-fix-only.
- After publishing, re-pin `packaging/homebrew/cwist.rb` and the tap
  (`c4punks/homebrew-cwist`) to the released archive.

## Reporting a vulnerability

Open an issue titled `[CVE/<component>]` describing the finding (see
existing issues tagged that way for the expected level of detail -
affected file/line, reproduction, suggested fix), or reach out in the
[Discord](https://discord.gg/6F8HDmNAPg).
