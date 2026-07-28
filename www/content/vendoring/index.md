---
title: Vendoring and versioning
date: 2026-07-24
abstract: How to take a copy, how to identify the copy you took, and how to upgrade it. libiox expects to be copied rather than installed.
category: guide
tags: [vendoring, releases]
---

libiox is meant to be copied into your tree. There is no shared library, no
`pkg-config` file, and nothing to negotiate at run time.

That is not a claim about elegance. libiox spent its first year as fourteen
divergent copies scattered across unrelated projects, because it was easy to
copy and had no release mechanism at all. The tooling described here is that
missing mechanism, added after the fact.

## Taking a copy

```sh
curl -fsSLO https://raw.githubusercontent.com/OrangeTide/libiox/v0.1.1/tools/vendor.sh
less vendor.sh
sh vendor.sh --version v0.1.1 --dest third_party/libiox
```

The script fetches the source snapshot GitHub serves for the tag, copies the
thirteen library files and the licence, and writes a `VENDORED.md` recording
what it took. `--with-tests` adds the test suite, which is worth having.

Reading a script before piping it into a shell is the habit worth keeping.
Naming a tag rather than `main` is the other one: a tag is an immutable
commit, so the script cannot change under you between builds.

## Identifying a copy

A vendored copy has no git and no tags. `iox_version.h` is the only thing
that can say what it is:

```c
#include "iox_version.h"

#if IOX_VERSION < 101          /* 0.1.1 */
#  error "libiox 0.1.1 or newer is required"
#endif
```

The header is the authority and the git tag mirrors it, not the other way
round. `tools/version.sh --check` verifies they agree, along with the
CHANGELOG entry and a clean tree, and CI runs it on every push.

## Cutting a release

```sh
tools/version.sh --bump minor     # rewrites the header, then the docs
$EDITOR CHANGELOG.md              # add the section for it
git commit -am "libiox 0.2.0"
tools/version.sh --check          # header, docs, changelog, tree, tag
tools/version.sh --tag            # annotated tag on that commit
```

`--bump` and `--set` rewrite `src/iox_version.h` and then run `--sync`,
which rewrites the version in the handful of copy-pasteable commands in the
documentation. The header is the only place a version is typed by hand;
`--check` fails if anything drifted, and CI runs it on every push.

The order matters. `--bump` and `--set` only edit the header, so the tag
always lands on a commit whose header already says the right thing. A tag
that disagrees with the header would ship a copy that lies about itself, and
the lie would only surface in someone else's tree.

## Upgrading

Re-run with a newer `--version` and `--force`, then read the CHANGELOG and
run the tests. `--force` removes what the previous run recorded before
writing, so a copy that had tests and a run that does not want them does not
leave a stale test file compiling.

Do not edit a vendored copy in place. That is exactly how fourteen copies
happened. If you need a change, send it upstream, or keep it as a patch you
re-apply and say so in `VENDORED.md`.

## Compatibility

Versions are SemVer, and the major version is the promise. While the major
version is 0, a minor bump may break the API; the CHANGELOG records every
break with the reason.

There is no wire format and no ABI, so the only compatibility surface is the
four public headers.
