#!/usr/bin/env bash
# WatchEngine under Wine, in the three launch modes that matter:
#   plain         - the engine alone
#   detached      - Syringe --detach, as a WATCHDUMP round must launch
#   attached      - plain Syringe; arming must REFUSE (self-test 0 of 2)
# Needs mingw-w64 (i686) and wine. Usage: tests/run_watchengine_test.sh <Syringe.exe>
set -euo pipefail
syringe=${1:?usage: $0 <path to Syringe.exe>}
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src/Spawner
work=$(mktemp -d)
cp "$syringe" "$work/Syringe.exe"
cd "$work"
i686-w64-mingw32-g++ -O2 -std=c++17 -Wall -Wextra -Wshadow -Wl,--disable-dynamicbase \
  -I"$src" -o host.exe "$here/watchengine_test.cpp" "$src/WatchEngine.cpp" -static
hook=$(i686-w64-mingw32-nm host.exe | awk '$3=="hookpoint"{print "0x"$1}')
i686-w64-mingw32-gcc -shared -O0 -DHOOKADDR="$hook" -o probe.dll "$here/watchengine_hook.c" -Wl,--kill-at
fail=0
for mode in plain detached attached; do
  rm -f engtest.out
  case $mode in
    plain)    WINEDEBUG=-all timeout 60 wine host.exe >/dev/null 2>&1 || true ;;
    detached) WINEDEBUG=-all timeout 60 wine Syringe.exe host.exe -i=probe.dll --detach >/dev/null 2>&1 || true ;;
    attached) WINEDEBUG=-all timeout 60 wine Syringe.exe host.exe -i=probe.dll --args=expect-fail >/dev/null 2>&1 || true ;;
  esac
  verdict=$(tr -d '\r' < engtest.out 2>/dev/null | grep '^VERDICT' || echo 'VERDICT: MISSING')
  echo "$mode: $verdict"
  [[ $verdict == "VERDICT: PASS"* ]] || fail=1
done
rm -rf "$work"
exit $fail
