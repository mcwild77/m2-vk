// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — Vulkan entry points.

    Everything Vulkan in this core is reached through function pointers resolved at run time from
    the vkGetInstanceProcAddr the frontend hands over, and from vkGetDeviceProcAddr for device-level
    calls. The core links no Vulkan library and has no Vulkan library on its link line: on macOS
    there is no system loader, and the frontend is talking to an implementation it ships itself
    (RetroArch bundles MoltenVK). Linking anything of our own would either fail to load or load a
    second, unrelated implementation.

    The practical consequence is that model2_libretro.dylib stays loadable on a machine with no
    Vulkan at all, which is what makes the software renderer an honest fallback rather than a
    theoretical one.

    Only the headers are a build-time requirement. Their location is the M2VK_VULKAN_INCLUDEDIR
    environment variable read by scripts/src/osd/libretro_m2.lua, defaulting to Homebrew's prefix on
    macOS.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_FUNCS_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_FUNCS_H

#pragma once

namespace m2vk {

// One line naming the Vulkan and libretro-Vulkan headers this core was compiled against. Whatever
// the frontend turns out to be running is a separate question, answered at context_reset; the two
// need not match, and the difference is the first thing worth knowing when they disagree.
//
// The returned string has static storage duration and is built on first call.
const char *vk_build_info();

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_FUNCS_H
