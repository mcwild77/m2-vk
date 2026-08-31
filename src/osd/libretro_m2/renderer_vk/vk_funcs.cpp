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

bool load_funcs(vk_funcs &fns, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
		VkInstance instance, VkDevice device)
{
	fns = vk_funcs{};

	if ((gipa == nullptr) || (instance == VK_NULL_HANDLE))
	{
		vk_log(RETRO_LOG_ERROR, "the frontend supplied no instance loader\n");
		return false;
	}
	if (device == VK_NULL_HANDLE)
	{
		vk_log(RETRO_LOG_ERROR, "the frontend supplied no device\n");
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

	if (fns.get_device_proc_addr == nullptr)
		return false;

	// Device level. Resolved through the device loader rather than the instance one, which is not
	// pedantry: the instance loader would answer for a name the device cannot actually dispatch, and
	// the resulting call goes through a trampoline that has no business being in a per-frame path.
	auto const resolve_device = [&](char const *name) -> PFN_vkVoidFunction
	{
		PFN_vkVoidFunction const p = fns.get_device_proc_addr(device, name);
		if (p == nullptr)
		{
			vk_log(RETRO_LOG_ERROR, "%s could not be resolved against the frontend's device\n", name);
			ok = false;
		}
		return p;
	};

#define M2VK_RESOLVE_DEVICE(field, type, name) \
		fns.field = reinterpret_cast<type>(resolve_device(name))

	M2VK_RESOLVE_DEVICE(device_wait_idle,                 PFN_vkDeviceWaitIdle,                 "vkDeviceWaitIdle");
	M2VK_RESOLVE_DEVICE(queue_submit,                     PFN_vkQueueSubmit,                    "vkQueueSubmit");
	M2VK_RESOLVE_DEVICE(create_fence,                     PFN_vkCreateFence,                    "vkCreateFence");
	M2VK_RESOLVE_DEVICE(destroy_fence,                    PFN_vkDestroyFence,                   "vkDestroyFence");
	M2VK_RESOLVE_DEVICE(wait_for_fences,                  PFN_vkWaitForFences,                  "vkWaitForFences");
	M2VK_RESOLVE_DEVICE(get_fence_status,                 PFN_vkGetFenceStatus,                 "vkGetFenceStatus");
	M2VK_RESOLVE_DEVICE(reset_fences,                     PFN_vkResetFences,                    "vkResetFences");

	M2VK_RESOLVE_DEVICE(allocate_memory,                  PFN_vkAllocateMemory,                 "vkAllocateMemory");
	M2VK_RESOLVE_DEVICE(free_memory,                      PFN_vkFreeMemory,                     "vkFreeMemory");
	M2VK_RESOLVE_DEVICE(map_memory,                       PFN_vkMapMemory,                      "vkMapMemory");
	M2VK_RESOLVE_DEVICE(unmap_memory,                     PFN_vkUnmapMemory,                    "vkUnmapMemory");

	M2VK_RESOLVE_DEVICE(create_image,                     PFN_vkCreateImage,                    "vkCreateImage");
	M2VK_RESOLVE_DEVICE(destroy_image,                    PFN_vkDestroyImage,                   "vkDestroyImage");
	M2VK_RESOLVE_DEVICE(get_image_memory_requirements,    PFN_vkGetImageMemoryRequirements,     "vkGetImageMemoryRequirements");
	M2VK_RESOLVE_DEVICE(bind_image_memory,                PFN_vkBindImageMemory,                "vkBindImageMemory");
	M2VK_RESOLVE_DEVICE(create_image_view,                PFN_vkCreateImageView,                "vkCreateImageView");
	M2VK_RESOLVE_DEVICE(destroy_image_view,               PFN_vkDestroyImageView,               "vkDestroyImageView");
	M2VK_RESOLVE_DEVICE(create_sampler,                   PFN_vkCreateSampler,                  "vkCreateSampler");
	M2VK_RESOLVE_DEVICE(destroy_sampler,                  PFN_vkDestroySampler,                 "vkDestroySampler");

	M2VK_RESOLVE_DEVICE(create_buffer,                    PFN_vkCreateBuffer,                   "vkCreateBuffer");
	M2VK_RESOLVE_DEVICE(destroy_buffer,                   PFN_vkDestroyBuffer,                  "vkDestroyBuffer");
	M2VK_RESOLVE_DEVICE(get_buffer_memory_requirements,   PFN_vkGetBufferMemoryRequirements,    "vkGetBufferMemoryRequirements");
	M2VK_RESOLVE_DEVICE(bind_buffer_memory,               PFN_vkBindBufferMemory,               "vkBindBufferMemory");

	M2VK_RESOLVE_DEVICE(create_descriptor_set_layout,     PFN_vkCreateDescriptorSetLayout,      "vkCreateDescriptorSetLayout");
	M2VK_RESOLVE_DEVICE(destroy_descriptor_set_layout,    PFN_vkDestroyDescriptorSetLayout,     "vkDestroyDescriptorSetLayout");
	M2VK_RESOLVE_DEVICE(create_descriptor_pool,           PFN_vkCreateDescriptorPool,           "vkCreateDescriptorPool");
	M2VK_RESOLVE_DEVICE(destroy_descriptor_pool,          PFN_vkDestroyDescriptorPool,          "vkDestroyDescriptorPool");
	M2VK_RESOLVE_DEVICE(allocate_descriptor_sets,         PFN_vkAllocateDescriptorSets,         "vkAllocateDescriptorSets");
	M2VK_RESOLVE_DEVICE(update_descriptor_sets,           PFN_vkUpdateDescriptorSets,           "vkUpdateDescriptorSets");

	M2VK_RESOLVE_DEVICE(create_shader_module,             PFN_vkCreateShaderModule,             "vkCreateShaderModule");
	M2VK_RESOLVE_DEVICE(destroy_shader_module,            PFN_vkDestroyShaderModule,            "vkDestroyShaderModule");
	M2VK_RESOLVE_DEVICE(create_render_pass,               PFN_vkCreateRenderPass,               "vkCreateRenderPass");
	M2VK_RESOLVE_DEVICE(destroy_render_pass,              PFN_vkDestroyRenderPass,              "vkDestroyRenderPass");
	M2VK_RESOLVE_DEVICE(create_framebuffer,               PFN_vkCreateFramebuffer,              "vkCreateFramebuffer");
	M2VK_RESOLVE_DEVICE(destroy_framebuffer,              PFN_vkDestroyFramebuffer,             "vkDestroyFramebuffer");
	M2VK_RESOLVE_DEVICE(create_pipeline_layout,           PFN_vkCreatePipelineLayout,           "vkCreatePipelineLayout");
	M2VK_RESOLVE_DEVICE(destroy_pipeline_layout,          PFN_vkDestroyPipelineLayout,          "vkDestroyPipelineLayout");
	M2VK_RESOLVE_DEVICE(create_graphics_pipelines,        PFN_vkCreateGraphicsPipelines,        "vkCreateGraphicsPipelines");
	M2VK_RESOLVE_DEVICE(destroy_pipeline,                 PFN_vkDestroyPipeline,                "vkDestroyPipeline");

	M2VK_RESOLVE_DEVICE(create_command_pool,              PFN_vkCreateCommandPool,              "vkCreateCommandPool");
	M2VK_RESOLVE_DEVICE(destroy_command_pool,             PFN_vkDestroyCommandPool,             "vkDestroyCommandPool");
	M2VK_RESOLVE_DEVICE(reset_command_pool,               PFN_vkResetCommandPool,               "vkResetCommandPool");
	M2VK_RESOLVE_DEVICE(allocate_command_buffers,         PFN_vkAllocateCommandBuffers,         "vkAllocateCommandBuffers");
	M2VK_RESOLVE_DEVICE(begin_command_buffer,             PFN_vkBeginCommandBuffer,             "vkBeginCommandBuffer");
	M2VK_RESOLVE_DEVICE(end_command_buffer,               PFN_vkEndCommandBuffer,               "vkEndCommandBuffer");
	M2VK_RESOLVE_DEVICE(cmd_pipeline_barrier,             PFN_vkCmdPipelineBarrier,             "vkCmdPipelineBarrier");
	M2VK_RESOLVE_DEVICE(cmd_copy_buffer_to_image,         PFN_vkCmdCopyBufferToImage,           "vkCmdCopyBufferToImage");
	M2VK_RESOLVE_DEVICE(cmd_copy_image_to_buffer,         PFN_vkCmdCopyImageToBuffer,           "vkCmdCopyImageToBuffer");
	M2VK_RESOLVE_DEVICE(cmd_begin_render_pass,            PFN_vkCmdBeginRenderPass,             "vkCmdBeginRenderPass");
	M2VK_RESOLVE_DEVICE(cmd_end_render_pass,              PFN_vkCmdEndRenderPass,               "vkCmdEndRenderPass");
	M2VK_RESOLVE_DEVICE(cmd_bind_pipeline,                PFN_vkCmdBindPipeline,                "vkCmdBindPipeline");
	M2VK_RESOLVE_DEVICE(cmd_bind_descriptor_sets,         PFN_vkCmdBindDescriptorSets,          "vkCmdBindDescriptorSets");
	M2VK_RESOLVE_DEVICE(cmd_set_viewport,                 PFN_vkCmdSetViewport,                 "vkCmdSetViewport");
	M2VK_RESOLVE_DEVICE(cmd_set_scissor,                  PFN_vkCmdSetScissor,                  "vkCmdSetScissor");
	M2VK_RESOLVE_DEVICE(cmd_draw,                         PFN_vkCmdDraw,                        "vkCmdDraw");

	M2VK_RESOLVE_DEVICE(cmd_bind_vertex_buffers,          PFN_vkCmdBindVertexBuffers,           "vkCmdBindVertexBuffers");
	M2VK_RESOLVE_DEVICE(cmd_bind_index_buffer,            PFN_vkCmdBindIndexBuffer,             "vkCmdBindIndexBuffer");
	M2VK_RESOLVE_DEVICE(cmd_draw_indexed,                 PFN_vkCmdDrawIndexed,                 "vkCmdDrawIndexed");
	M2VK_RESOLVE_DEVICE(cmd_push_constants,               PFN_vkCmdPushConstants,               "vkCmdPushConstants");

#undef M2VK_RESOLVE_DEVICE

	return ok;
}


//============================================================
//  result names
//============================================================

char const *vk_result_name(VkResult result)
{
	switch (result)
	{
	case VK_SUCCESS:                        return "VK_SUCCESS";
	case VK_NOT_READY:                      return "VK_NOT_READY";
	case VK_TIMEOUT:                        return "VK_TIMEOUT";
	case VK_EVENT_SET:                      return "VK_EVENT_SET";
	case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
	case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
	case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
	case VK_ERROR_OUT_OF_POOL_MEMORY:       return "VK_ERROR_OUT_OF_POOL_MEMORY";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE:  return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
	default:                                break;
	}

	static char buf[24];
	std::snprintf(buf, sizeof(buf), "VkResult %d", int(result));
	return buf;
}


//============================================================
//  allocation
//============================================================

bool find_memory_type(vk_funcs const &fns, VkPhysicalDevice gpu, uint32_t type_bits,
		VkMemoryPropertyFlags want, uint32_t &out)
{
	VkPhysicalDeviceMemoryProperties mem{};
	fns.get_physical_device_memory_properties(gpu, &mem);

	for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
	{
		if (((type_bits & (uint32_t(1) << i)) != 0) && ((mem.memoryTypes[i].propertyFlags & want) == want))
		{
			out = i;
			return true;
		}
	}
	return false;
}

} // namespace m2vk
