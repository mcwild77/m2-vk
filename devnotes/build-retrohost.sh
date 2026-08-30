#!/bin/sh
# Builds devnotes/retrohost, the minimal libretro host the A/B harness runs on.
#
# Two include paths and no libraries, which is the whole point:
#
#   src/osd/libretro_m2   libretro.h and libretro_vulkan.h, the same copies the core is built
#                         against, so the host and the core cannot disagree about a struct layout.
#   vulkan/vulkan.h       from vulkan-headers (brew install vulkan-headers), pulled in by
#                         libretro_vulkan.h. Override with M2VK_VULKAN_INCLUDEDIR, exactly as
#                         scripts/src/osd/libretro_m2.lua does for the core.
#
# Nothing is linked against Vulkan. The host dlopens MoltenVK at run time and resolves every entry
# point through vkGetInstanceProcAddr, so a stray direct call to a Vulkan function is a link error
# rather than a hidden dependency — and the binary still runs on a machine with no Vulkan at all,
# where it is simply the software-only host it has always been.
set -e

cd "$(dirname "$0")/.."

INCDIR=${M2VK_VULKAN_INCLUDEDIR:-/opt/homebrew/include}
if [ ! -f "$INCDIR/vulkan/vulkan.h" ]; then
	echo "vulkan/vulkan.h not found under '$INCDIR'." >&2
	echo "brew install vulkan-headers, or set M2VK_VULKAN_INCLUDEDIR." >&2
	exit 1
fi

cc -O2 -Wall -Wextra -o devnotes/retrohost devnotes/retrohost.c \
	-I src/osd/libretro_m2 -I "$INCDIR"

echo "built devnotes/retrohost"
