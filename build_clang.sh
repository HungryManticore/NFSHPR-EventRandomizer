#!/bin/bash
# Cross-builds build/NFSHPR_EventRandomizer.asi on Linux with clang + lld, using only the MinGW-w64
# headers (no MinGW runtime): the C library is msvcrt.dll and the import libraries are generated for
# exactly the functions the code uses. This is how the release binary was built.
#
# Needs: clang, ld.lld, llvm-nm, llvm-dlltool, python3, and a checkout of MinGW-w64:
#   git clone --depth 1 https://github.com/mingw-w64/mingw-w64
#   MINGW_W64=/path/to/mingw-w64 ./build_clang.sh
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
MINGW_W64=${MINGW_W64:?set MINGW_W64 to a mingw-w64 source checkout}
B=$ROOT/build
mkdir -p "$B/obj" "$B/lib" "$B/include"

# MinGW-w64 headers (crt + include), with _mingw.h generated from its template
if [ ! -f "$B/include/_mingw.h" ]; then
    cp -r "$MINGW_W64"/mingw-w64-headers/include/. "$B/include/"
    cp -rf "$MINGW_W64"/mingw-w64-headers/crt/. "$B/include/"
    sed -e 's/@DEFAULT_MSVCRT_VERSION@/0x700/' -e 's/@DEFAULT_WIN32_WINNT@/0x601/' \
        "$B/include/_mingw.h.in" > "$B/include/_mingw.h"
fi

IMGUI=$ROOT/third_party/imgui
FLAGS="--target=x86_64-w64-windows-gnu -nostdinc -isystem $(clang -print-resource-dir)/include -isystem $B/include
 -I$IMGUI -I$IMGUI/backends -I$ROOT/src
 -std=c++17 -O2 -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -fno-stack-protector
 -ffunction-sections -fdata-sections -fno-threadsafe-statics
 -D__USE_MINGW_ANSI_STDIO=0 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DNDEBUG
 -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_FILE_FUNCTIONS
 -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -DIMGUI_DISABLE_DEBUG_TOOLS"
SRCS="$IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp
 $IMGUI/backends/imgui_impl_dx11.cpp $IMGUI/backends/imgui_impl_win32.cpp
 $ROOT/src/NFSHPR_EventRandomizer.cpp $ROOT/src/Overlay.cpp $ROOT/src/CrtShim.cpp"
OBJS=""
for s in $SRCS; do
    o=$B/obj/$(basename "${s%.cpp}").o
    OBJS="$OBJS $o"
    clang++ $FLAGS -Wall -c "$s" -o "$o"
done

# Import libraries for exactly the undefined symbols, from MinGW-w64's .def lists
python3 - "$B" "$MINGW_W64" $OBJS <<'PY'
import os, re, subprocess, sys
B, MW, objs = sys.argv[1], sys.argv[2], sys.argv[3:]
und, defd = set(), set()
for o in objs:
    for l in subprocess.run(['llvm-nm', o], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) >= 2 and p[-2] == 'U': und.add(p[-1])
        elif len(p) >= 3: defd.add(p[-1])
need = {s[6:] if s.startswith('__imp_') else s for s in und if s not in defd} - defd
libs = [('kernel32.dll', 'kernel32.def.in'), ('user32.dll', 'user32.def.in'), ('gdi32.dll', 'gdi32.def'),
        ('msvcrt.dll', 'msvcrt.def.in'), ('dwmapi.dll', 'dwmapi.def'), ('d3dcompiler_47.dll', 'd3dcompiler_47.def')]
names = {}
for dll, f in libs:
    t = open(os.path.join(MW, 'mingw-w64-crt/lib-common', f)).read()
    names[dll] = set(re.findall(r'^(?:F[A-Z0-9_]*\()?([A-Za-z_][A-Za-z0-9_]*)', t, re.M))
assign, missing = {}, []
for s in sorted(need):
    dll = next((d for d, _ in libs if s in names[d]), None)
    if dll: assign.setdefault(dll, []).append(s)
    else: missing.append(s)
if missing: sys.exit('unresolved: %s' % missing)
for dll, syms in assign.items():
    base = dll[:-4]
    open(f'{B}/lib/{base}.def', 'w').write(f'LIBRARY {dll}\nEXPORTS\n' + '\n'.join(syms) + '\n')
    subprocess.run(['llvm-dlltool', '-m', 'i386:x86-64', '-d', f'{B}/lib/{base}.def', '-l', f'{B}/lib/lib{base}.a'], check=True)
PY

ld.lld -m i386pep --shared --entry DllMain --subsystem windows --gc-sections \
    -o "$B/NFSHPR_EventRandomizer.asi" $OBJS "$B"/lib/*.a
echo "built $B/NFSHPR_EventRandomizer.asi"
