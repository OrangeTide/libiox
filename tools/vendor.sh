#!/bin/sh
# vendor.sh : copy a release of libiox into another project's tree
#
# Vendoring is the supported way to use libiox. There is no shared library to
# ship and no version to negotiate at run time. You take the source, you own
# the copy, and you upgrade when you decide to.
#
#   curl -fsSL https://raw.githubusercontent.com/OrangeTide/libiox/main/tools/vendor.sh \
#     | sh -s -- --dest third_party/libiox
#
# That form takes the latest release. Piping a script from the network into a
# shell means trusting the server and the connection, so for a repeatable
# build name a tag instead. The URL then points at an immutable commit, and
# reading the script before running it is the better habit:
#
#   TAG=v1.2.3    # whichever release you want
#   curl -fsSLO "https://raw.githubusercontent.com/OrangeTide/libiox/$TAG/tools/vendor.sh"
#   less vendor.sh && sh vendor.sh --version "$TAG"
#
# None of this needs the script. libiox is one directory of C files; a
# tar -xzf and a cp -R does the same job.

set -eu

# Overridable so a fork, a mirror, or an offline copy can be used in place of
# the canonical repository.
REPO=${IOX_VENDOR_REPO:-OrangeTide/libiox}
BASE_URL=${IOX_VENDOR_BASE_URL:-https://github.com}
API_URL=${IOX_VENDOR_API_URL:-https://api.github.com}

VERSION=latest
FROM=
DEST=third_party/libiox
FORCE=0
WITH_TESTS=0
WITH_MODULE_MK=1
LIST=0

self=$(basename "$0")

usage() {
    cat <<'EOF'
Usage: vendor.sh [options]

  --version <tag>   release to take, e.g. v1.2.3, or "latest" (default)
  --from <dir>      vendor from a local checkout instead of fetching a tag.
                    For an air-gapped copy, and for CI to vendor the commit
                    under test rather than the last release.
  --dest <dir>      where to put it (default: third_party/libiox)
  --with-tests      also copy the tests, into <dest>/tests/
  --no-module-mk    do not copy the library's module.mk
  --force           overwrite an existing destination
  --list            print what gets copied, then exit
  --help            this

libiox has no optional layers: the library is the whole of src/ and you take
all of it. Tests live in tests/ upstream and are left behind unless asked
for, so a host build that globs the vendored directory for *.c gets the
library and no stray main(). Taking them and running them after each upgrade
is the better habit.

Examples:

  vendor.sh --dest vendor/libiox
  vendor.sh --version v1.2.3 --with-tests
EOF
}

# The library proper. Every file here is needed; there is nothing optional.
LIB_FILES="iox_loop.c iox_loop.h \
           iox_fd.c iox_fd.h \
           iox_signal.c iox_signal.h \
           iox_timer.c iox_timer.h \
           iox_plat.c iox_plat.h \
           iox_internal.h iox_version.h \
           pq.h"

TEST_FILES="iox_test.c iox_portable_test.c iox_fuzz.c iox_oom.c"

die() {
    echo "$self: $*" >&2
    exit 1
}

have() {
    command -v "$1" >/dev/null 2>&1
}

# curl and wget are both common and neither is guaranteed.
fetch() {
    if have curl; then
        curl -fsSL "$1" -o "$2"
    elif have wget; then
        wget -qO "$2" "$1"
    else
        die "need curl or wget"
    fi
}

fetch_stdout() {
    if have curl; then
        curl -fsSL "$1"
    elif have wget; then
        wget -qO- "$1"
    else
        die "need curl or wget"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
    --version)    [ $# -ge 2 ] || die "--version needs a value"; VERSION=$2; shift 2 ;;
    --from)       [ $# -ge 2 ] || die "--from needs a directory"; FROM=$2;   shift 2 ;;
    --dest)       [ $# -ge 2 ] || die "--dest needs a value";    DEST=$2;    shift 2 ;;
    --with-tests) WITH_TESTS=1; shift ;;
    --no-module-mk) WITH_MODULE_MK=0; shift ;;
    --force)      FORCE=1; shift ;;
    --list)       LIST=1; shift ;;
    --help|-h)    usage; exit 0 ;;
    *)            usage >&2; die "unknown option: $1" ;;
    esac
done

if [ "$LIST" -eq 1 ]; then
    echo "library:"
    for f in $LIB_FILES; do echo "  src/$f"; done
    echo "  module.mk (unless --no-module-mk)"
    echo "tests (--with-tests), into <dest>/tests/:"
    for f in $TEST_FILES; do echo "  tests/$f"; done
    echo "always:"
    echo "  LICENSE"
    exit 0
fi

if [ -n "$FROM" ] && [ "$VERSION" != latest ]; then
    die "--from and --version are alternatives; pick one"
fi

if [ -n "$FROM" ] && [ "$LIST" -ne 1 ]; then
    [ -f "$FROM/src/iox_loop.h" ] || die "$FROM does not look like libiox"
    SRC=$(unset CDPATH; cd -- "$FROM" && pwd)
    VERSION=$(sed -n 's/^#define IOX_VERSION_STRING "\(.*\)"$/v\1/p' \
              "$SRC/src/iox_version.h")
    URL="local:$SRC"
    echo "$self: vendoring from $SRC ($VERSION)"
fi

