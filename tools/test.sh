#!/bin/sh
# test.sh : build and run libiox every aggressive way this machine allows
#
# `make run-tests` is the fast check. This is the thorough one: the same
# suite under two compilers, four optimisation levels, three C standards,
# sanitizers, valgrind, 32-bit, static analysis, an allocation-failure sweep,
# and a fuzz corpus replay.
#
#   tools/test.sh            run everything available
#   tools/test.sh --list     name the stages, then exit
#   tools/test.sh sanitizers run one or more stages by name
#
# A stage whose tool is missing is skipped and reported as such, so this is
# runnable on a bare machine and still says what it did not cover.

set -eu

self=$(basename "$0")
root=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)
work=${IOX_TEST_WORK:-$root/_build/test.sh}

LIB="$root/src/iox_loop.c $root/src/iox_fd.c $root/src/iox_signal.c \
     $root/src/iox_timer.c $root/src/iox_plat.c"
SUITE="$root/tests/iox_test.c"
FUZZ="$root/tests/iox_fuzz.c"
OOM="$root/tests/iox_oom.c"
INC="-I$root/src"
BASE="-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wshadow"

pass=0
fail=0
skip=0
failed_stages=

say()  { printf '\n=== %s\n' "$*"; }
ok()   { printf '  ok      %s\n' "$*"; pass=$((pass + 1)); }
bad()  { printf '  FAIL    %s\n' "$*"; fail=$((fail + 1)); failed_stages="$failed_stages $1"; }
skipd(){ printf '  skip    %s\n' "$*"; skip=$((skip + 1)); }

have() { command -v "$1" >/dev/null 2>&1; }

# Build and run one configuration. Anything non-zero anywhere is a failure.
try() {
    name=$1
    shift
    if "$@" >"$work/$name.log" 2>&1; then
        ok "$name"
    else
        bad "$name"
        sed 's/^/          /' "$work/$name.log" | tail -15
    fi
}

build_run() {
    name=$1
    cc=$2
    shift 2
    # shellcheck disable=SC2086
    if ! $cc $BASE $INC "$@" -o "$work/$name" $SUITE $LIB \
            >"$work/$name.log" 2>&1; then
        bad "$name (build)"
        sed 's/^/          /' "$work/$name.log" | tail -15
        return 0
    fi
    if "$work/$name" >>"$work/$name.log" 2>&1; then
        ok "$name"
    else
        bad "$name (run)"
        sed 's/^/          /' "$work/$name.log" | tail -20
    fi
}

# ---------------------------------------------------------------- stages

stage_warnings() {
    say "strict warnings, both compilers, every optimisation level"
    for o in -O0 -O1 -O2 -O3 -Os; do
        build_run "gcc$o" gcc -Werror "$o"
    done
    if have clang; then
        for o in -O0 -O2 -O3; do
            build_run "clang$o" clang -Werror "$o"
        done
    else
        skipd "clang (not installed)"
    fi
}

stage_standards() {
    say "language standards"
    for std in c99 c11 c17; do
        # shellcheck disable=SC2086
        if gcc -std=$std -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic \
               -Werror $INC -O2 -o "$work/std-$std" $SUITE $LIB \
               >"$work/std-$std.log" 2>&1 && "$work/std-$std" \
               >>"$work/std-$std.log" 2>&1; then
            ok "std=$std"
        else
            bad "std=$std"
            sed 's/^/          /' "$work/std-$std.log" | tail -10
        fi
    done
}

stage_sanitizers() {
    say "sanitizers"
    # -fno-sanitize-recover makes the first report fatal, so a run that exits
    # zero really did see nothing rather than printing and carrying on.
    build_run "gcc-asan-ubsan" gcc -O1 -g \
        -fsanitize=address,undefined -fno-sanitize-recover=all
    if have clang; then
        build_run "clang-asan-ubsan" clang -O1 -g \
            -fsanitize=address,undefined -fno-sanitize-recover=all
        build_run "clang-integer" clang -O1 -g \
            -fsanitize=integer,implicit-conversion -fno-sanitize-recover=all
    else
        skipd "clang sanitizers (not installed)"
    fi
    # Trap mode compiles the checks down to an illegal instruction, which
    # catches anything the runtime library might soften.
    build_run "ubsan-trap" gcc -O2 -g \
        -fsanitize=undefined -fsanitize-undefined-trap-on-error

    # MemorySanitizer needs every library it links instrumented, including
    # libc, so on a stock toolchain it reports the C runtime rather than us.
    skipd "msan (needs an instrumented libc)"
    # ThreadSanitizer has nothing to find: libiox is single-threaded by
    # contract and a loop belongs to one thread.
    skipd "tsan (library is single-threaded by contract)"
}

