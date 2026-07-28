#!/bin/sh
# fuzz.sh : run the libFuzzer build of the API fuzz target
#
# tests/iox_fuzz.c turns an input byte stream into a sequence of libiox calls,
# so what gets fuzzed is the order operations arrive in rather than any one
# argument.
#
#   tools/fuzz.sh                 fuzz for 60 seconds
#   tools/fuzz.sh 600             fuzz for 600 seconds
#   tools/fuzz.sh 600 -jobs=4     pass anything else through to libFuzzer
#
# Two corpora, deliberately. tests/fuzz-corpus/ is committed and small: the
# hand-written seeds plus any input that ever crashed. _build/fuzz/corpus is
# the working corpus libFuzzer grows, which reached 435 files and 1.8MB in
# three minutes and has no business in git. New inputs land there; crashes
# land in the committed one, because those are the ones worth keeping forever.
#
# Needs clang with libFuzzer.

set -eu

self=$(basename "$0")
root=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)
work=${IOX_FUZZ_WORK:-$root/_build/fuzz}
corpus=$root/tests/fuzz-corpus

die() {
    echo "$self: $*" >&2
    exit 1
}

command -v clang >/dev/null 2>&1 || die "needs clang with libFuzzer"

secs=60
case "${1:-}" in
[0-9]*) secs=$1; shift ;;
esac

mkdir -p "$work" "$corpus"

echo "$self: building the libFuzzer target"
clang -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra \
    -I"$root/src" -O1 -g \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
    -o "$work/iox_fuzz" \
    "$root/tests/iox_fuzz.c" \
    "$root/src/iox_loop.c" "$root/src/iox_fd.c" "$root/src/iox_signal.c" \
    "$root/src/iox_timer.c" "$root/src/iox_plat.c" ||
    die "build failed"

mkdir -p "$work/corpus"

echo "$self: fuzzing for ${secs}s"
echo "        seeds:   $corpus (committed)"
echo "        working: $work/corpus (generated)"

# libFuzzer writes new inputs to the first directory and only reads the rest,
# which is what keeps the committed corpus from growing on every run.
#
# -max_len is 512 rather than the default: each operation consumes three
# bytes, so 512 is around 170 calls, which is far past the depth any of these
# bugs needed and keeps a saved input readable as a call sequence.
# -rss_limit_mb stops a leak from eating the machine.
"$work/iox_fuzz" "$work/corpus" "$corpus" \
    -max_total_time="$secs" \
    -max_len=512 \
    -rss_limit_mb=2048 \
    -artifact_prefix="$corpus/" \
    "$@"

echo
echo "$self: no crashes."
echo "        A crashing input would be written to $corpus as crash-<hash>."
echo "        Commit it: 'make run-tests' replays that directory, so a bug"
echo "        found once is checked for forever."
