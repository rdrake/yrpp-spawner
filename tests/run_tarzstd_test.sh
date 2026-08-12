#!/usr/bin/env bash
# Builds and runs the TarZstdWriter host test.
#
# The writer is deliberately free of Windows API so it can be exercised on any
# host, against the REAL `tar` and `zstd` rather than a reader of our own --
# a bespoke reader would happily agree with a bespoke writer's misunderstanding
# of the ustar format. The vendored amalgamation is compiled here too, so what
# is tested is the source that actually ships, not a system libzstd that merely
# shares its version number.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "== compiling the vendored zstd amalgamation =="
cc -std=c99 -O1 -c "$root/src/Spawner/ThirdParty/zstd/zstd.c" -o "$work/zstd.o"

echo "== compiling and linking the test =="
c++ -std=c++17 -O1 -Wall -Wextra \
    -I "$root/src/Spawner" \
    "$here/tarzstd_test.cpp" \
    "$root/src/Spawner/TarZstd.cpp" \
    "$work/zstd.o" \
    -o "$work/tarzstd_test"

echo "== running =="
"$work/tarzstd_test"
