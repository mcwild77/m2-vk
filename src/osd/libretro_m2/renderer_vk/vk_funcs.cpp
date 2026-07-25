// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — Vulkan entry points.

    This is the one translation unit that pulls in <vulkan/vulkan.h> and the vendored
    libretro_vulkan.h; see vk_funcs.h for why nothing is linked.

*********************************************************************************************************************************/

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace m2vk {

namespace {

// The header set has to be able to describe the interface the frontend implements. Version 5 is
// what RetroArch 1.22 offers and what libretro_vulkan.h was vendored at; a newer header is fine
// (the struct only grows at the end), an older one would mean the vendored copy went backwards.
static_assert(RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION >= 5,
		"vendored libretro_vulkan.h is older than the interface RetroArch implements");

// Vulkan 1.0 is the floor this core targets — MoltenVK sets the ceiling, not us, and passthrough
// needs nothing above core 1.0. Anything the headers offer beyond that is opt-in, per feature,
// after context_reset has said what the device actually supports.
static_assert(VK_HEADER_VERSION_COMPLETE >= VK_MAKE_API_VERSION(0, 1, 0, 0),
		"vulkan headers too old");


retro_log_printf_t s_log_cb = nullptr;

} // anonymous namespace


//============================================================
//  build identity
//============================================================

const char *vk_build_info()
{
	static const std::string s = []
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf),
				"vulkan headers %u.%u.%u, libretro vulkan interface v%u (negotiation v%u)",
				VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),
				VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE),
				VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE),
				unsigned(RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION),
				unsigned(RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION));
		return std::string(buf);
	}();

	return s.c_str();
}


//============================================================
//  logging
//============================================================

void set_log(retro_log_printf_t cb)
{
	s_log_cb = cb;
}

void vk_log(retro_log_level level, char const *fmt, ...)
{
	if (s_log_cb == nullptr)
		return;

	// Formatted here rather than forwarded: the frontend's callback is printf-shaped, so passing a
	// caller's format string straight through would make any stray '%' in a device name — and
	// device names come from the driver — someone else's problem to survive.
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	s_log_cb(level, "[model2] vk: %s", buf);
}


//============================================================
//  the function table
//============================================================

bool load_funcs(vk_funcs &fns, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, VkInstance instance)
{
	fns = vk_funcs{};

	if ((gipa == nullptr) || (instance == VK_NULL_HANDLE))
	{
		vk_log(RETRO_LOG_ERROR, "the frontend supplied no instance loader\n");
		return false;
	}

	bool ok = true;
	auto const resolve = [&](char const *name, bool required) -> PFN_vkVoidFunction
	{
		PFN_vkVoidFunction const p = gipa(instance, name);
		if ((p == nullptr) && required)
		{
			vk_log(RETRO_LOG_ERROR, "%s could not be resolved\n", name);
			ok = false;
		}
		return p;
	};

	fns.get_instance_proc_addr = gipa;

	// The interface hands us a device loader as well; prefer it, and fall back to asking the
	// instance loader for the same thing rather than assuming the field is populated.
	fns.get_device_proc_addr = (gdpa != nullptr)
			? gdpa
			: reinterpret_cast<PFN_vkGetDeviceProcAddr>(resolve("vkGetDeviceProcAddr", true));

	// Global-level: queried with no instance, and legitimately absent on a 1.0 loader.
	fns.enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
			gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));

	fns.get_physical_device_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
			resolve("vkGetPhysicalDeviceProperties", true));
	fns.get_physical_device_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
			resolve("vkGetPhysicalDeviceFeatures", true));
	fns.get_physical_device_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
			resolve("vkGetPhysicalDeviceMemoryProperties", true));
	fns.get_physical_device_queue_family_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
			resolve("vkGetPhysicalDeviceQueueFamilyProperties", true));
	fns.get_physical_device_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
			resolve("vkGetPhysicalDeviceFormatProperties", true));
	fns.enumerate_device_extension_properties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
			resolve("vkEnumerateDeviceExtensionProperties", true));

	return ok;
}

} // namespace m2vk
