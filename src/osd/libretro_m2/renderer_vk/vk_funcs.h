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

// Every entry point this core calls, in one table. The instance-level half is resolved from the
// frontend's vkGetInstanceProcAddr against the frontend's VkInstance; the device-level half from
// vkGetDeviceProcAddr against the frontend's VkDevice. Both halves are filled in one go at
// context_reset and the whole table is dropped at context_destroy, because after that every handle
// it was resolved against is dead.
//
// The table grows as the phases that need it arrive. Nothing above core Vulkan 1.1 belongs here —
// that is what the frontend's physical device admits to (see the probe log) — and nothing above
// core 1.0 belongs here without a run-time check that the device supports it.
struct vk_funcs
{
	PFN_vkGetInstanceProcAddr                       get_instance_proc_addr = nullptr;
	PFN_vkGetDeviceProcAddr                         get_device_proc_addr = nullptr;

	// Global-level (instance == VK_NULL_HANDLE), and absent on a strictly 1.0 loader — hence
	// optional. The device's own apiVersion is what actually bounds us; this only says what the
	// loader in front of it will admit to.
	PFN_vkEnumerateInstanceVersion                  enumerate_instance_version = nullptr;

	// instance level
	PFN_vkGetPhysicalDeviceProperties               get_physical_device_properties = nullptr;
	PFN_vkGetPhysicalDeviceFeatures                 get_physical_device_features = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties         get_physical_device_memory_properties = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties    get_physical_device_queue_family_properties = nullptr;
	PFN_vkGetPhysicalDeviceFormatProperties         get_physical_device_format_properties = nullptr;
	PFN_vkEnumerateDeviceExtensionProperties        enumerate_device_extension_properties = nullptr;

	// device level — submission and synchronisation
	PFN_vkDeviceWaitIdle                            device_wait_idle = nullptr;
	PFN_vkQueueSubmit                               queue_submit = nullptr;
	PFN_vkCreateFence                               create_fence = nullptr;
	PFN_vkDestroyFence                              destroy_fence = nullptr;
	PFN_vkWaitForFences                             wait_for_fences = nullptr;
	PFN_vkResetFences                               reset_fences = nullptr;

	// device level — memory
	PFN_vkAllocateMemory                            allocate_memory = nullptr;
	PFN_vkFreeMemory                                free_memory = nullptr;
	PFN_vkMapMemory                                 map_memory = nullptr;
	PFN_vkUnmapMemory                               unmap_memory = nullptr;

	// device level — images
	PFN_vkCreateImage                               create_image = nullptr;
	PFN_vkDestroyImage                              destroy_image = nullptr;
	PFN_vkGetImageMemoryRequirements                get_image_memory_requirements = nullptr;
	PFN_vkBindImageMemory                           bind_image_memory = nullptr;
	PFN_vkCreateImageView                           create_image_view = nullptr;
	PFN_vkDestroyImageView                          destroy_image_view = nullptr;
	PFN_vkCreateSampler                             create_sampler = nullptr;
	PFN_vkDestroySampler                            destroy_sampler = nullptr;

	// device level — buffers (the staging upload)
	PFN_vkCreateBuffer                              create_buffer = nullptr;
	PFN_vkDestroyBuffer                             destroy_buffer = nullptr;
	PFN_vkGetBufferMemoryRequirements               get_buffer_memory_requirements = nullptr;
	PFN_vkBindBufferMemory                          bind_buffer_memory = nullptr;

	// device level — descriptors
	PFN_vkCreateDescriptorSetLayout                 create_descriptor_set_layout = nullptr;
	PFN_vkDestroyDescriptorSetLayout                destroy_descriptor_set_layout = nullptr;
	PFN_vkCreateDescriptorPool                      create_descriptor_pool = nullptr;
	PFN_vkDestroyDescriptorPool                     destroy_descriptor_pool = nullptr;
	PFN_vkAllocateDescriptorSets                    allocate_descriptor_sets = nullptr;
	PFN_vkUpdateDescriptorSets                      update_descriptor_sets = nullptr;

