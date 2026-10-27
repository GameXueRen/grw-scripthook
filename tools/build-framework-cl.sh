#!/usr/bin/env bash
# Build dinput8.dll with cl.exe directly, skipping build_msvc.ps1.
#
# Why this exists: build_msvc.ps1 finds the toolchain through reg.exe, and a
# sandboxed shell (or any CLI that blocks Program Blacklist entries) cannot run
# reg.exe at all - the script dies before the first compile. This does the same
# three steps the script does, in the same order, with the same flags:
#
#   1. rc    - the version resource scripthook.rc, from SH_VERSION in the header
#   2. ml64  - guard.asm, the stack-pad shim (no inline asm on x64 in MSVC)
#   3. cl    - the 7 C++ overlay objects, then 46 C sources linked with
#              proxy.def and the implib plugins link against
#
# Differences from the script, on purpose: the toolchain paths are pinned here
# (the script discovers them), the build goes to out/ instead of straight into
# the game folder, and nothing is deployed. Copy the result yourself.
#
# Usage:  bash tools/build-framework-cl.sh [--release]
#   --release adds /DSH_RELEASE=1, which compiles the framework's own
#   diagnostics out (same as build_msvc.ps1 -Release).
#
# The export table is the framework's contract with every plugin on disk -
# 442 Sh* names from SH_API plus the six proxy entry points in proxy.def - and
# there is no other source-level list of it: SH_API is a __declspec, so the
# symbol list exists only in a built binary. A build therefore ends by
# comparing itself against tools/exports-baseline.txt and saying whether the
# surface moved. Refresh that file deliberately, in the same commit as the
# header change that moved it, never to make a red line go away.
set -e

VCT='C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231'
SDK='C:/Program Files (x86)/Windows Kits/10'
SDKV='10.0.26100.0'
export PATH="/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:/c/Program Files (x86)/Windows Kits/10/bin/$SDKV/x64:$PATH"
export INCLUDE="$VCT\include;$SDK\Include\\$SDKV\ucrt;$SDK\Include\\$SDKV\um;$SDK\Include\\$SDKV\shared"
export LIB="$VCT\lib\x64;$SDK\Lib\\$SDKV\ucrt\x64;$SDK\Lib\\$SDKV\um\x64"
export MSYS2_ARG_CONV_EXCL='*'

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

RELEASE=0
[ "$1" = "--release" ] && RELEASE=1
EXTRA_C=""
EXTRA_CPP=""
if [ "$RELEASE" = "1" ]; then
    EXTRA_C="-DSH_RELEASE=1"
    EXTRA_CPP="-DSH_RELEASE=1"
fi

# The version resource and its numbers come from the header, never retyped.
# "1.0-beta6" becomes file version 1,0,0,6 - the beta number in the fourth
# field - and a plain "1.0.3" would be 1.0.3.0, the same rule the PowerShell
# script applies.
VER=$(sed -n 's/^#define SH_VERSION "\(.*\)"/\1/p' scripthook.h | head -1)
[ -n "$VER" ] || { echo "SH_VERSION not found in scripthook.h"; exit 1; }
V4=$(printf '%s' "$VER" | awk '{
    v = $0; beta = 0;
    if (match(v, /-beta[0-9]+$/)) { beta = substr(v, RSTART + 5) + 0; v = substr(v, 1, RSTART - 1) }
    n = split(v, a, ".");
    printf "%d,%d,%d,%d\n", a[1], a[2], (n >= 3 ? a[3] : 0), beta
}')
case "$VER" in *beta*) FLAGS='VS_FF_PRERELEASE' ;; *) FLAGS='0' ;; esac

TMPD=$(cygpath -w /tmp/fwbuild)
rm -rf /tmp/fwbuild; mkdir -p /tmp/fwbuild
mkdir -p out
# cl wants Windows paths; $ROOT is an MSYS one (/f/...) and would be read
# as a relative \f\... - the reason the first link after the rewrite failed.
OUTW=$(cygpath -w "$ROOT/out/dinput8.dll")
DEFW=$(cygpath -w "$ROOT/proxy.def")
LIBW=$(cygpath -w "$ROOT/libscripthook.lib")

