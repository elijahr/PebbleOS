# Pull request procedure (fork)

```{important}
This page applies to the `elijahr/PebbleOS` fork. It does not apply to the
canonical `coredevices/PebbleOS` repository.
```

Several sessions work on this fork at the same time. This page is the single
record of how they land work. Follow it exactly.

## Remotes

The fork uses two remotes:

| Remote | URL | Use |
| --- | --- | --- |
| `origin` | `https://github.com/elijahr/PebbleOS.git` | The fork. Push here. |
| `upstream` | `https://github.com/coredevices/PebbleOS.git` | Canonical. Never push here. |

Push all work to `origin`. Never push to `upstream`. The operator submits work
to the canonical repository manually.

The remotes were renamed on 2026-08-01. Before the rename, the fork remote was
named `elijahr`, and `origin` pointed at `coredevices`. Any instruction that
says `git push elijahr <branch>` is stale. The remote `elijahr` no longer
exists.

## Always pass `--repo` to `gh`

Pass `--repo elijahr/PebbleOS` to every `gh` command:

```shell
gh pr create --repo elijahr/PebbleOS --base <base-branch> --head <head-branch>
gh pr list --repo elijahr/PebbleOS
gh pr view <n> --repo elijahr/PebbleOS
```

The flag is required. It is required even though the remotes now have correct
names.

`gh` selects a base repository from the remotes when you do not pass `--repo`.
That selection has its own precedence rules, and it can prefer a remote named
`upstream`. Such a remote now exists, and it points at `coredevices`. A command
without `--repo` can therefore act on the canonical repository.

The `--repo` flag does not read the remotes. This is what makes it correct
after a rename, and correct in any worktree.

## Verify the pull request after you create it

A pull request that targets `coredevices` is a leak to the canonical
repository. Check every new pull request:

```shell
gh api repos/elijahr/PebbleOS/pulls/<n> --jq '{base_repo: .base.repo.full_name, head_repo: .head.repo.full_name}'
```

Both fields must read `elijahr/PebbleOS`. If `coredevices` appears in either
field, stop. Do not add commits. Report the pull request number to the
operator.

## Pull request bodies

This repository has no pull request template. Write plain prose that describes
the change and the reason for it.

Do not add `## Summary` or `## Test plan` headings. Some tooling inserts that
shape by default. It is not wanted here.

Do not reference issue numbers in commit messages, pull request titles, or
pull request bodies. Text such as `#123` or `fixes #123` auto-links on GitHub
and notifies every subscriber of the issue.

## Stacked pull requests

Work lands fork to fork. The base branch and the head branch are both branches
in `elijahr/PebbleOS`.

For a series of related changes, stack each pull request on its predecessor.
The second pull request uses the first branch as its base, the third uses the
second, and so on. Do not target the series base branch from every pull
request. A stack keeps each diff small and reviewable.

## Gotchas

### Remotes are shared between worktrees

All worktrees of this repository share one remote configuration, in a single
`.git/config`. A change to a remote affects every session at once.

Coordinate with the other sessions before you add, rename, or remove a remote.

### Pushes that touch workflows need SSH

`origin` uses an HTTPS URL. It authenticates with a `gh` OAuth token. That
token does not have the `workflow` scope. GitHub refuses any push that changes
a file under `.github/workflows/`.

Push such a branch with an explicit SSH URL:

```shell
git push git@github.com:elijahr/PebbleOS.git <branch>
```

Use `origin` for all other pushes.

## Commit trailers

Every commit needs two trailers.

Add `Signed-off-by:` with the `-s` option:

```shell
git commit -s
```

Add `Co-Authored-By:` that names the model which did the work, for example:

```
Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

The model name can differ between commits in one series. This is correct. Each
trailer names the model that did that commit. Do not normalize the names to one
value.