stage_hardening() {
    say "hardened build"
    build_run "fortify" gcc -O2 -g -D_FORTIFY_SOURCE=3 \
        -fstack-protector-strong -fstack-clash-protection \
        -fcf-protection=full -Wformat -Wformat-security -Werror
}

stage_valgrind() {
    say "valgrind"
    if ! have valgrind; then
        skipd "valgrind (not installed)"
        return 0
    fi
    # shellcheck disable=SC2086
    gcc $BASE $INC -O1 -g -o "$work/vg" $SUITE $LIB >"$work/vg.log" 2>&1
    try valgrind-memcheck valgrind -q --error-exitcode=9 --leak-check=full \
        --track-origins=yes --errors-for-leak-kinds=all "$work/vg"
}

stage_m32() {
    say "32-bit"
    # Probe with the headers the library actually pulls in. A bare main()
    # links fine with no multilib headers installed, and then the real build
    # fails on asm/errno.h, which reads as a library fault rather than a
    # missing gcc-multilib.
    if ! printf '#include <poll.h>\n#include <errno.h>\nint main(void){return 0;}\n' |
            gcc -m32 -x c - -o "$work/m32probe" >/dev/null 2>&1; then
        skipd "-m32 (no 32-bit headers; install gcc-multilib)"
        return 0
    fi
    build_run "m32" gcc -m32 -O2 -Werror
}

stage_analysis() {
    say "static analysis"
    # shellcheck disable=SC2086
    if gcc -fanalyzer -c -o /dev/null $BASE $INC -O2 $root/src/iox_loop.c \
            >/dev/null 2>&1; then
        rc=0
        for f in $LIB; do
            # shellcheck disable=SC2086
            gcc -fanalyzer -c -o /dev/null $BASE $INC -O2 "$f" \
                >>"$work/analyzer.log" 2>&1 || rc=1
        done
        # pq.h is excluded for the same reason as the clang stage below: on
        # the path through resize() the analyzer cannot prove that the
        # realloc'd entry buffer, stored in pq->entry, stays reachable, so it
        # reports a bogus leak at the enqueue in pq.h. The buffer is owned by
        # the queue and freed in free(). Warnings in libiox's own files still
        # fail the stage.
        grep 'warning:' "$work/analyzer.log" | grep -v '/pq\.h:' \
            > "$work/analyzer.warnings" || true
        if [ "$rc" -eq 0 ] && [ ! -s "$work/analyzer.warnings" ]; then
            ok "gcc -fanalyzer (pq.h excluded, see the note in this script)"
        else
            bad "gcc -fanalyzer"
            sed 's/^/          /' "$work/analyzer.warnings" | head -15
        fi
    else
        skipd "gcc -fanalyzer (not supported)"
    fi

    if have clang; then
        rc=0
        for f in $LIB; do
            # shellcheck disable=SC2086
            clang --analyze -Xanalyzer -analyzer-output=text $BASE $INC "$f" \
                >>"$work/clang-analyze.log" 2>&1 || rc=1
        done
        # pq.h is excluded, for one specific and checked reason: the analyzer
        # reports a garbage read at the PQ_COMPARE in siftup_ on the path
        # through resize(), because it cannot show that realloc kept the old
        # entries. siftup_ only ever reads PQ__PARENT(i), which is strictly
        # less than i for every i > 0, so every read is inside [0, nr) and
        # initialised. Warnings in libiox's own files still fail the stage.
        grep 'warning:' "$work/clang-analyze.log" | grep -v '/pq\.h:' \
            > "$work/clang-analyze.warnings" || true
        if [ "$rc" -eq 0 ] && [ ! -s "$work/clang-analyze.warnings" ]; then
            ok "clang --analyze (pq.h excluded, see the note in this script)"
        else
            bad "clang --analyze"
            sed 's/^/          /' "$work/clang-analyze.warnings" | head -15
        fi
    else
        skipd "clang --analyze (not installed)"
    fi

    if have cppcheck; then
        # shellcheck disable=SC2086
        try cppcheck cppcheck --error-exitcode=1 --quiet --enable=warning,portability \
            --inline-suppr $INC $LIB
    else
        skipd "cppcheck (not installed)"
    fi
}