echo "version: $VER (file $V4, $FLAGS)"
rc /nologo "/fo$TMPD\\scripthook.res" "/dSH_FILEVER=$V4" \
   "/dSH_VERSTR=$VER" "/dSH_FILEFLAGS=$FLAGS" scripthook.rc
ml64 /nologo /c "/Fo$TMPD\\guard_pad.obj" guard.asm

CPPSRC="third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp \
third_party/imgui/backends/imgui_impl_dx11.cpp \
third_party/imgui/backends/imgui_impl_win32.cpp scripthook_ovl.cpp"
cl.exe -nologo -O2 -W3 -c -MT -std:c++17 -utf-8 -D_CRT_SECURE_NO_WARNINGS \
  -DSH_BUILD=1 $EXTRA_CPP -I. -Ithird_party/imgui -Ithird_party/imgui/backends \
  "-Fo$TMPD\\" $CPPSRC

CSRC="loader.c scripthook_api.c scripthook_config.c scripthook_text.c \
scripthook_tick.c scripthook_physics.c scripthook_health.c scripthook_state.c \
scripthook_playmode.c scripthook_blacklist.c scripthook_entity.c \
scripthook_spawn.c scripthook_npc.c scripthook_domino.c scripthook_hit.c \
scripthook_accuracy.c scripthook_camera.c scripthook_head.c scripthook_fov.c \
scripthook_blur.c scripthook_fpx.c scripthook_stat.c scripthook_resource.c \
scripthook_stealth.c scripthook_ammocap.c scripthook_weather.c \
scripthook_crash.c scripthook_input.c scripthook_havok.c scripthook_reflect.c \
scripthook_ui.c scripthook_scene.c scripthook_uiprop.c scripthook_uiinput.c \
scripthook_dinput.c scripthook_frame.c scripthook_hud.c scripthook_menu.c \
scripthook_draw.c guard.c scripthook_corefix.c scripthook_modsettings.c \
forge.c scripthook_forge.c scripthook_forge_io.c scripthook_forgeprobe.c \
scripthook_files.c scripthook_hooks.c \
third_party/minhook/src/buffer.c third_party/minhook/src/hook.c \
third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"

OBJS="$TMPD\\guard_pad.obj"
for n in imgui imgui_draw imgui_tables imgui_widgets imgui_impl_dx11 \
         imgui_impl_win32 scripthook_ovl; do
    OBJS="$OBJS $TMPD\\$n.obj"
done

cl.exe -nologo -O2 -W3 -LD -MT -std:c17 -utf-8 -I. -D_CRT_SECURE_NO_WARNINGS \
  $EXTRA_C "-Fo$TMPD\\" $CSRC "$TMPD\\scripthook.res" \
  "-Fe$OUTW" \
  -link "/DEF:$DEFW" "/IMPLIB:$LIBW" $OBJS \
  dinput8.lib dxguid.lib gdi32.lib user32.lib d3d11.lib dxgi.lib dwmapi.lib

# ---- export table, against the checked-in baseline ----------------------
# Written next to the build rather than to a temp file that has to be deleted:
# out/ is ignored by git, and keeping the last build's list around is what
# makes "which name appeared" answerable after the fact.
dumpbin -nologo -exports out/dinput8.dll \
  | awk '$0 ~ /^ +[0-9]+ +[0-9A-F]+ +[0-9A-F]+ +/ {print $4}' \
  | sort > out/exports-now.txt
# Line endings, not names: a Windows checkout with autocrlf hands us the
# baseline with CRLF while a generated list has LF, and comparing those two
# reports all 448 lines as changed. Normalise one side, never the file.
sed 's/\r$//' tools/exports-baseline.txt > out/exports-baseline.lf.txt
if diff -q out/exports-baseline.lf.txt out/exports-now.txt >/dev/null 2>&1; then
    echo "exports: $(wc -l < out/exports-now.txt | tr -d ' ') unchanged"
else
    echo "EXPORTS CHANGED - tools/exports-baseline.txt vs this build:"
    diff out/exports-baseline.lf.txt out/exports-now.txt | sed 's/^/  /'
fi

echo "FW_BUILD_OK out/dinput8.dll"
