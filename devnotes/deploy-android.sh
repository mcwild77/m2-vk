#!/bin/sh
# Pushes the Android core (and optionally ROMs) to a connected device over adb.
#
#   ./devnotes/deploy-android.sh              core only
#   ./devnotes/deploy-android.sh vf2 daytona  core + those sets from devnotes/roms
#   ./devnotes/deploy-android.sh all          core + the whole of devnotes/roms (~712 MB)
#
# 🚨 The core is pushed to RetroArch's *downloads* directory, not to its cores directory, and that
# is not a workaround -- it is the supported route.  RetroArch Android keeps its cores under
# /data/user/0/<pkg>/cores, which the adb shell user cannot write to on a production build (no root
# on the Odin 2).  RetroArch's own "Load Core -> Install or Restore a Core" browses the downloads
# directory and copies the .so into place itself, with the right permissions.  Redirecting
# libretro_directory at /sdcard instead would work too and is what an earlier version of this script
# did -- but it hides every other core the device already has, which on a handheld somebody actually
# plays is a rude thing to do for a test build.
#
# Both directories are read out of the device's own retroarch.cfg rather than assumed, because the
# config lives in Android/data (adb-readable) while the paths it names may be anywhere.
set -e

cd "$(dirname "$0")/.."

CORE=libmodelizer_libretro_android.so
[ -f "$CORE" ] || { echo "no $CORE -- run ./devnotes/build-android.sh first" >&2; exit 1; }

command -v adb >/dev/null || { echo "adb not on PATH (brew install --cask android-platform-tools; on Windows it ships with the SDK's platform-tools, e.g. C:\\NVPACK\\android-sdk-windows\\platform-tools)" >&2; exit 1; }
if [ -z "$(adb devices | sed -n '2p')" ]; then
	echo "no device: check the cable, and that USB debugging is on and this host authorised." >&2
	adb devices
	exit 1
fi

PKG=${RETROARCH_PKG:-com.retroarch.aarch64}
CFG=/storage/emulated/0/Android/data/$PKG/files/retroarch.cfg
adb shell "[ -f $CFG ]" || { echo "no retroarch.cfg at $CFG -- is $PKG installed and launched once?" >&2; exit 1; }

cfgval() { adb shell "grep -m1 '^$1' $CFG" | sed 's/.*= *"//; s/"[[:space:]]*$//' | tr -d '\r'; }
DOWNLOADS=$(cfgval core_assets_directory)
SYSDIR=$(cfgval system_directory)
VIDEO=$(cfgval video_driver)

# 🚨 ROMs live on the SD CARD, not on internal storage.  The card is found by its fsLabel rather
# than by its UUID-named mount point, because the mount point is the volume's UUID (F8B2-FD4C here)
# and would change with the card.  ROMS/model2 is an existing ES-DE system folder -- it already had
# a systeminfo.txt in it before any of this -- so this drops into a tree the device already uses
# rather than inventing a parallel one.
SDLABEL=${M2VK_ANDROID_SDLABEL:-RPFlip2}
SDPATH=$(adb shell "dumpsys mount 2>/dev/null" | tr -d '\r' | awk -v l="fsLabel=$SDLABEL" '
	$0 ~ l {found=1} found && /path=\/storage\// {sub(/.*path=/,""); sub(/ .*/,""); print; exit}')
if [ -n "$M2VK_ANDROID_ROMDIR" ]; then
	ROMDIR=$M2VK_ANDROID_ROMDIR
elif [ -n "$SDPATH" ]; then
	ROMDIR=$SDPATH/ROMS/model2
else
	echo "no SD card labelled '$SDLABEL' is mounted." >&2
	echo "Insert it, or set M2VK_ANDROID_ROMDIR to where the sets should go." >&2
	exit 1
fi

echo "device      $(adb shell getprop ro.product.model | tr -d '\r')  ($(adb shell getprop ro.product.cpu.abi | tr -d '\r'))"
echo "downloads   $DOWNLOADS"
echo "system      $SYSDIR"
echo "roms        $ROMDIR"
echo "video       $VIDEO"
[ "$VIDEO" = vulkan ] || echo "  ⚠️  video_driver is not vulkan -- the core declares RETRO_HW_CONTEXT_VULKAN and will not load"
echo

# The build keeps its debug info so ndk-stack can symbolise a native crash; what gets pushed does
# not need it, and unstripped this is 90 MB across a USB cable.
EXE=
case "$(uname -s)" in
	Darwin)              HOSTTAG=darwin-x86_64 ;;
	MINGW*|MSYS*|CYGWIN*) HOSTTAG=windows-x86_64; EXE=.exe ;;
	*)                   HOSTTAG=linux-x86_64 ;;
esac
if [ -z "$ANDROID_NDK_HOME" ]; then
	case "$HOSTTAG" in
		windows-x86_64) ANDROID_NDK_HOME=/c/NVPACK/android-ndk-r27d ;;
		*)              ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/android-ndk-r27d ;;
	esac
fi
STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOSTTAG/bin/llvm-strip$EXE"
PUSH=$CORE
if [ -x "$STRIP" ]; then
	mkdir -p build/android
	PUSH=build/android/$CORE
	"$STRIP" -o "$PUSH" "$CORE"
	echo "stripped $(du -h "$CORE" | cut -f1) -> $(du -h "$PUSH" | cut -f1)"
fi

adb shell mkdir -p "$DOWNLOADS" "$ROMDIR"
adb push "$PUSH" "$DOWNLOADS/"

# ⚠️ Nothing is pushed to <system dir>/model2, the core's SECOND rompath (retro_entry.cpp:669).
# The first is the content's own directory, so device and BIOS sets -- segabill.zip for vonj, the
# model1io2 BIOS merged into vcop.zip -- work by sitting beside the games, which is how
# devnotes/roms is arranged on the desktop side too.  One directory, one answer to "where is it".
#
# `all` mirrors the whole of devnotes/roms.  🚨 That INCLUDES the two loose-file directories
# (manxttc/, overrev/): each holds a BIOS the matching zip does not, MAME reads a rompath entry
# named after the set as loose files, and a copy that skips them is the vcop trap again -- the sets
# look present and fail at load.
push_set() {
	if [ -e "devnotes/roms/$1.zip" ]; then adb push "devnotes/roms/$1.zip" "$ROMDIR/"; fi
	if [ -d "devnotes/roms/$1" ]; then adb push "devnotes/roms/$1" "$ROMDIR/"; fi
	if [ ! -e "devnotes/roms/$1.zip" ] && [ ! -d "devnotes/roms/$1" ]; then
		echo "no devnotes/roms/$1 -- skipped" >&2
	fi
}

for rom in "$@"; do
	if [ "$rom" = all ]; then
		for f in devnotes/roms/*.zip devnotes/roms/*/; do
			case "$f" in *.bak) continue;; esac        # vcop.zip.bak is the pre-fix copy
			push_set "$(basename "${f%.zip}" | sed 's:/$::')"
		done
	else
		push_set "$rom"
	fi
done

cat <<EOF

pushed.  On the device:
  Main Menu -> Load Core -> Install or Restore a Core -> $CORE
    (RetroArch copies it into its own cores directory; repeat this after every rebuild)
  Main Menu -> Load Content -> $ROMDIR

⚠️  There is no .info file for this core, so it lists by filename and the content browser does not
    filter by extension.  Harmless for testing; it is not a sign the core failed to install.

Log:  adb logcat -c && adb logcat -s RetroArch:V
Crash symbols:  adb logcat | \$ANDROID_NDK_HOME/ndk-stack -sym .
EOF
