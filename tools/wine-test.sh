#!/bin/sh
# wine-test.sh : cross-compile for Windows and run the tests under wine
#
# The Windows half of libiox is a real implementation, not a stub: WSAPoll
# instead of poll, QueryPerformanceCounter instead of clock_gettime, and a
# bound UDP loopback socket instead of a self-pipe, because anonymous pipes
# are not pollable by WSAPoll. For a long time it was only ever compiled.
#
# CI runs it natively on a Windows runner. This runs it here, which is a
# faster loop and catches the obvious breakage before a push. Wine is not
# Windows, so a pass here is evidence rather than proof; the native CI job
# is the authority.
#
#   tools/wine-test.sh
#
# Needs x86_64-w64-mingw32-gcc and wine.

set -eu

self=$(basename "$0")
root=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)
work=${IOX_WINE_WORK:-$root/_build/wine}

CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

die() {
    echo "$self: $*" >&2
    exit 1
}

command -v "$CC" >/dev/null 2>&1 ||
    die "need $CC (apt install gcc-mingw-w64-x86-64)"
command -v wine >/dev/null 2>&1 || die "need wine"

mkdir -p "$work"

echo "$self: building the Windows objects"
for f in iox_loop iox_fd iox_signal iox_timer iox_plat; do
    "$CC" -c -o "$work/$f.o" -I"$root/src" \
        -std=c99 -Wall -Wextra -Werror -O2 "$root/src/$f.c" ||
        die "$f.c failed to compile for Windows"
done

echo "$self: linking the portable test"
"$CC" -o "$work/iox_portable_test.exe" -I"$root/src" \
    -std=c99 -Wall -Wextra -Werror -O2 \
    "$root/tests/iox_portable_test.c" "$work"/*.o -lws2_32 ||
    die "link failed"

echo "$self: running under wine"
echo

# WINEDEBUG quiets the noise wine writes to stderr on a cold prefix, which
# would otherwise bury the test output. Failures still surface: the exit
# status is the test binary's own.
WINEDEBUG=${WINEDEBUG:--all} wine "$work/iox_portable_test.exe"
rc=$?

echo
if [ "$rc" -eq 0 ]; then
    echo "$self: the Windows build passes under wine."
    echo "        The native Windows CI job is what actually gates a release."
else
    echo "$self: FAILED (exit $rc)"
fi
exit "$rc"