	// device level — the pipeline
	PFN_vkCreateShaderModule                        create_shader_module = nullptr;
	PFN_vkDestroyShaderModule                       destroy_shader_module = nullptr;
	PFN_vkCreateRenderPass                          create_render_pass = nullptr;
	PFN_vkDestroyRenderPass                         destroy_render_pass = nullptr;
	PFN_vkCreateFramebuffer                         create_framebuffer = nullptr;
	PFN_vkDestroyFramebuffer                        destroy_framebuffer = nullptr;
	PFN_vkCreatePipelineLayout                      create_pipeline_layout = nullptr;
	PFN_vkDestroyPipelineLayout                     destroy_pipeline_layout = nullptr;
	PFN_vkCreateGraphicsPipelines                   create_graphics_pipelines = nullptr;
	PFN_vkDestroyPipeline                           destroy_pipeline = nullptr;

	// device level — command recording
	PFN_vkCreateCommandPool                         create_command_pool = nullptr;
	PFN_vkDestroyCommandPool                        destroy_command_pool = nullptr;
	PFN_vkResetCommandPool                          reset_command_pool = nullptr;
	PFN_vkAllocateCommandBuffers                    allocate_command_buffers = nullptr;
	PFN_vkBeginCommandBuffer                        begin_command_buffer = nullptr;
	PFN_vkEndCommandBuffer                          end_command_buffer = nullptr;
	PFN_vkCmdPipelineBarrier                        cmd_pipeline_barrier = nullptr;
	PFN_vkCmdCopyBufferToImage                      cmd_copy_buffer_to_image = nullptr;
	// Only ever used by the M2VK_VK_DUMP diagnostic, but resolved unconditionally: an entry point
	// that is missing is worth knowing about at context_reset rather than the first time someone
	// reaches for the diagnostic.
	PFN_vkCmdCopyImageToBuffer                      cmd_copy_image_to_buffer = nullptr;
	PFN_vkCmdBeginRenderPass                        cmd_begin_render_pass = nullptr;
	PFN_vkCmdEndRenderPass                          cmd_end_render_pass = nullptr;
	PFN_vkCmdBindPipeline                           cmd_bind_pipeline = nullptr;
	PFN_vkCmdBindDescriptorSets                     cmd_bind_descriptor_sets = nullptr;
	PFN_vkCmdSetViewport                            cmd_set_viewport = nullptr;
	PFN_vkCmdSetScissor                             cmd_set_scissor = nullptr;
	PFN_vkCmdDraw                                   cmd_draw = nullptr;

	// device level — the polygon pass
	PFN_vkCmdBindVertexBuffers                      cmd_bind_vertex_buffers = nullptr;
	PFN_vkCmdBindIndexBuffer                        cmd_bind_index_buffer = nullptr;
	PFN_vkCmdDrawIndexed                            cmd_draw_indexed = nullptr;
	PFN_vkCmdPushConstants                          cmd_push_constants = nullptr;
};

// Fills the table. Returns false if a required entry point is missing, having logged which one:
// that would mean the frontend handed over a loader that cannot answer for its own instance or its
// own device, and the only safe response is to leave Vulkan alone.
bool load_funcs(vk_funcs &fns, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
		VkInstance instance, VkDevice device);

// Names a VkResult for a log line. Unknown codes come back as their number.
char const *vk_result_name(VkResult result);


//============================================================
//  allocation
//============================================================

// The index of a memory type from `type_bits` that has all of `want`, or false if the device offers
// none. Both the ring and the geometry buffers need this and neither should have its own copy of
// it: it is pure plumbing, and two copies would drift the first time one of them learned something.
//
// Callers ask for preferred properties first and give up on them if refused. This device's memory
// type 1 is device-local *and* host-visible (unified memory, see devnotes/vulkan-target.md), which
// is why the geometry buffers are written straight into rather than staged — but that is an Apple
// luxury and not something to depend on.
bool find_memory_type(vk_funcs const &fns, VkPhysicalDevice gpu, uint32_t type_bits,
		VkMemoryPropertyFlags want, uint32_t &out);

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_FUNCS_H
