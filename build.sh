#!/usr/bin/env bash
# Builds bin/NFSHP2KeyFix.asi and harness.exe as 32-bit Windows binaries.
# ZIG=/path/to/zig ./build.sh      (zig 0.13+; or set CC=i686-w64-mingw32-gcc)
set -euo pipefail
cd "$(dirname "$0")"
if [[ -n "${CC:-}" ]]; then
  "$CC" -shared -O2 -o bin/NFSHP2KeyFix.asi keyfix.c -luser32 -lkernel32
  "$CC" -O1 -o harness.exe harness.c -ldinput -luser32 -lkernel32
else
  : "${ZIG:=zig}"
  "$ZIG" cc -target x86-windows-gnu -shared -O2 -o bin/NFSHP2KeyFix.asi keyfix.c -luser32 -lkernel32
  "$ZIG" cc -target x86-windows-gnu -O1 -o harness.exe harness.c -ldinput -luser32 -lkernel32
fi
(cd bin && md5sum NFSHP2KeyFix.asi > MD5SUMS)
ls -la bin/NFSHP2KeyFix.asi harness.exe