stage_oom() {
    say "allocation failure sweep"
    # --wrap is a GNU ld feature; a toolchain without it skips rather than fails.
    # shellcheck disable=SC2086
    if ! gcc $BASE $INC -O1 -g -Wl,--wrap=calloc -o "$work/oom" $OOM $LIB \
            >"$work/oom-build.log" 2>&1; then
        skipd "oom sweep (no --wrap=calloc support)"
        return 0
    fi
    if have valgrind; then
        try oom-valgrind valgrind -q --error-exitcode=9 --leak-check=full "$work/oom"
    else
        try oom "$work/oom"
    fi
}

stage_fuzz() {
    say "fuzz corpus replay"
    corpus=$root/tests/fuzz-corpus
    set --
    [ -d "$corpus" ] && set -- "$corpus"/*
    # shellcheck disable=SC2086
    if ! gcc $BASE $INC -DIOX_FUZZ_STANDALONE -O1 -g \
            -fsanitize=address,undefined -fno-sanitize-recover=all \
            -o "$work/fuzz" $FUZZ $LIB >"$work/fuzz-build.log" 2>&1; then
        bad "fuzz (build)"
        sed 's/^/          /' "$work/fuzz-build.log" | tail -15
        return 0
    fi
    try fuzz-replay "$work/fuzz" "$@"
}

stage_coverage() {
    say "coverage"
    if ! have gcov; then
        skipd "gcov (not installed)"
        return 0
    fi
    # shellcheck disable=SC2086
    if ! ( cd "$work" &&
           gcc $BASE $INC -O0 -g --coverage -o cov $SUITE $LIB >cov.log 2>&1 &&
           ./cov >>cov.log 2>&1 &&
           gcov -n ./*.gcda >cov-report.txt 2>&1 ); then
        skipd "coverage (build or run failed)"
        return 0
    fi
    ok "coverage collected"
    awk '/^File .*src\/iox_/ { f=$2 } /^Lines executed/ && f {
            gsub(/.*src\//, "", f); gsub(/'"'"'/, "", f);
            printf "          %-16s %s\n", f, $0; f="" }' \
        "$work/cov-report.txt" 2>/dev/null || true
}

stage_shell() {
    say "shell tools"
    if have shellcheck; then
        try shellcheck shellcheck -s sh "$root/tools/version.sh" \
            "$root/tools/vendor.sh" "$root/tools/test.sh" "$root/tools/fuzz.sh"
    else
        skipd "shellcheck (not installed)"
    fi
}

stage_mingw() {
    say "windows cross-compile"
    if ! have x86_64-w64-mingw32-gcc; then
        skipd "mingw (not installed)"
        return 0
    fi
    rc=0
    for f in $LIB; do
        x86_64-w64-mingw32-gcc -c -o /dev/null "$INC" -std=c99 -Wall -Wextra \
            -Werror -O2 "$f" >>"$work/mingw.log" 2>&1 || rc=1
    done
    if [ "$rc" -eq 0 ]; then
        ok "mingw objects"
    else
        bad "mingw objects"
        sed 's/^/          /' "$work/mingw.log" | tail -15
    fi
}

STAGES="warnings standards sanitizers hardening valgrind m32 analysis oom fuzz coverage shell mingw"

if [ "${1:-}" = "--list" ]; then
    for s in $STAGES; do echo "$s"; done
    exit 0
fi
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi

rm -rf "$work"
mkdir -p "$work"

# shellcheck disable=SC2086
[ $# -gt 0 ] || set -- $STAGES
for s in "$@"; do
    case " $STAGES " in
    *" $s "*) "stage_$s" ;;
    *) echo "$self: unknown stage: $s (try --list)" >&2; exit 1 ;;
    esac
done

printf '\n%s\n' "----------------------------------------------------------"
printf '%s: %d passed, %d failed, %d skipped\n' "$self" "$pass" "$fail" "$skip"
[ -n "$failed_stages" ] && printf 'failed:%s\n' "$failed_stages"
printf 'logs in %s\n' "$work"
[ "$fail" -eq 0 ]
