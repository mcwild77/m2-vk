// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the image ring, and the frame's submit.

    See vk_present.h for the arrangement this implements. What the frame actually draws is, at this
    step, a flat colour: the point of the step is the ring, the sync-index bookkeeping and the
    handover, and a clear is the least amount of drawing that proves all three. The staging upload
    and the fullscreen triangle replace the clear in the next step, and nothing around them changes.

*********************************************************************************************************************************/

#include "renderer_vk/vk_present.h"

#include "renderer_vk/vk_context.h"

#include <cstdint>
#include <vector>


namespace m2vk {

namespace {

//============================================================
//  constants
//============================================================

// MAME's bitmap_rgb32 pixel is 0xAARRGGBB in a native uint32_t, which on little-endian is B,G,R,A
// in memory — so this is the format the next step's upload is a straight memcpy into. The probe
// confirmed it on this device: sampled, colour attachment, blend, blit and transfer both ways.
constexpr VkFormat RING_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;

// A fence that has not signalled in this long means the GPU is wedged or the frontend has taken the
// queue away. Waiting forever would hang the frontend's thread with no diagnosis; this gives up,
// says so, and dupes the frame.
constexpr uint64_t FENCE_TIMEOUT_NS = 2000000000ull;

// Ceiling on the ring, purely as a sanity bound on a mask we did not compute. RetroArch reports 3.
constexpr uint32_t MAX_RING_SLOTS = 8;


//============================================================
//  state
//============================================================

// One of these per sync index. Nothing in it is ever shared with another slot: that is the whole
// point of indexing by get_sync_index().
struct frame_slot
{
	VkImage         image = VK_NULL_HANDLE;
	VkDeviceMemory  memory = VK_NULL_HANDLE;
	VkImageView     view = VK_NULL_HANDLE;
	VkCommandPool   pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence         fence = VK_NULL_HANDLE;

