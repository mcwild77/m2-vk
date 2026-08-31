#!/bin/sh
# Cross-builds the Modelizer libretro core for Android arm64 -> libmodelizer_libretro_android.so
#
#   ./devnotes/build-android.sh              incremental
#   REGENIE=1 ./devnotes/build-android.sh    after any scripts/ change, same rule as the host build
#
# This deliberately does NOT go through the top-level makefile's android-arm64 target.  That rule
# hard-codes --osd=sdl and demands SDL_INSTALL_ROOT (makefile:1195), neither of which applies to an
# OSD with no SDL in it; calling genie directly is shorter and leaves the upstream makefile alone.
#
# What the build needs that the repo does not carry:
#
#   ANDROID_NDK_HOME  the NDK root.  scripts/toolchain.lua:63 bakes this into the generated
#                     makefile at GENIE time, so it has to be set for the generate step and not
#                     merely for the compile step.  Defaults to the r27d this was built against.
#
# The core links no Vulkan library on Android either -- every entry point still comes from the
# frontend's vkGetInstanceProcAddr -- so the NDK's headers are all that is wanted.  They are NOT
# passed as the sysroot's whole usr/include: libretro_m2.lua turns M2VK_VULKAN_INCLUDEDIR into an
# ordinary -I, and putting the entire sysroot ahead of the toolchain's own search order is a good
# way to break libc++'s #include_next chain.  A directory holding nothing but a symlink to vulkan/
# has the same effect with none of the risk.
set -e

cd "$(dirname "$0")/.."

# EXE is the host executable suffix -- empty on unix, ".exe" on a Windows (MSYS2/Cygwin) host, where
# genie and the NDK's clang carry it and `[ -x ]` will not auto-append it the way exec does.
EXE=
case "$(uname -s)" in
	Darwin)              HOSTTAG=darwin-x86_64;  GENIEOS=darwin ;;
	Linux)               HOSTTAG=linux-x86_64;   GENIEOS=linux ;;
	MINGW*|MSYS*|CYGWIN*) HOSTTAG=windows-x86_64; GENIEOS=windows; EXE=.exe ;;
	*)      echo "unsupported build host: $(uname -s)" >&2; exit 1 ;;
esac

# Default the NDK root per host.  On Windows this repo's setup unzips r27d beside NVPACK's old r14b.
if [ -z "$ANDROID_NDK_HOME" ]; then
	case "$GENIEOS" in
		windows) ANDROID_NDK_HOME="/c/NVPACK/android-ndk-r27d" ;;
		*)       ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk/android-ndk-r27d" ;;
	esac
fi
export ANDROID_NDK_HOME

TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOSTTAG"
if [ ! -x "$TOOLCHAIN/bin/clang$EXE" ]; then
	echo "no NDK toolchain at '$TOOLCHAIN'." >&2
	echo "Set ANDROID_NDK_HOME to an NDK root (r27d is what this was written against)." >&2
	exit 1
fi

SYSROOT="$TOOLCHAIN/sysroot"
if [ ! -f "$SYSROOT/usr/include/vulkan/vulkan.h" ]; then
	echo "no vulkan/vulkan.h in the NDK sysroot at '$SYSROOT'." >&2
	exit 1
fi

VKINC=build/android-vulkan-include
mkdir -p "$VKINC"
rm -rf "$VKINC/vulkan"
# A directory holding nothing but the NDK's vulkan/ headers, turned into an ordinary -I by
# libretro_m2.lua.  A symlink is enough on unix; MSYS2's `ln -s` silently makes a copy (or fails)
# unless winsymlinks is on, so on Windows just copy the small header dir outright.
if [ -n "$EXE" ]; then
	cp -r "$SYSROOT/usr/include/vulkan" "$VKINC/vulkan"
else
	ln -s "$SYSROOT/usr/include/vulkan" "$VKINC/vulkan"
fi
M2VK_VULKAN_INCLUDEDIR=$(cd "$VKINC" && pwd)
export M2VK_VULKAN_INCLUDEDIR

CLANG_VERSION=$("$TOOLCHAIN/bin/clang" -dumpversion)
JOBS=${JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc || echo 4) )}

# Diagnostic per-device profiler.  PROFILER=1 ./devnotes/build-android.sh forwards --PROFILER=1 to
# genie, which defines MAME_PROFILER globally (scripts/genie.lua) so g_profiler collects and
# m2vk_profile.h dumps the per-device split to logcat.  Off by default: the shipping build must not
# carry the profiler's per-scope tick reads.  Because this flips a GLOBAL compile define (it changes
# g_profiler's type for every translation unit), turning it on or off needs BOTH `REGENIE=1` and a
# clean of the object tree (make will not rebuild on a flag-only change and would silently mix a real
# g_profiler with dummy ones) — e.g. `rm -rf build/android/obj && REGENIE=1 PROFILER=1 ./devnotes/build-android.sh`.
PROF_ARG=
if [ -n "${PROFILER:-}" ]; then
	PROF_ARG="PROFILER=$PROFILER"
fi

GENIE=3rdparty/genie/bin/$GENIEOS/genie$EXE
PROJDIR=build/projects/libretro_m2/mamemodelizer/gmake-android-arm64
# Repo root, next to model2_libretro.dylib: scripts/toolchain.lua would have put an android build
# under build/android/bin, but mainProject()'s ordinary (non-android-app) branch does
# targetdir(MAME_DIR) last and wins.  Objects still go to build/android/obj/arm64, which is what
# keeps this build from colliding with the host one.
OUT=libmodelizer_libretro_android.so

# PARAMS is harvested from the makefile rather than retyped, so an upstream change to the flag set
# reaches this build the way it reaches every other one.  The android-specific half is the same set
# the upstream android-arm64 rule passes, with --osd swapped: --NOASM=1 because the x86 DRC back end
# is not buildable here and every Model 2 CPU (i960, MB86233/5, 68000, Z80) has a C interpreter.
PARAMS=$(printf 'include makefile\n\nm2vk_pp:\n\t@echo $(PARAMS)\n' > build/.android-params.mk \
	&& make -f build/.android-params.mk m2vk_pp SUBTARGET=modelizer OSD=libretro_m2 NOWERROR=1 $PROF_ARG 2>/dev/null | tail -1)
rm -f build/.android-params.mk

if [ "$REGENIE" = 1 ] || [ ! -f "$PROJDIR/Makefile" ]; then
	make generate SUBTARGET=modelizer OSD=libretro_m2 NOWERROR=1 $PROF_ARG
	# shellcheck disable=SC2086
	"$GENIE" $PARAMS \
		--gcc=android-arm64 --gcc_version="$CLANG_VERSION" \
		--osd=libretro_m2 --targetos=android --PLATFORM=arm64 --NOASM=1 \
		gmake
fi

make -C "$PROJDIR" config=release -j"$JOBS" precompile
make -C "$PROJDIR" config=release -j"$JOBS"

ls -la "$OUT"
# Kept unstripped -- a first-generation port wants symbols when the phone produces a native crash.
# deploy-android.sh pushes a stripped copy; the debug info stays here for ndk-stack.
"$TOOLCHAIN/bin/llvm-readelf" -d "$OUT" | grep -E 'NEEDED|SONAME' || true
echo "exported retro_ entry points: $("$TOOLCHAIN/bin/llvm-nm" --dynamic --defined-only "$OUT" | grep -c ' T retro_')"
