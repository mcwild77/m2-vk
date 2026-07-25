#!/usr/bin/env bash
#
# Compiles this directory's GLSL into the committed SPIR-V headers next to it.
#
# Run by hand, and only when a shader changes. The generated *_spv.h files are checked in, which is
# a deliberate choice: a genie custom build rule would make glslc a build requirement for everyone
# who touches the tree, and these shaders change roughly never. See
# devnotes/p2-vulkan-passthrough.md.
#
#   brew install shaderc      # macOS; otherwise your distribution's shaderc / glslang package
#   ./build_shaders.sh
#
# The words are emitted as uint32_t rather than as bytes on purpose. vkCreateShaderModule takes a
# const uint32_t* and requires it to be 4-byte aligned; an unsigned char[] carries no such
# guarantee, and the resulting misalignment is undefined behaviour that happens to work until it
# does not.

set -euo pipefail

GLSLC="${GLSLC:-glslc}"
command -v "$GLSLC" >/dev/null 2>&1 || {
	echo "build_shaders.sh: no glslc on PATH; set GLSLC or 'brew install shaderc'" >&2
	exit 1
}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# sed rather than head: head closes the pipe after the first line, glslc takes a SIGPIPE for it, and
# `set -o pipefail` above turns that into a failed build with nothing to show for it.
GLSLC_VERSION="$("$GLSLC" --version | sed -n 1p)"

emit() { # <shader-stage> <source> <symbol-stem>
	local stage="$1" src="$2" stem="$3"
	local upper
	upper="$(printf '%s' "$stem" | tr '[:lower:]' '[:upper:]')"

	# --target-env=vulkan1.0: the physical device RetroArch's MoltenVK hands over reports apiVersion
	# 1.1.0, and nothing in these shaders wants anything past core 1.0. -O because the blob is
	# committed and a debug one is several times the size for no benefit.
	"$GLSLC" --target-env=vulkan1.0 -O -fshader-stage="$stage" "$HERE/$src" -o "$TMP/$stem.spv"

	{
		echo "// license:BSD-3-Clause"
		echo "// copyright-holders:mcwild77"
		echo ""
		echo "// Generated from $src by build_shaders.sh — do not edit."
		echo "// $GLSLC_VERSION"
		echo ""
		echo "#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_SHADERS_${upper}_SPV_H"
		echo "#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_SHADERS_${upper}_SPV_H"
		echo ""
		echo "#pragma once"
		echo ""
		echo "#include <cstdint>"
		echo ""
		echo "static const uint32_t ${upper}_SPV[] = {"
		od -An -tx4 -v "$TMP/$stem.spv" | awk 'NF {
			line = ""
			for (i = 1; i <= NF; i++)
				line = line sprintf("0x%s,%s", $i, (i == NF) ? "" : " ")
			print "\t" line
		}'
		echo "};"
		echo ""
		echo "#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_SHADERS_${upper}_SPV_H"
	} > "$HERE/${stem}_spv.h"

	echo "  $src -> ${stem}_spv.h ($(wc -c < "$TMP/$stem.spv" | tr -d ' ') bytes of SPIR-V)"
}

echo "$GLSLC_VERSION"
emit vert fullscreen.vert  fullscreen_vert
emit frag passthrough.frag passthrough_frag