	// Handed to set_image by address and read by the frontend until video_refresh has returned —
	// and possibly again, if a later frame is duped. Hence a member rather than a local.
	retro_vulkan_image handover{};
};

std::vector<frame_slot> s_slots;

// Captured when the ring is built, so that teardown does not depend on the context's state having
// survived — and so that the funcs table is one pointer chase away in the per-frame path.
const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

unsigned s_width = 0;
unsigned s_height = 0;
uint32_t s_mask = 0;

uint64_t s_frames = 0;

// Errors in the per-frame path would otherwise repeat 57 times a second.
bool s_reported_frame_error = false;


//============================================================
//  helpers
//============================================================

bool check(VkResult result, char const *what)
{
	if (result == VK_SUCCESS)
		return true;
	vk_log(RETRO_LOG_ERROR, "%s failed: %s\n", what, vk_result_name(result));
	return false;
}

// Slots needed to index the mask: bit N set means get_sync_index() may return N, so it is the
// highest set bit that decides the size, not the number of bits set.
uint32_t slots_for_mask(uint32_t mask)
{
	uint32_t count = 0;
	for (uint32_t i = 0; i < 32; i++)
	{
		if ((mask & (uint32_t(1) << i)) != 0)
			count = i + 1;
	}
	return count;
}

// A memory type from the requirement's mask that has all of `want`. Preferred properties are asked
// for first and then given up on: the probe found this device's type 1 is device-local *and* host
// visible (unified memory), but that is an Apple luxury and not something to depend on.
bool find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want, uint32_t &out)
{
	VkPhysicalDeviceMemoryProperties mem{};
	s_fns.get_physical_device_memory_properties(s_iface->gpu, &mem);

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


//============================================================
//  the ring
//============================================================

void destroy_ring()
{
	if (s_slots.empty())
	{
		s_device = VK_NULL_HANDLE;
		s_iface = nullptr;
		return;
	}

	// The frontend may still be sampling the images. vkDeviceWaitIdle is a wait on every queue, and
	// the queue is shared, so it is bracketed exactly as a submit would be.
	if ((s_iface != nullptr) && (s_fns.device_wait_idle != nullptr))
	{
		s_iface->lock_queue(s_iface->handle);
		s_fns.device_wait_idle(s_device);
		s_iface->unlock_queue(s_iface->handle);
	}
	else
	{
		// Only reachable if the device went away before we were told to tear down, in which case
		// every handle below is already dead and touching one would be worse than leaking it.
		vk_log(RETRO_LOG_WARN, "the ring outlived its device; %u slots abandoned\n", unsigned(s_slots.size()));
		s_slots.clear();
		s_device = VK_NULL_HANDLE;
		s_iface = nullptr;
		return;
	}

	for (frame_slot &slot : s_slots)
	{
		// The command buffer is freed with its pool; the memory is freed after the image that is
		// bound to it, which is the order Vulkan requires.
		if (slot.pool != VK_NULL_HANDLE)
			s_fns.destroy_command_pool(s_device, slot.pool, nullptr);
		if (slot.fence != VK_NULL_HANDLE)
			s_fns.destroy_fence(s_device, slot.fence, nullptr);
		if (slot.view != VK_NULL_HANDLE)
			s_fns.destroy_image_view(s_device, slot.view, nullptr);
		if (slot.image != VK_NULL_HANDLE)
			s_fns.destroy_image(s_device, slot.image, nullptr);
		if (slot.memory != VK_NULL_HANDLE)
			s_fns.free_memory(s_device, slot.memory, nullptr);
	}

	vk_log(RETRO_LOG_INFO, "ring of %u destroyed\n", unsigned(s_slots.size()));

	s_slots.clear();
	s_device = VK_NULL_HANDLE;
	s_iface = nullptr;
	s_width = 0;
	s_height = 0;
	s_mask = 0;
}

bool build_slot(frame_slot &slot, unsigned width, unsigned height, uint32_t queue_family)
{
	// TRANSFER_SRC and SAMPLED are what the interface demands of anything passed to set_image;
	// TRANSFER_DST is for this step's clear and the next step's upload; COLOR_ATTACHMENT is for the
	// render pass from step 4 on. MUTABLE_FORMAT is not optional — the interface requires it of
	// 8-bit formats so that the frontend can reinterpret sRGB as it sees fit.
	VkImageCreateInfo image_info{};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = RING_FORMAT;
	image_info.extent = { width, height, 1 };
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
			| VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (!check(s_fns.create_image(s_device, &image_info, nullptr, &slot.image), "vkCreateImage"))
		return false;

	VkMemoryRequirements reqs{};
	s_fns.get_image_memory_requirements(s_device, slot.image, &reqs);

	uint32_t type_index = 0;
	if (!find_memory_type(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index)
			&& !find_memory_type(reqs.memoryTypeBits, 0, type_index))
	{
		vk_log(RETRO_LOG_ERROR, "no memory type accepts a %ux%u ring image\n", width, height);
		return false;
	}

	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = type_index;
	if (!check(s_fns.allocate_memory(s_device, &alloc, nullptr, &slot.memory), "vkAllocateMemory"))
		return false;
	if (!check(s_fns.bind_image_memory(s_device, slot.image, slot.memory, 0), "vkBindImageMemory"))
		return false;

	// The create_info is kept because the frontend is entitled to recreate the view itself — that is
	// how it reinterprets the format — so it must be the struct this view was actually made from.
	VkImageViewCreateInfo &view_info = slot.handover.create_info;
	view_info = VkImageViewCreateInfo{};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = slot.image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = RING_FORMAT;
	view_info.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
	view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	if (!check(s_fns.create_image_view(s_device, &view_info, nullptr, &slot.view), "vkCreateImageView"))
		return false;

	slot.handover.image_view = slot.view;
	slot.handover.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// One pool per slot, reset wholesale each time the slot comes round. That is cheaper than
	// resetting individual buffers and it means the slot's command memory is recycled rather than
	// growing, which a single shared pool would not give us.
	VkCommandPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool_info.queueFamilyIndex = queue_family;
	if (!check(s_fns.create_command_pool(s_device, &pool_info, nullptr, &slot.pool), "vkCreateCommandPool"))
		return false;

	VkCommandBufferAllocateInfo cmd_info{};
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_info.commandPool = slot.pool;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_info.commandBufferCount = 1;
	if (!check(s_fns.allocate_command_buffers(s_device, &cmd_info, &slot.cmd), "vkAllocateCommandBuffers"))
		return false;

	// Created signalled so the first frame through each slot waits on nothing and the per-frame path
	// needs no "has this been submitted yet" special case.
	VkFenceCreateInfo fence_info{};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (!check(s_fns.create_fence(s_device, &fence_info, nullptr, &slot.fence), "vkCreateFence"))
		return false;

	return true;
}

bool build_ring(const retro_hw_render_interface_vulkan &iface, unsigned width, unsigned height, uint32_t mask)
{
	destroy_ring();

	const uint32_t count = slots_for_mask(mask);
	if ((count == 0) || (count > MAX_RING_SLOTS))
	{
		vk_log(RETRO_LOG_ERROR, "the frontend's sync index mask 0x%x asks for %u slots\n", unsigned(mask), unsigned(count));
		return false;
	}

	s_iface = &iface;
	s_fns = context_funcs();
	s_device = iface.device;
	s_width = width;
	s_height = height;
	s_mask = mask;

	s_slots.resize(count);
	for (frame_slot &slot : s_slots)
	{
		if (!build_slot(slot, width, height, iface.queue_index))
		{
			destroy_ring();
			return false;
		}
	}

	vk_log(RETRO_LOG_INFO, "ring of %u %ux%u B8G8R8A8_UNORM images, sync index mask 0x%x, queue family %u\n",
			unsigned(count), width, height, unsigned(mask), iface.queue_index);
	return true;
}


//============================================================
//  the frame
//============================================================

// Flat, but deliberately not static. A single unchanging colour cannot tell "the ring is advancing
// and the frontend is presenting each slot" apart from "the frontend is showing one stale image
// forever", which is exactly the failure this step exists to rule out. So the brightness walks a
// slow triangle — about two seconds a cycle at Model 2's 57.5 Hz — and a frozen picture is visible
// as such at a glance. Orange, because a red/blue swizzle would read as blue and be unmissable.
VkClearColorValue clear_colour()
{
	constexpr uint64_t PERIOD = 120;
	const uint64_t phase = s_frames % PERIOD;
	const uint64_t up = (phase < (PERIOD / 2)) ? phase : (PERIOD - phase);
	const float t = float(up) / float(PERIOD / 2);
	const float scale = 0.35f + (0.65f * t);

	VkClearColorValue value{};
	value.float32[0] = 1.00f * scale;
	value.float32[1] = 0.45f * scale;
	value.float32[2] = 0.05f * scale;
	value.float32[3] = 1.0f;
	return value;
}

// UNDEFINED as the old layout every time, not the layout we left it in. The frontend is explicitly
// allowed to transition the image while it holds it, so its current layout is not ours to know —
// and discarding the contents costs nothing when the first thing the frame does is overwrite every
// pixel of it.
void barrier(VkCommandBuffer cmd, VkImage image,
		VkImageLayout old_layout, VkImageLayout new_layout,
		VkAccessFlags src_access, VkAccessFlags dst_access,
		VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b{};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = old_layout;
	b.newLayout = new_layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	s_fns.cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool record_and_submit(frame_slot &slot)
{
	if (!check(s_fns.reset_command_pool(s_device, slot.pool, 0), "vkResetCommandPool"))
		return false;

	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (!check(s_fns.begin_command_buffer(slot.cmd, &begin), "vkBeginCommandBuffer"))
		return false;

	barrier(slot.cmd, slot.image,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	const VkClearColorValue colour = clear_colour();
	const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	s_fns.cmd_clear_color_image(slot.cmd, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);

	// Into the layout named in handover.image_layout, and readable by the frontend's fragment shader.
	// This barrier is what makes semaphores unnecessary in set_image below.
	barrier(slot.cmd, slot.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	if (!check(s_fns.end_command_buffer(slot.cmd), "vkEndCommandBuffer"))
		return false;

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &slot.cmd;

	if (!check(s_fns.reset_fences(s_device, 1, &slot.fence), "vkResetFences"))
		return false;

	// The frontend submits on this same queue from its own thread.
	s_iface->lock_queue(s_iface->handle);
	const VkResult result = s_fns.queue_submit(s_iface->queue, 1, &submit, slot.fence);
	s_iface->unlock_queue(s_iface->handle);

	return check(result, "vkQueueSubmit");
}

} // anonymous namespace


//============================================================
//  m2vk — the public surface
//============================================================

bool present_frame(unsigned width, unsigned height)
{
	const retro_hw_render_interface_vulkan *const iface = context_interface();
	if (iface == nullptr)
		return false;
	if ((width == 0) || (height == 0))
		return false;

	// Re-read every frame. The mask is documented to change — a fullscreen toggle changes the
	// swapchain length — and the spec's promise is that when it does, the device is idle.
	const uint32_t mask = iface->get_sync_index_mask(iface->handle);

	if (s_slots.empty() || (iface != s_iface) || (width != s_width) || (height != s_height) || (mask != s_mask))
	{
		if (!s_slots.empty())
		{
			vk_log(RETRO_LOG_INFO, "rebuilding the ring: %ux%u mask 0x%x -> %ux%u mask 0x%x\n",
					s_width, s_height, unsigned(s_mask), width, height, unsigned(mask));
		}
		if (!build_ring(*iface, width, height, mask))
			return false;
		s_reported_frame_error = false;
	}

	const uint32_t index = iface->get_sync_index(iface->handle);
	if (index >= s_slots.size())
	{
		if (!s_reported_frame_error)
		{
			vk_log(RETRO_LOG_ERROR, "the frontend returned sync index %u for a ring of %u (mask 0x%x)\n",
					unsigned(index), unsigned(s_slots.size()), unsigned(s_mask));
			s_reported_frame_error = true;
		}
		return false;
	}

	// The frontend is done with this slot's image, and has released queue-family ownership of it
	// back to us. Only after this may the slot be touched at all.
	iface->wait_sync_index(iface->handle);

	frame_slot &slot = s_slots[index];

	// And our own previous submit into this slot has retired, so its command buffer can be reset.
	const VkResult waited = s_fns.wait_for_fences(s_device, 1, &slot.fence, VK_TRUE, FENCE_TIMEOUT_NS);
	if (waited != VK_SUCCESS)
	{
		if (!s_reported_frame_error)
		{
			vk_log(RETRO_LOG_ERROR, "slot %u never retired: %s\n", unsigned(index), vk_result_name(waited));
			s_reported_frame_error = true;
		}
		return false;
	}

	if (!record_and_submit(slot))
	{
		if (!s_reported_frame_error)
		{
			vk_log(RETRO_LOG_ERROR, "slot %u could not be submitted; the picture stops here\n", unsigned(index));
			s_reported_frame_error = true;
		}
		return false;
	}

	// No semaphores: the layout transition at the end of the command buffer is the synchronisation,
	// which the interface documents as the preferred of the two. VK_QUEUE_FAMILY_IGNORED because we
	// submit on the frontend's own queue family, so there is no ownership to transfer.
	iface->set_image(iface->handle, &slot.handover, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);

	s_frames++;
	s_reported_frame_error = false;
	return true;
}

void present_shutdown()
{
	destroy_ring();
	s_frames = 0;
	s_reported_frame_error = false;
}

} // namespace m2vk