if [ -z "$FROM" ] && [ "$VERSION" = latest ]; then
    echo "$self: asking for the latest release of $REPO"
    VERSION=$(fetch_stdout "$API_URL/repos/$REPO/releases/latest" \
              | sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p' | head -n 1) || true
    [ -n "$VERSION" ] || die "could not determine the latest release; pass --version"
fi

case "$VERSION" in
v*) ;;
*)  VERSION="v$VERSION" ;;
esac

if [ -e "$DEST" ] && [ "$FORCE" -ne 1 ]; then
    die "$DEST already exists (use --force to overwrite, after checking what is in it)"
fi

if [ -z "$FROM" ]; then
    TMP=$(mktemp -d "${TMPDIR:-/tmp}/libiox-vendor.XXXXXX") ||
        die "cannot make a temporary directory"
    trap 'rm -rf "$TMP"' EXIT INT TERM

    URL="$BASE_URL/$REPO/archive/refs/tags/$VERSION.tar.gz"
    echo "$self: fetching $URL"
    fetch "$URL" "$TMP/src.tar.gz" ||
        die "cannot fetch $VERSION (does the tag exist?)"

    have tar || die "need tar"
    tar -xzf "$TMP/src.tar.gz" -C "$TMP" || die "cannot unpack the snapshot"

    # GitHub names the directory <repo>-<tag without the v>, but that is its
    # convention rather than a promise, so find it instead of assuming it.
    SRC=
    for d in "$TMP"/*/; do
        [ -d "$d" ] || continue
        [ -f "$d/src/iox_loop.h" ] || continue
        SRC=${d%/}
        break
    done
    [ -n "$SRC" ] || die "the snapshot does not look like libiox"
fi

# A --force over a copy that had tests, by a run that does not want them,
# would otherwise leave the test file behind, stale and still compiling.
if [ "$FORCE" -eq 1 ] && [ -d "$DEST" ]; then
    for f in $LIB_FILES module.mk; do
        rm -f "${DEST:?}/$f"
    done
    rm -rf "${DEST:?}/tests"
fi

mkdir -p "$DEST"

copied=
for f in $LIB_FILES; do
    [ -f "$SRC/src/$f" ] || die "src/$f is missing from $VERSION"
    cp "$SRC/src/$f" "$DEST/$f"
    copied="$copied $f"
done

# The library's own module.mk, for a host that also uses modular-make. It
# declares the library and nothing else, so it is harmless to a project that
# ignores it. --no-module-mk leaves it out.
if [ "$WITH_MODULE_MK" -eq 1 ]; then
    cp "$SRC/src/module.mk" "$DEST/module.mk"
    copied="$copied module.mk"
fi

# Tests go in their own directory, mirroring the upstream layout, so that a
# host build globbing the vendored directory for *.c still gets the library
# and only the library.
if [ "$WITH_TESTS" -eq 1 ]; then
    mkdir -p "$DEST/tests"
    for f in $TEST_FILES; do
        [ -f "$SRC/tests/$f" ] || die "tests/$f is missing from $VERSION"
        cp "$SRC/tests/$f" "$DEST/tests/$f"
        copied="$copied tests/$f"
    done
    if [ -d "$SRC/tests/fuzz-corpus" ]; then
        cp -R "$SRC/tests/fuzz-corpus" "$DEST/tests/fuzz-corpus"
        copied="$copied tests/fuzz-corpus/"
    fi
fi

# The licence travels with the code, always.
cp "$SRC/LICENSE" "$DEST/LICENSE"

vendored_version=$(sed -n 's/^#define IOX_VERSION_STRING "\(.*\)"$/\1/p' \
                   "$DEST/iox_version.h")

# The compile-time guard below has to name the version actually copied, so
# derive it rather than writing a number here that would rot on every
# release. Same arithmetic as IOX_VERSION in iox_version.h.
v_major=${vendored_version%%.*}
v_rest=${vendored_version#*.}
v_minor=${v_rest%%.*}
v_patch=${v_rest#*.}
vendored_code=$((v_major * 10000 + v_minor * 100 + v_patch))

cat > "$DEST/VENDORED.md" <<EOF
# Vendored libiox

| | |
| --- | --- |
| Version | \`$VERSION\` (IOX_VERSION_STRING $vendored_version) |
| Source | <$URL> |
| Tests | $([ "$WITH_TESTS" -eq 1 ] && echo "included" || echo "not copied") |

Copied by \`tools/vendor.sh\` from the release snapshot. Do not edit these
files in place: local changes are lost on the next upgrade and make it
impossible to tell what version this is. Upgrade by re-running the script
with a newer \`--version\`, then read the upstream CHANGELOG.

\`iox_version.h\` identifies this copy at compile time:

\`\`\`c
#if IOX_VERSION < $vendored_code          /* $vendored_version */
#  error "libiox $vendored_version or newer is required"
#endif
\`\`\`
EOF

echo "$self: copied into $DEST"
for f in $copied; do
    echo "  $f"
done

echo
echo "Add one include path and the sources to your build:"
echo "  CPPFLAGS += -I$DEST"
echo "  SRCS += $DEST/iox_loop.c $DEST/iox_fd.c $DEST/iox_signal.c \\"
echo "          $DEST/iox_timer.c $DEST/iox_plat.c"
echo "  Windows also needs: -lws2_32"
echo
echo "Details are in $DEST/VENDORED.md."
