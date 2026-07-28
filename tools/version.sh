#!/bin/sh
# version.sh : read, check, or bump the version of libiox
#
# src/iox_version.h is the authority. A vendored copy has no git and no tags,
# so the header is the only thing that can say what it is. git tags mirror the
# header rather than the other way round.
#
#   tools/version.sh                 print the current version
#   tools/version.sh --check         verify everything agrees with the header
#   tools/version.sh --set 0.2.0     rewrite the header to a version
#   tools/version.sh --bump minor    rewrite the header to the next major/minor/patch
#   tools/version.sh --sync          rewrite the version in the docs from the header
#   tools/version.sh --tag           create the git tag for the current header
#
# --set and --bump only edit the header. Commit that, then --tag, so the tag
# always points at a commit whose header already says the right thing.
#
# Documentation quotes the version in a handful of copy-pasteable commands.
# Those are generated from the header by --sync and verified by --check, so
# the header stays the only place a version is written by hand. Illustrative
# examples elsewhere use v1.2.3, which is obviously not a real release and
# never needs updating.

set -eu

self=$(basename "$0")
root=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)
header=$root/src/iox_version.h
changelog=$root/CHANGELOG.md

die() {
    echo "$self: $*" >&2
    exit 1
}

usage() {
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

read_field() {
    sed -n "s/^#define IOX_VERSION_$1 \\([0-9][0-9]*\\)\$/\\1/p" "$header"
}

read_version() {
    major=$(read_field MAJOR)
    minor=$(read_field MINOR)
    patch=$(read_field PATCH)
    if [ -z "$major" ] || [ -z "$minor" ] || [ -z "$patch" ]; then
        die "cannot parse $header"
    fi
    echo "$major.$minor.$patch"
}

read_string() {
    sed -n 's/^#define IOX_VERSION_STRING "\(.*\)"$/\1/p' "$header"
}

# The same arithmetic as IOX_VERSION in the header: 1.2.3 is 10203.
#
# POSIX sh has no local variables, so every helper here prefixes its own to
# keep out of the callers' way. Getting this wrong once already produced a
# doubled path, because sync_file's "f" was the loop's "f".
version_code() {
    vc_major=${1%%.*}
    vc_rest=${1#*.}
    vc_minor=${vc_rest%%.*}
    vc_patch=${vc_rest#*.}
    echo $((vc_major * 10000 + vc_minor * 100 + vc_patch))
}

# Files that quote the current version in something a reader would copy and
# run. Anything not on this list must use the v1.2.3 placeholder instead.
SYNCED="README.md VENDORING.md www/content/vendoring/index.md"

# Rewrite one file's version references. Every pattern is anchored on
# surrounding text, so a stray version number elsewhere in prose is left
# alone and CHANGELOG.md's history is never touched.
sync_file() {
    sf_path=$root/$1
    sf_v=$2
    sf_code=$3

    [ -f "$sf_path" ] || return 0

    sed -e "s|libiox/v[0-9][0-9.]*/tools/vendor\.sh|libiox/v$sf_v/tools/vendor.sh|g" \
        -e "s|--version v[0-9][0-9.]*|--version v$sf_v|g" \
        -e "s|^TAG=v[0-9][0-9.]*$|TAG=v$sf_v|" \
        -e "s|IOX_VERSION < [0-9][0-9]*\( *\)/\* [0-9][0-9.]* \*/|IOX_VERSION < $sf_code\1/* $sf_v */|g" \
        -e "s|libiox [0-9][0-9.]* or newer is required|libiox $sf_v or newer is required|g" \
        -e "s|^#define IOX_VERSION_MAJOR  [0-9][0-9]*$|#define IOX_VERSION_MAJOR  ${sf_v%%.*}|" \
        -e "s|^#define IOX_VERSION_STRING \"[0-9][0-9.]*\"$|#define IOX_VERSION_STRING \"$sf_v\"|" \
        "$sf_path"
}

do_sync() {
    ds_v=$(read_version)
    ds_code=$(version_code "$ds_v")
    ds_changed=0

    for ds_f in $SYNCED; do
        [ -f "$root/$ds_f" ] || continue
        ds_tmp=$(mktemp "${TMPDIR:-/tmp}/iox-sync.XXXXXX") ||
            die "cannot make a temp file"
        sync_file "$ds_f" "$ds_v" "$ds_code" > "$ds_tmp"
        if cmp -s "$ds_tmp" "$root/$ds_f"; then
            rm -f "$ds_tmp"
        else
            cat "$ds_tmp" > "$root/$ds_f"
            rm -f "$ds_tmp"
            echo "$self: updated $ds_f"
            ds_changed=1
        fi
    done

    [ "$ds_changed" -eq 0 ] && echo "$self: docs already say $ds_v"
    return 0
}

# Same rewrite, but reporting rather than writing.
check_sync() {
    cs_v=$(read_version)
    cs_code=$(version_code "$cs_v")
    cs_rc=0

    for cs_f in $SYNCED; do
        [ -f "$root/$cs_f" ] || continue
        if ! sync_file "$cs_f" "$cs_v" "$cs_code" | cmp -s - "$root/$cs_f"; then
            echo "$self: $cs_f does not say $cs_v (run --sync)" >&2
            cs_rc=1
        fi
    done

    [ "$cs_rc" -eq 0 ] && echo "$self: docs agree with the header"
    return "$cs_rc"
}

write_version() {
    v=$1
    case "$v" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "not a MAJOR.MINOR.PATCH version: $v" ;;
    esac

    major=${v%%.*}
    rest=${v#*.}
    minor=${rest%%.*}
    patch=${rest#*.}

    tmp=$(mktemp "${TMPDIR:-/tmp}/iox-version.XXXXXX") || die "cannot make a temp file"
    trap 'rm -f "$tmp"' EXIT INT TERM

    sed -e "s/^#define IOX_VERSION_MAJOR .*/#define IOX_VERSION_MAJOR $major/" \
        -e "s/^#define IOX_VERSION_MINOR .*/#define IOX_VERSION_MINOR $minor/" \
        -e "s/^#define IOX_VERSION_PATCH .*/#define IOX_VERSION_PATCH $patch/" \
        -e "s/^#define IOX_VERSION_STRING .*/#define IOX_VERSION_STRING \"$v\"/" \
        "$header" > "$tmp"

    cat "$tmp" > "$header"
    rm -f "$tmp"
    trap - EXIT INT TERM

    echo "$self: $header is now $v"

    # Sync immediately rather than leaving it to be remembered. A bump that
    # left the documented install command pointing at the previous release
    # is exactly the drift this whole mechanism exists to stop.
    do_sync
}

# Every check is advisory on its own; together they are what a release needs.
do_check() {
    v=$(read_version)
    s=$(read_string)
    rc=0

    if [ "$v" != "$s" ]; then
        echo "$self: IOX_VERSION_STRING is \"$s\" but the fields say $v" >&2
        rc=1
    fi

    check_sync || rc=1

    if [ -d "$root/.git" ]; then
        if git -C "$root" rev-parse "v$v" >/dev/null 2>&1; then
            echo "$self: tag v$v exists"
        else
            echo "$self: no tag v$v yet (run --tag once the header is committed)"
        fi

        if [ -n "$(git -C "$root" status --porcelain 2>/dev/null)" ]; then
            echo "$self: working tree is dirty" >&2
            rc=1
        fi
    fi

    if [ -f "$changelog" ]; then
        if grep -q "^## \[\{0,1\}$v" "$changelog"; then
            echo "$self: CHANGELOG.md has an entry for $v"
        else
            echo "$self: CHANGELOG.md has no '## $v' entry" >&2
            rc=1
        fi
    fi

    [ "$rc" -eq 0 ] && echo "$self: $v looks releasable"
    return "$rc"
}

do_tag() {
    v=$(read_version)

    [ -d "$root/.git" ] || die "not a git repository"
    [ -z "$(git -C "$root" status --porcelain)" ] ||
        die "working tree is dirty; commit the version bump first"

    if git -C "$root" rev-parse "v$v" >/dev/null 2>&1; then
        die "tag v$v already exists"
    fi

    git -C "$root" tag -a "v$v" -m "libiox $v"
    echo "$self: tagged v$v"
}

do_bump() {
    v=$(read_version)
    major=${v%%.*}
    rest=${v#*.}
    minor=${rest%%.*}
    patch=${rest#*.}

    case "$1" in
    major) major=$((major + 1)); minor=0; patch=0 ;;
    minor) minor=$((minor + 1)); patch=0 ;;
    patch) patch=$((patch + 1)) ;;
    *)     die "bump takes major, minor, or patch" ;;
    esac

    write_version "$major.$minor.$patch"
}

[ -f "$header" ] || die "cannot find $header"

case "${1:-}" in
"")            read_version ;;
--check)       do_check ;;
--sync)        do_sync ;;
--tag)         do_tag ;;
--set)         [ $# -ge 2 ] || die "--set needs a version"; write_version "$2" ;;
--bump)        [ $# -ge 2 ] || die "--bump needs major, minor, or patch"; do_bump "$2" ;;
--help|-h)     usage 0 ;;
*)             usage 1 >&2 ;;
esac
