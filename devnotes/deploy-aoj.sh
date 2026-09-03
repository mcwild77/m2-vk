#!/bin/sh
# Installs the Android core into the Age of Joy Unity project, which is the shipping consumer.
#
#   ./devnotes/deploy-aoj.sh                 strip and copy
#   M2VK_AOJ_DIR=<unity project> ./devnotes/deploy-aoj.sh
#
# This is the OTHER Quest arm, and it is not interchangeable with deploy-android.sh:
#
#   deploy-android.sh   pushes to RetroArch on the headset.  Has a core-options menu and logcat.
#                       The debugging arm.
#   deploy-aoj.sh       puts the core in Unity's Assets/Plugins/Android64, where it is packed into
#                       the APK.  There is no core-options menu in Age of Joy -- options come from
#                       each cabinet's description.yaml `environment:` block, pushed through
#                       LibretroModelizerCore.cs.  So an option that was only ever set in
#                       RetroArch's menu is NOT set here.  The shipping arm.
#
# 🚨 The core is STRIPPED on the way in.  build-android.sh deliberately leaves debug info in the
# build (ndk-stack wants it), which is 103 MB of a ~1.2 GB APK for symbols no player can use.  The
# unstripped copy stays in the repo root, so a native crash from the headset can still be
# symbolised against it.
#
# Unity's .so.meta file is not touched: Unity owns it, it keys off the file name rather than the
# contents, and a plugin's import settings (arm64, Android) survive the file being replaced.
set -e

cd "$(dirname "$0")/.."

CORE=libmodelizer_libretro_android.so
[ -f "$CORE" ] || { echo "no $CORE -- run ./devnotes/build-android.sh first" >&2; exit 1; }

AOJ=${M2VK_AOJ_DIR:-/e/AgeOfJoy-2022.1_curif}
DEST=$AOJ/Assets/Plugins/Android64
[ -d "$DEST" ] || {
	echo "no Unity plugin directory at '$DEST'." >&2
	echo "Set M2VK_AOJ_DIR to the Age of Joy project root." >&2
	exit 1
}

EXE=
case "$(uname -s)" in
	Darwin)               HOSTTAG=darwin-x86_64 ;;
	MINGW*|MSYS*|CYGWIN*) HOSTTAG=windows-x86_64; EXE=.exe ;;
	*)                    HOSTTAG=linux-x86_64 ;;
esac
if [ -z "$ANDROID_NDK_HOME" ]; then
	case "$HOSTTAG" in
		windows-x86_64) ANDROID_NDK_HOME=/c/NVPACK/android-ndk-r27d ;;
		*)              ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/android-ndk-r27d ;;
	esac
fi
STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOSTTAG/bin/llvm-strip$EXE"

mkdir -p build/android
PUSH=build/android/$CORE
if [ -x "$STRIP" ]; then
	"$STRIP" -o "$PUSH" "$CORE"
	echo "stripped $(du -h "$CORE" | cut -f1) -> $(du -h "$PUSH" | cut -f1)"
else
	echo "⚠️  no llvm-strip at '$STRIP' -- copying the unstripped core ($(du -h "$CORE" | cut -f1))" >&2
	cp "$CORE" "$PUSH"
fi

cp "$PUSH" "$DEST/$CORE"
ls -la "$DEST/$CORE"

cat <<EOF

installed into the Unity project.  Next:
  Unity -> Build (Android, arm64, IL2CPP)  ->  the APK
  adb install -r <apk>                     (or the project's own _pushtoheadset.bat)

⚠️  Unity caches native plugins in Library/Bee -- a build that still ships the old core means the
    import did not re-run.  Re-importing Assets/Plugins/Android64 in the editor forces it.
EOF
