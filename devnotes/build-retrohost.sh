#!/bin/sh
# Builds devnotes/retrohost, the minimal libretro host the A/B harness runs on.
#
# Two include paths and (almost) no libraries, which is the whole point:
#
#   src/osd/libretro_m2   libretro.h and libretro_vulkan.h, the same copies the core is built
#                         against, so the host and the core cannot disagree about a struct layout.
#   vulkan/vulkan.h       from vulkan-headers (macOS: brew install vulkan-headers; MSYS2:
#                         pacman -S mingw-w64-x86_64-vulkan-headers), pulled in by
#                         libretro_vulkan.h. Override with M2VK_VULKAN_INCLUDEDIR, exactly as
#                         scripts/src/osd/libretro_m2.lua does for the core.
#
# Nothing is linked against Vulkan. The host loads the platform's Vulkan library at run time and
# resolves every entry point through vkGetInstanceProcAddr, so a stray direct call to a Vulkan
# function is a link error rather than a hidden dependency — and the binary still runs on a machine
# with no Vulkan at all, where it is simply the software-only host it has always been.
#
# Hosts: macOS, Linux, and Windows through MSYS2's MINGW64 shell (NOT Git-for-Windows bash, which
# has no compiler).  The Windows link adds psapi (resident-size read-out) and winpthreads; the
# Vulkan loader is still not linked, only dlopened.
set -e

cd "$(dirname "$0")/.."

EXE=
LIBS=
case "$(uname -s)" in
	Darwin)
		DEFAULT_INC=/opt/homebrew/include
		;;
	MINGW*|MSYS*|CYGWIN*)
		EXE=.exe
		# MINGW_PREFIX is /mingw64 in the shell and the compiler understands it; genie, being a
		# native binary, does not, which is why libretro_m2.lua names the drive-letter form and
		# this script does not have to.
		DEFAULT_INC=${MINGW_PREFIX:-/mingw64}/include
		LIBS="-lpsapi"
		;;
	*)
		DEFAULT_INC=/usr/include
		LIBS="-ldl"
		;;
esac

INCDIR=${M2VK_VULKAN_INCLUDEDIR:-$DEFAULT_INC}
if [ ! -f "$INCDIR/vulkan/vulkan.h" ]; then
	echo "vulkan/vulkan.h not found under '$INCDIR'." >&2
	echo "macOS: brew install vulkan-headers.  MSYS2: pacman -S mingw-w64-x86_64-vulkan-headers." >&2
	echo "Or set M2VK_VULKAN_INCLUDEDIR." >&2
	exit 1
fi

CC=${CC:-cc}
# shellcheck disable=SC2086
$CC -O2 -Wall -Wextra -pthread -o "devnotes/retrohost$EXE" devnotes/retrohost.c \
	-I src/osd/libretro_m2 -I "$INCDIR" $LIBS

echo "built devnotes/retrohost$EXE"
