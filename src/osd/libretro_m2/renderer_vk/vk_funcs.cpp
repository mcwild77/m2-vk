// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — Vulkan entry points.

    This is the one translation unit that pulls in <vulkan/vulkan.h> and the vendored
    libretro_vulkan.h; see vk_funcs.h for why nothing is linked.

*********************************************************************************************************************************/

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

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

} // anonymous namespace

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

} // namespace m2vk
