#!/usr/bin/env bash
# Builds all 32-bit Windows binaries with zig. ZIG=/path/to/zig ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
: "${ZIG:=zig}"
mkdir -p bin
"$ZIG" cc -target x86-windows-gnu -shared -O2 -o bin/NFSHP2KeyFix.asi keyfix.c -luser32 -lkernel32
"$ZIG" cc -target x86-windows-gnu -shared -O2 -o bin/D3D8Fps.asi d3d8fps.c -luser32 -lkernel32 -lwinmm
"$ZIG" cc -target x86-windows-gnu -shared -O2 -o bin/dinput8.dll dibridge8.c dinput8.def -luser32 -lkernel32
"$ZIG" cc -target x86-windows-gnu -shared -O2 -o bin/KeyMouse.dll keymouse.c -luser32 -lkernel32
"$ZIG" cc -target x86-windows-gnu -shared -O2 -DEBUEULA_EXPORT -o bin/EBUEulaX.dll keymouse.c -luser32 -lkernel32
"$ZIG" cc -target x86-windows-gnu -shared -O2 -DDSOUND_PROXY -o bin/dsound.dll keymouse.c dsound.def -luser32 -lkernel32
"$ZIG" cc -target x86-windows-gnu -O1 -o harness.exe harness.c -ldinput -luser32 -lkernel32
"$ZIG" cc -target x86-windows-gnu -O1 -o winclick.exe winclick.c -luser32 -lkernel32
(cd bin && md5sum NFSHP2KeyFix.asi D3D8Fps.asi dinput8.dll KeyMouse.dll EBUEulaX.dll dsound.dll > MD5SUMS)
ls -la bin/
