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

#include "libretro.h"

#include <vulkan/vulkan.h>


#if defined(__GNUC__) || defined(__clang__)
#define M2VK_PRINTF(fmtarg, firstvararg) __attribute__((format(printf, fmtarg, firstvararg)))
#else
#define M2VK_PRINTF(fmtarg, firstvararg)
#endif


namespace m2vk {

//============================================================
//  build identity
//============================================================

// One line naming the Vulkan and libretro-Vulkan headers this core was compiled against. Whatever
// the frontend turns out to be running is a separate question, answered at context_reset; the two
// need not match, and the difference is the first thing worth knowing when they disagree.
//
// The returned string has static storage duration and is built on first call.
const char *vk_build_info();


//============================================================
//  logging
//============================================================

// The renderer logs through the frontend like everything else in this core, but it is several
// translation units away from the entry points that own the callback, and it has no MAME osd_printf
// available either (nothing here runs on the emulation thread). So the callback is handed over once
// and reached through vk_log(), which prefixes every line the same way and is a no-op until then.
void set_log(retro_log_printf_t cb);
void vk_log(retro_log_level level, char const *fmt, ...) M2VK_PRINTF(2, 3);


//============================================================
//  the function table
//============================================================

// Instance-level entry points, resolved from the frontend's vkGetInstanceProcAddr against the
// frontend's VkInstance. Device-level calls come from get_device_proc_addr and are added to this
// table as the phases that need them arrive; nothing above core Vulkan 1.0 belongs here without a
// run-time check that the device supports it.
struct vk_funcs
{
	PFN_vkGetInstanceProcAddr                       get_instance_proc_addr = nullptr;
	PFN_vkGetDeviceProcAddr                         get_device_proc_addr = nullptr;

	// Global-level (instance == VK_NULL_HANDLE), and absent on a strictly 1.0 loader — hence
	// optional. The device's own apiVersion is what actually bounds us; this only says what the
	// loader in front of it will admit to.
	PFN_vkEnumerateInstanceVersion                  enumerate_instance_version = nullptr;

	PFN_vkGetPhysicalDeviceProperties               get_physical_device_properties = nullptr;
	PFN_vkGetPhysicalDeviceFeatures                 get_physical_device_features = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties         get_physical_device_memory_properties = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties    get_physical_device_queue_family_properties = nullptr;
	PFN_vkGetPhysicalDeviceFormatProperties         get_physical_device_format_properties = nullptr;
	PFN_vkEnumerateDeviceExtensionProperties        enumerate_device_extension_properties = nullptr;
};

// Fills the table. Returns false if a required entry point is missing, having logged which one:
// that would mean the frontend handed over a loader that cannot answer for its own instance, and
// the only safe response is to leave Vulkan alone.
bool load_funcs(vk_funcs &fns, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, VkInstance instance);

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_FUNCS_H
