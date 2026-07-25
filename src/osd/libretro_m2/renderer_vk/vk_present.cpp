// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the image ring, the staging upload, and the frame's submit.

    See vk_present.h for the ring's arrangement, which is unchanged. What the frame draws is now the
    picture: MAME's finished software frame is memcpy'd into a host-visible staging buffer, copied
    into an optimal-tiled texture, and sampled by a fullscreen triangle into the ring image the
    frontend presents. No polygons — the tapped stream still goes to the diagnostic sink and nowhere
    else. This is the passthrough that P3 replaces from the inside.

    The exit criterion for the phase is a bit-exact match against the software renderer, so the
    chain is arranged so that there is nowhere for a bit to go missing:

      * MAME's bitmap_rgb32 pixel is 0xAARRGGBB in a native uint32_t, which on little-endian is
        B,G,R,A in memory. That is VK_FORMAT_B8G8R8A8_UNORM exactly, so the upload is a memcpy and
        the sampler needs no swizzle. UNORM, not SRGB: no transfer function anywhere.
      * Source and destination are the same size and the sampler is NEAREST, so pixel x samples at
        (x + 0.5) / w, which is texel x. One texel in, one pixel out.
      * No blending, no multisample, no depth. The fragment shader's output is the attachment's
        value.

    Two things below are worth knowing before editing:

      * Everything shared between slots — render pass, sampler, descriptor layout and pool, pipeline
        layout, pipeline — is built and destroyed with the ring rather than with the context. It all
        depends on the ring's format and the descriptor pool on its size, and tying the lifetimes
        together means there is one build path and one teardown path instead of two that have to
        agree.
      * The staging buffer is mapped once, at build, and stays mapped. Mapping is not free and the
        alternative is a map/unmap pair 57 times a second for no benefit; a persistently mapped
        HOST_COHERENT allocation is the ordinary shape for this.

*********************************************************************************************************************************/

#include "renderer_vk/vk_present.h"

#include "renderer_vk/vk_context.h"

#include "m2vk_frame.h"

#include "renderer_vk/shaders/fullscreen_vert_spv.h"
#include "renderer_vk/shaders/overlay_frag_spv.h"
#include "renderer_vk/shaders/passthrough_frag_spv.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


namespace m2vk {

namespace {

//============================================================
//  constants
//============================================================

// MAME's bitmap_rgb32 pixel is 0xAARRGGBB in a native uint32_t, which on little-endian is B,G,R,A
// in memory — so the upload is a straight memcpy. The probe confirmed it on this device: sampled,
// colour attachment, blend, blit and transfer both ways.
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

// One 2D layer's worth of upload: host-visible staging, an optimal-tiled texture, and the descriptor
// set naming it. Two of these per slot — the background and the foreground — because the frame is a
// sandwich and MAME draws both slices (m2vk_frame.h).
struct layer_tex
{
	VkBuffer        staging = VK_NULL_HANDLE;
	VkDeviceMemory  staging_memory = VK_NULL_HANDLE;
	void           *staging_mapped = nullptr;
	VkImage         texture = VK_NULL_HANDLE;
	VkDeviceMemory  texture_memory = VK_NULL_HANDLE;
	VkImageView     texture_view = VK_NULL_HANDLE;
	VkDescriptorSet descriptor = VK_NULL_HANDLE;
};

// One of these per sync index. Nothing in it is ever shared with another slot: that is the whole
// point of indexing by get_sync_index(). The textures are per-slot for the same reason the image is —
// they are written by this frame's copy and read by this frame's draw, and a shared one would be a
// hazard between frames still in flight.
struct frame_slot
{
	// the image the frontend samples
	VkImage         image = VK_NULL_HANDLE;
	VkDeviceMemory  memory = VK_NULL_HANDLE;
	VkImageView     view = VK_NULL_HANDLE;
	VkFramebuffer   framebuffer = VK_NULL_HANDLE;

	layer_tex       layers[m2vk::LAYER_COUNT];

	VkCommandPool   pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence         fence = VK_NULL_HANDLE;

	// Handed to set_image by address and read by the frontend until video_refresh has returned —
	// and possibly again, if a later frame is duped. Hence a member rather than a local.
	retro_vulkan_image handover{};
};

// A fixed array rather than a vector, and the addresses are the reason. set_image takes the
// handover by pointer and the frontend may read it again on any later duped frame, so a slot's
// address has to outlive not just the frame but the ring itself: a rebuild frees the old ring
// before the new one has been handed over, and a vector would have moved or freed the storage the
// frontend is still holding a pointer into. Here a torn-down slot is zeroed in place instead, so
// the worst a stale pointer can find is a null handle rather than freed memory.
std::array<frame_slot, MAX_RING_SLOTS> s_slots{};
uint32_t s_slot_count = 0;

// Shared by every slot, built and destroyed with the ring.
VkRenderPass          s_render_pass = VK_NULL_HANDLE;
VkSampler             s_sampler = VK_NULL_HANDLE;
VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout      s_pipeline_layout = VK_NULL_HANDLE;
VkPipeline            s_pipeline = VK_NULL_HANDLE;        // opaque: the under layer
VkPipeline            s_pipeline_over = VK_NULL_HANDLE;   // discards pixel 0: the over layer

// Captured when the ring is built, so that teardown does not depend on the context's state having
// survived — and so that the funcs table is one pointer chase away in the per-frame path.
const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

unsigned s_width = 0;
unsigned s_height = 0;
uint32_t s_mask = 0;

// Frames presented since the content was loaded. This deliberately survives a context loss: a
// fixture is identified by (rom, frame), and if this restarted at every context_reset then a
// fullscreen toggle at frame 1400 would silently move the frame the read-back is armed for. The
// per-context count is kept alongside it only so that a resumed picture can say so.
uint64_t s_frames = 0;
uint64_t s_context_frames = 0;

// Errors in the per-frame path would otherwise repeat 57 times a second.
bool s_reported_frame_error = false;


//============================================================
//  the read-back diagnostic
//============================================================

// M2VK_VK_DUMP=<prefix> writes two PPMs on one nominated frame: <prefix>-src.ppm, the software
// picture as it went into the staging buffer, and <prefix>-vk.ppm, the ring image read straight
// back off the GPU after the draw. If they differ, the renderer reinterpreted something.
//
// This exists because there is no other way to see the core's own output. A frontend screenshot is
// not it — step 3 measured RetroArch presenting (252, 131, 43) for a clear that wrote
// (252, 113, 13), which is a colour-space conversion on presentation — and retrohost has no Vulkan
// until step 7. Reading the image back inside the core sidesteps both.
//
// Cost when unset: one getenv at build time and one integer compare per frame.
std::string s_dump_prefix;
uint64_t    s_dump_frame = 0;
bool        s_dump_done = false;

VkBuffer       s_dump_buffer = VK_NULL_HANDLE;
VkDeviceMemory s_dump_memory = VK_NULL_HANDLE;
void          *s_dump_mapped = nullptr;


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

bool allocate_and_bind_image(VkImage image, VkDeviceMemory &out)
{
	VkMemoryRequirements reqs{};
	s_fns.get_image_memory_requirements(s_device, image, &reqs);

	uint32_t type_index = 0;
	if (!find_memory_type(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index)
			&& !find_memory_type(reqs.memoryTypeBits, 0, type_index))
	{
		vk_log(RETRO_LOG_ERROR, "no memory type accepts this image\n");
		return false;
	}

	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = type_index;
	if (!check(s_fns.allocate_memory(s_device, &alloc, nullptr, &out), "vkAllocateMemory"))
		return false;

	return check(s_fns.bind_image_memory(s_device, image, out, 0), "vkBindImageMemory");
}


//============================================================
//  the read-back diagnostic
//============================================================

void dump_destroy()
{
	if (s_dump_mapped != nullptr)
		s_fns.unmap_memory(s_device, s_dump_memory);
	if (s_dump_buffer != VK_NULL_HANDLE)
		s_fns.destroy_buffer(s_device, s_dump_buffer, nullptr);
	if (s_dump_memory != VK_NULL_HANDLE)
		s_fns.free_memory(s_device, s_dump_memory, nullptr);

	s_dump_mapped = nullptr;
	s_dump_buffer = VK_NULL_HANDLE;
	s_dump_memory = VK_NULL_HANDLE;
}

// One buffer, not one per slot: the dump happens on a single frame and stalls on that frame's fence
// before reading, so there is nothing to overlap with.
bool dump_build_buffer(unsigned width, unsigned height)
{
	VkBufferCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = VkDeviceSize(width) * VkDeviceSize(height) * 4;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!check(s_fns.create_buffer(s_device, &info, nullptr, &s_dump_buffer), "vkCreateBuffer (dump)"))
		return false;

	VkMemoryRequirements reqs{};
	s_fns.get_buffer_memory_requirements(s_device, s_dump_buffer, &reqs);

	uint32_t type_index = 0;
	if (!find_memory_type(reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type_index))
		return false;

	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = type_index;
	if (!check(s_fns.allocate_memory(s_device, &alloc, nullptr, &s_dump_memory), "vkAllocateMemory (dump)"))
		return false;
	if (!check(s_fns.bind_buffer_memory(s_device, s_dump_buffer, s_dump_memory, 0), "vkBindBufferMemory (dump)"))
		return false;

	return check(s_fns.map_memory(s_device, s_dump_memory, 0, VK_WHOLE_SIZE, 0, &s_dump_mapped), "vkMapMemory (dump)");
}

// P6 with the alpha channel dropped, which is also what retrohost writes — so the two are
// comparable with cmp, and the fragment shader's synthetic alpha cannot flatter the result.
bool write_ppm(std::string const &path, void const *bgra, unsigned width, unsigned height)
{
	std::FILE *const f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
	{
		vk_log(RETRO_LOG_ERROR, "could not open '%s' for writing\n", path.c_str());
		return false;
	}

	std::fprintf(f, "P6\n%u %u\n255\n", width, height);

	std::vector<uint8_t> row(size_t(width) * 3);
	auto const *const src = static_cast<uint8_t const *>(bgra);
	for (unsigned y = 0; y < height; y++)
	{
		uint8_t const *p = src + (size_t(y) * width * 4);
		for (unsigned x = 0; x < width; x++, p += 4)
		{
			row[(size_t(x) * 3) + 0] = p[2];    // B,G,R,A in memory -> R,G,B on disk
			row[(size_t(x) * 3) + 1] = p[1];
			row[(size_t(x) * 3) + 2] = p[0];
		}
		std::fwrite(row.data(), 1, row.size(), f);
	}

	std::fclose(f);
	vk_log(RETRO_LOG_INFO, "wrote %s (%ux%u)\n", path.c_str(), width, height);
	return true;
}


//============================================================
//  the objects every slot shares
//============================================================

// One colour attachment, drawn once, handed straight over. Two things are doing real work here:
//
//   * finalLayout is SHADER_READ_ONLY_OPTIMAL, which is the layout named in handover.image_layout.
//     The render pass performs that transition, so it — with the subpass dependency below — is what
//     makes semaphores unnecessary at set_image, exactly as the clear's closing barrier was.
//   * initialLayout is UNDEFINED every frame, not the layout we left the image in. The frontend is
//     explicitly allowed to transition an image it holds, so its current layout is not ours to know,
//     and discarding the contents costs nothing when the frame overwrites every pixel.
//
// loadOp is CLEAR rather than DONT_CARE even though the triangle covers the whole attachment. It is
// a diagnostic, not a correctness measure: if the draw ever fails to happen the result is black,
// which reads as a failure, rather than whatever was last in that memory, which reads as a picture.
bool build_render_pass()
{
	VkAttachmentDescription colour{};
	colour.format = RING_FORMAT;
	colour.samples = VK_SAMPLE_COUNT_1_BIT;
	colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colour.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkAttachmentReference colour_ref{};
	colour_ref.attachment = 0;
	colour_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colour_ref;

	// Both directions stated rather than left to the implicit defaults, which are TOP_OF_PIPE on the
	// way in and BOTTOM_OF_PIPE with no access mask on the way out — the latter being no dependency
	// at all as far as the frontend's fragment shader read is concerned.
	VkSubpassDependency deps[2]{};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].srcAccessMask = 0;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	VkRenderPassCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1;
	info.pAttachments = &colour;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = deps;

	return check(s_fns.create_render_pass(s_device, &info, nullptr, &s_render_pass), "vkCreateRenderPass");
}

// NEAREST and CLAMP_TO_EDGE. Filtering is P5's business; here one output pixel is one source pixel
// and anything else would cost the bit-exactness this phase exists to establish.
bool build_sampler()
{
	VkSamplerCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = VK_FILTER_NEAREST;
	info.minFilter = VK_FILTER_NEAREST;
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.mipLodBias = 0.0f;
	info.anisotropyEnable = VK_FALSE;
	info.maxAnisotropy = 1.0f;
	info.compareEnable = VK_FALSE;
	info.compareOp = VK_COMPARE_OP_ALWAYS;
	info.minLod = 0.0f;
	info.maxLod = 0.0f;
	info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	info.unnormalizedCoordinates = VK_FALSE;

	return check(s_fns.create_sampler(s_device, &info, nullptr, &s_sampler), "vkCreateSampler");
}

// One combined image sampler, fragment stage, one set per slot. The sampler is immutable — it never
// varies and baking it into the layout is one fewer thing for the per-frame path to get wrong.
bool build_descriptors(uint32_t slot_count)
{
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	binding.pImmutableSamplers = &s_sampler;

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &layout_info, nullptr, &s_set_layout), "vkCreateDescriptorSetLayout"))
		return false;

	// One set per layer per slot: the two layers are sampled by two draws in the same command buffer,
	// so they cannot share.
	const uint32_t sets = slot_count * uint32_t(m2vk::LAYER_COUNT);

	VkDescriptorPoolSize size{};
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = sets;

	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = sets;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	// No FREE_DESCRIPTOR_SET_BIT: the sets are allocated once and live exactly as long as the pool,
	// which is destroyed wholesale with the ring.
	return check(s_fns.create_descriptor_pool(s_device, &pool_info, nullptr, &s_descriptor_pool), "vkCreateDescriptorPool");
}

bool build_pipeline()
{
	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &s_set_layout;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout), "vkCreatePipelineLayout"))
		return false;

	// The modules exist only for the duration of vkCreateGraphicsPipelines; the pipeline does not
	// reference them afterwards.
	VkShaderModule vert = VK_NULL_HANDLE;
	VkShaderModule frag = VK_NULL_HANDLE;
	VkShaderModule frag_over = VK_NULL_HANDLE;
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &out)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule");
	};

	bool ok = make_module(FULLSCREEN_VERT_SPV, sizeof(FULLSCREEN_VERT_SPV), vert)
			&& make_module(PASSTHROUGH_FRAG_SPV, sizeof(PASSTHROUGH_FRAG_SPV), frag)
			&& make_module(OVERLAY_FRAG_SPV, sizeof(OVERLAY_FRAG_SPV), frag_over);

	if (ok)
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vert;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = frag;
		stages[1].pName = "main";

		// Empty, and that is the point: the vertex shader builds its three positions from
		// gl_VertexIndex, so there is no vertex buffer, no index buffer and no binding to describe.
		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		// Dynamic, so the pipeline says nothing about geometry and would survive a resize on its own.
		// It does not have to today — the whole ring is rebuilt when the geometry changes — but the
		// alternative is baking 496x384 into a pipeline, which is a trap for whoever adds internal-res
		// scaling in P5.
		VkPipelineViewportStateCreateInfo viewport_state{};
		viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_state.viewportCount = 1;
		viewport_state.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		// No culling: one triangle whose winding is nobody's business but this file's.
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// Blending off: the shader's output is the attachment's value, with nothing in between to
		// round it. The whole point of the phase.
		VkPipelineColorBlendAttachmentState blend_attachment{};
		blend_attachment.blendEnable = VK_FALSE;
		blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
				| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendStateCreateInfo blend{};
		blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blend.attachmentCount = 1;
		blend.pAttachments = &blend_attachment;

		const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{};
		dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamic_states;

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertex_input;
		info.pInputAssemblyState = &assembly;
		info.pViewportState = &viewport_state;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		// pDepthStencilState stays null: the render pass has no depth attachment. P3 adds one.
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = s_pipeline_layout;
		info.renderPass = s_render_pass;
		info.subpass = 0;

		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline),
				"vkCreateGraphicsPipelines");

		// The overlay pipeline differs in exactly one thing — the fragment shader, which discards the
		// transparent pen — so it is built from the same structs rather than from a copy of them. Two
		// near-identical 60-line pipeline descriptions that have to stay in step is how the second one
		// ends up subtly different.
		if (ok)
		{
			stages[1].module = frag_over;
			ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline_over),
					"vkCreateGraphicsPipelines (overlay)");
		}
	}

	if (frag_over != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag_over, nullptr);
	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}


//============================================================
//  the ring
//============================================================

void destroy_shared()
{
	if (s_pipeline_over != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline_over, nullptr);
	if (s_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline, nullptr);
	if (s_pipeline_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_pipeline_layout, nullptr);
	// The sets are freed with the pool.
	if (s_descriptor_pool != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_pool(s_device, s_descriptor_pool, nullptr);
	if (s_set_layout != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_set_layout(s_device, s_set_layout, nullptr);
	if (s_sampler != VK_NULL_HANDLE)
		s_fns.destroy_sampler(s_device, s_sampler, nullptr);
	if (s_render_pass != VK_NULL_HANDLE)
		s_fns.destroy_render_pass(s_device, s_render_pass, nullptr);

	s_pipeline_over = VK_NULL_HANDLE;
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_sampler = VK_NULL_HANDLE;
	s_render_pass = VK_NULL_HANDLE;
}

// Drops every handle without calling Vulkan. Either the objects have just been destroyed, or the
// device they belonged to is already gone and touching one would be worse than leaking it.
void forget_ring()
{
	// Zeroed rather than left stale: the frontend may still hold a pointer to a slot's handover.
	for (frame_slot &slot : s_slots)
		slot = frame_slot{};
	s_slot_count = 0;

	s_dump_mapped = nullptr;
	s_dump_buffer = VK_NULL_HANDLE;
	s_dump_memory = VK_NULL_HANDLE;
	s_pipeline_over = VK_NULL_HANDLE;
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_sampler = VK_NULL_HANDLE;
	s_render_pass = VK_NULL_HANDLE;
	s_device = VK_NULL_HANDLE;
	s_iface = nullptr;
	s_width = 0;
	s_height = 0;
	s_mask = 0;
}

void destroy_ring()
{
	if ((s_slot_count == 0) && (s_render_pass == VK_NULL_HANDLE))
	{
		forget_ring();
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
		vk_log(RETRO_LOG_WARN, "the ring outlived its device; %u slots abandoned\n", unsigned(s_slot_count));
		forget_ring();
		return;
	}

	const unsigned count = s_slot_count;

	for (uint32_t i = 0; i < s_slot_count; i++)
	{
		frame_slot &slot = s_slots[i];

		// The command buffer is freed with its pool, and each memory allocation is freed after the
		// resource bound to it, which is the order Vulkan requires.
		if (slot.pool != VK_NULL_HANDLE)
			s_fns.destroy_command_pool(s_device, slot.pool, nullptr);
		if (slot.fence != VK_NULL_HANDLE)
			s_fns.destroy_fence(s_device, slot.fence, nullptr);
		if (slot.framebuffer != VK_NULL_HANDLE)
			s_fns.destroy_framebuffer(s_device, slot.framebuffer, nullptr);
		if (slot.view != VK_NULL_HANDLE)
			s_fns.destroy_image_view(s_device, slot.view, nullptr);
		if (slot.image != VK_NULL_HANDLE)
			s_fns.destroy_image(s_device, slot.image, nullptr);
		if (slot.memory != VK_NULL_HANDLE)
			s_fns.free_memory(s_device, slot.memory, nullptr);

		for (layer_tex &l : slot.layers)
		{
			if (l.texture_view != VK_NULL_HANDLE)
				s_fns.destroy_image_view(s_device, l.texture_view, nullptr);
			if (l.texture != VK_NULL_HANDLE)
				s_fns.destroy_image(s_device, l.texture, nullptr);
			if (l.texture_memory != VK_NULL_HANDLE)
				s_fns.free_memory(s_device, l.texture_memory, nullptr);

			// Unmapping is not strictly required before freeing, but leaving a mapping live across a
			// vkFreeMemory is the sort of thing a validation layer is right to complain about.
			if (l.staging_mapped != nullptr)
				s_fns.unmap_memory(s_device, l.staging_memory);
			if (l.staging != VK_NULL_HANDLE)
				s_fns.destroy_buffer(s_device, l.staging, nullptr);
			if (l.staging_memory != VK_NULL_HANDLE)
				s_fns.free_memory(s_device, l.staging_memory, nullptr);
		}
	}

	dump_destroy();
	destroy_shared();

	// The frame counts are the point of this line as much as the ring is: they are what says whether
	// a context loss cost the run anything, and they are the only place the two counters are seen
	// together.
	vk_log(RETRO_LOG_INFO, "ring of %u destroyed after %llu frames in this context (%llu since load)\n",
			count, (unsigned long long)s_context_frames, (unsigned long long)s_frames);

	forget_ring();
}

bool build_slot(frame_slot &slot, unsigned width, unsigned height, uint32_t queue_family)
{
	// TRANSFER_SRC and SAMPLED are what the interface demands of anything passed to set_image —
	// TRANSFER_SRC is also how retrohost-vk will read the image back for the A/B comparison at step
	// 7 — and COLOR_ATTACHMENT is what the render pass draws into. MUTABLE_FORMAT is not optional:
	// the interface requires it of 8-bit formats so that the frontend can reinterpret sRGB as it
	// sees fit.
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
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (!check(s_fns.create_image(s_device, &image_info, nullptr, &slot.image), "vkCreateImage (ring)"))
		return false;
	if (!allocate_and_bind_image(slot.image, slot.memory))
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

	if (!check(s_fns.create_image_view(s_device, &view_info, nullptr, &slot.view), "vkCreateImageView (ring)"))
		return false;

	slot.handover.image_view = slot.view;
	slot.handover.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkFramebufferCreateInfo fb_info{};
	fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb_info.renderPass = s_render_pass;
	fb_info.attachmentCount = 1;
	fb_info.pAttachments = &slot.view;
	fb_info.width = width;
	fb_info.height = height;
	fb_info.layers = 1;
	if (!check(s_fns.create_framebuffer(s_device, &fb_info, nullptr, &slot.framebuffer), "vkCreateFramebuffer"))
		return false;

	// One of these per 2D layer. Identical in every respect but which draw samples them; the loop is
	// the only thing that stops the two from drifting apart.
	for (layer_tex &l : slot.layers)
	{
		// The texture MAME's layer lands in. Optimal tiling and a copy rather than a linear-tiled image
		// sampled in place: MoltenVK's linear-tiling feature set is narrow, and this is the portable
		// shape regardless.
		VkImageCreateInfo tex_info = image_info;
		tex_info.flags = 0;
		tex_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		if (!check(s_fns.create_image(s_device, &tex_info, nullptr, &l.texture), "vkCreateImage (texture)"))
			return false;
		if (!allocate_and_bind_image(l.texture, l.texture_memory))
			return false;

		VkImageViewCreateInfo tex_view_info = view_info;
		tex_view_info.image = l.texture;
		if (!check(s_fns.create_image_view(s_device, &tex_view_info, nullptr, &l.texture_view), "vkCreateImageView (texture)"))
			return false;

		// Tightly packed: 496x384x4 is 762 KB, and the device's optimalBufferCopyRowPitchAlignment is a
		// performance hint rather than a requirement — it reads 1 here anyway, so there is nothing to
		// pad even for the hint's sake.
		const VkDeviceSize staging_size = VkDeviceSize(width) * VkDeviceSize(height) * 4;

		VkBufferCreateInfo buffer_info{};
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size = staging_size;
		buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (!check(s_fns.create_buffer(s_device, &buffer_info, nullptr, &l.staging), "vkCreateBuffer"))
			return false;

		VkMemoryRequirements buffer_reqs{};
		s_fns.get_buffer_memory_requirements(s_device, l.staging, &buffer_reqs);

		// HOST_VISIBLE | HOST_COHERENT, and no fallback: Vulkan guarantees at least one memory type with
		// both, so this cannot fail on a conforming implementation — and having it means no
		// vkFlushMappedMemoryRanges anywhere, which is one fewer thing to forget.
		uint32_t staging_type = 0;
		if (!find_memory_type(buffer_reqs.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_type))
		{
			vk_log(RETRO_LOG_ERROR, "no host-visible coherent memory type accepts a %llu byte staging buffer\n",
					(unsigned long long)staging_size);
			return false;
		}

		VkMemoryAllocateInfo staging_alloc{};
		staging_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		staging_alloc.allocationSize = buffer_reqs.size;
		staging_alloc.memoryTypeIndex = staging_type;
		if (!check(s_fns.allocate_memory(s_device, &staging_alloc, nullptr, &l.staging_memory), "vkAllocateMemory (staging)"))
			return false;
		if (!check(s_fns.bind_buffer_memory(s_device, l.staging, l.staging_memory, 0), "vkBindBufferMemory"))
			return false;
		if (!check(s_fns.map_memory(s_device, l.staging_memory, 0, VK_WHOLE_SIZE, 0, &l.staging_mapped), "vkMapMemory"))
			return false;

		VkDescriptorSetAllocateInfo set_alloc{};
		set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		set_alloc.descriptorPool = s_descriptor_pool;
		set_alloc.descriptorSetCount = 1;
		set_alloc.pSetLayouts = &s_set_layout;
		if (!check(s_fns.allocate_descriptor_sets(s_device, &set_alloc, &l.descriptor), "vkAllocateDescriptorSets"))
			return false;

		// Written once. The texture it names never changes for the life of the slot, so there is nothing
		// for the per-frame path to update.
		VkDescriptorImageInfo image_binding{};
		image_binding.sampler = s_sampler;   // immutable in the layout; named here for clarity
		image_binding.imageView = l.texture_view;
		image_binding.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = l.descriptor;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &image_binding;
		s_fns.update_descriptor_sets(s_device, 1, &write, 0, nullptr);
	}

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

	// The shared objects first: the slots' framebuffers need the render pass and their descriptor
	// sets need the layout and the pool.
	if (!build_render_pass() || !build_sampler() || !build_descriptors(count) || !build_pipeline())
	{
		destroy_ring();
		return false;
	}

	// Set before the loop so that a slot which fails half-built is still destroyed by destroy_ring.
	s_slot_count = count;
	for (uint32_t i = 0; i < count; i++)
	{
		if (!build_slot(s_slots[i], width, height, iface.queue_index))
		{
			destroy_ring();
			return false;
		}
	}

	vk_log(RETRO_LOG_INFO, "ring of %u %ux%u B8G8R8A8_UNORM images, sync index mask 0x%x, queue family %u; %llu KiB of staging\n",
			unsigned(count), width, height, unsigned(mask), iface.queue_index,
			(unsigned long long)((VkDeviceSize(width) * VkDeviceSize(height) * 4 * count * m2vk::LAYER_COUNT) / 1024));

	// The read-back buffer only exists when someone asked for it. A ring rebuild re-reads the
	// environment but does not re-arm a dump that has already been taken.
	if (char const *const prefix = std::getenv("M2VK_VK_DUMP"))
	{
		s_dump_prefix = prefix;
		char const *const at = std::getenv("M2VK_VK_DUMP_FRAME");
		s_dump_frame = (at != nullptr) ? uint64_t(std::strtoull(at, nullptr, 10)) : 600;
		if (!s_dump_done && !dump_build_buffer(width, height))
		{
			dump_destroy();
			s_dump_prefix.clear();
		}
		else if (!s_dump_done)
		{
			vk_log(RETRO_LOG_INFO, "read-back armed: '%s-{src,vk}.ppm' at presented frame %llu\n",
					s_dump_prefix.c_str(), (unsigned long long)s_dump_frame);
		}
	}

	return true;
}


//============================================================
//  the frame
//============================================================

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

// `draw_over` says whether the frame has a foreground layer to composite. Without one this is P2's
// passthrough exactly: a single opaque fullscreen draw of whatever landed in layer 0.
bool record_and_submit(frame_slot &slot, bool draw_over, bool dump)
{
	if (!check(s_fns.reset_command_pool(s_device, slot.pool, 0), "vkResetCommandPool"))
		return false;

	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (!check(s_fns.begin_command_buffer(slot.cmd, &begin), "vkBeginCommandBuffer"))
		return false;

	const uint32_t uploads = draw_over ? uint32_t(m2vk::LAYER_COUNT) : 1;

	for (uint32_t i = 0; i < uploads; i++)
	{
		layer_tex &l = slot.layers[i];

		// The upload. UNDEFINED as the old layout because last frame's contents are about to be
		// overwritten in full and discarding them is free.
		barrier(slot.cmd, l.texture,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkBufferImageCopy region{};
		region.bufferOffset = 0;
		// Zero means "tightly packed to the image extent", which is what the staging buffer is.
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { s_width, s_height, 1 };
		s_fns.cmd_copy_buffer_to_image(slot.cmd, l.staging, l.texture,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		// Into the layout the descriptor set names, ready for the draw below.
		barrier(slot.cmd, l.texture,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	// The draw. The ring image's transition into SHADER_READ_ONLY_OPTIMAL is the render pass's
	// finalLayout, so there is no closing barrier here and none is missing.
	VkClearValue clear{};
	clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

	VkRenderPassBeginInfo pass{};
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = s_render_pass;
	pass.framebuffer = slot.framebuffer;
	pass.renderArea.offset = { 0, 0 };
	pass.renderArea.extent = { s_width, s_height };
	pass.clearValueCount = 1;
	pass.pClearValues = &clear;
	s_fns.cmd_begin_render_pass(slot.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = float(s_width);
	viewport.height = float(s_height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	s_fns.cmd_set_viewport(slot.cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = { s_width, s_height };
	s_fns.cmd_set_scissor(slot.cmd, 0, 1, &scissor);

	// The sandwich, bottom slice first. Both draws are the same fullscreen triangle; they differ only
	// in the fragment shader and in which layer's descriptor is bound. The 3D goes between them in
	// step 3, which is why this is two draws in one pass rather than one draw of a shader that samples
	// both — a depth-tested geometry pass has to sit in the middle, and it cannot if the background
	// and foreground are resolved in a single fragment.
	s_fns.cmd_bind_pipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
	s_fns.cmd_bind_descriptor_sets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
			0, 1, &slot.layers[m2vk::LAYER_UNDER].descriptor, 0, nullptr);
	s_fns.cmd_draw(slot.cmd, 3, 1, 0, 0);

	if (draw_over)
	{
		s_fns.cmd_bind_pipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_over);
		s_fns.cmd_bind_descriptor_sets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
				0, 1, &slot.layers[m2vk::LAYER_OVER].descriptor, 0, nullptr);
		s_fns.cmd_draw(slot.cmd, 3, 1, 0, 0);
	}

	s_fns.cmd_end_render_pass(slot.cmd);

	// The diagnostic read-back, in this same command buffer so that what lands in the dump buffer is
	// exactly what the frontend is about to be handed. The image is put back into the layout the
	// handover names, so the frame is unaffected by having been looked at.
	if (dump)
	{
		barrier(slot.cmd, slot.image,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				0, VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkBufferImageCopy back{};
		back.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		back.imageExtent = { s_width, s_height, 1 };
		s_fns.cmd_copy_image_to_buffer(slot.cmd, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				s_dump_buffer, 1, &back);

		barrier(slot.cmd, slot.image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

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

bool present_frame(const uint32_t *pixels, unsigned width, unsigned height)
{
	const retro_hw_render_interface_vulkan *const iface = context_interface();
	if (iface == nullptr)
		return false;
	if ((pixels == nullptr) || (width == 0) || (height == 0))
		return false;

	// The composited path when the emulation thread has captured both 2D layers, the passthrough
	// otherwise. "Otherwise" is not only the software renderer: the layers do not exist until the
	// first screen_update has run, and a capture whose geometry disagrees with the frame the OSD
	// handed us is not one to composite from — the picture would be the right size and the wrong
	// content, which is worse than a frame of passthrough.
	m2vk::frame_record const *layers = m2vk::frame_current();
	if ((layers != nullptr)
			&& ((unsigned(layers->layer[m2vk::LAYER_UNDER].width) != width)
				|| (unsigned(layers->layer[m2vk::LAYER_UNDER].height) != height)
				|| (unsigned(layers->layer[m2vk::LAYER_OVER].width) != width)
				|| (unsigned(layers->layer[m2vk::LAYER_OVER].height) != height)))
	{
		layers = nullptr;
	}

	// Re-read every frame. The mask is documented to change — a fullscreen toggle changes the
	// swapchain length — and the spec's promise is that when it does, the device is idle.
	const uint32_t mask = iface->get_sync_index_mask(iface->handle);

	if ((s_slot_count == 0) || (iface != s_iface) || (width != s_width) || (height != s_height) || (mask != s_mask))
	{
		if (s_slot_count != 0)
		{
			vk_log(RETRO_LOG_INFO, "rebuilding the ring: %ux%u mask 0x%x -> %ux%u mask 0x%x\n",
					s_width, s_height, unsigned(s_mask), width, height, unsigned(mask));
		}
		if (!build_ring(*iface, width, height, mask))
			return false;
		s_reported_frame_error = false;
	}

	const uint32_t index = iface->get_sync_index(iface->handle);
	if (index >= s_slot_count)
	{
		if (!s_reported_frame_error)
		{
			vk_log(RETRO_LOG_ERROR, "the frontend returned sync index %u for a ring of %u (mask 0x%x)\n",
					unsigned(index), unsigned(s_slot_count), unsigned(s_mask));
			s_reported_frame_error = true;
		}
		return false;
	}

	// The frontend is done with this slot's image, and has released queue-family ownership of it
	// back to us. Only after this may the slot be touched at all.
	iface->wait_sync_index(iface->handle);

	frame_slot &slot = s_slots[index];

	// And our own previous submit into this slot has retired, so its command buffer can be reset —
	// and, just as importantly, so its staging buffer is no longer being read by a copy in flight.
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

	// The copies of the picture. capture_frame() and capture_layer() have both already packed the
	// visible rectangle tightly, so each is a single memcpy and the destination layout matches exactly.
	//
	// These could be avoided entirely — the capture could write straight into the mapped buffer — but
	// that would put Vulkan-owned memory in the emulation thread's write path and couple the
	// emulator's frame timing to the sync index. Not while the baseline is still being established.
	const size_t bytes = size_t(width) * size_t(height) * sizeof(uint32_t);

	if (layers != nullptr)
	{
		std::memcpy(slot.layers[m2vk::LAYER_UNDER].staging_mapped, layers->layer[m2vk::LAYER_UNDER].pixels.data(), bytes);
		std::memcpy(slot.layers[m2vk::LAYER_OVER].staging_mapped, layers->layer[m2vk::LAYER_OVER].pixels.data(), bytes);
	}
	else
	{
		std::memcpy(slot.layers[m2vk::LAYER_UNDER].staging_mapped, pixels, bytes);
	}

	const bool dump = !s_dump_done && !s_dump_prefix.empty() && (s_dump_mapped != nullptr)
			&& (s_frames == s_dump_frame);

	if (!record_and_submit(slot, layers != nullptr, dump))
	{
		if (!s_reported_frame_error)
		{
			vk_log(RETRO_LOG_ERROR, "slot %u could not be submitted; the picture stops here\n", unsigned(index));
			s_reported_frame_error = true;
		}
		return false;
	}

	// No semaphores: the render pass's closing layout transition is the synchronisation, which the
	// interface documents as the preferred of the two. VK_QUEUE_FAMILY_IGNORED because we submit on
	// the frontend's own queue family, so there is no ownership to transfer.
	iface->set_image(iface->handle, &slot.handover, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);

	// One line per context, saying the Vulkan path is actually carrying the picture. Without it the
	// only difference between "drawing" and "duping every frame" in the log is silence — and after a
	// context loss, "the picture came back" is precisely the thing that needs saying.
	if (s_context_frames == 0)
	{
		if (s_frames == 0)
			vk_log(RETRO_LOG_INFO, "first frame presented: %ux%u through slot %u\n", width, height, unsigned(index));
		else
			vk_log(RETRO_LOG_INFO, "picture resumed at frame %llu: %ux%u through slot %u\n",
					(unsigned long long)s_frames, width, height, unsigned(index));
	}

	// The read-back is in the command buffer just submitted, so the copy has to have retired before
	// the buffer holds anything. This stalls the frame it is taken on, which is the price of the
	// diagnostic and the reason it is one frame and not all of them.
	if (dump)
	{
		const VkResult copied = s_fns.wait_for_fences(s_device, 1, &slot.fence, VK_TRUE, FENCE_TIMEOUT_NS);
		if (copied == VK_SUCCESS)
		{
			write_ppm(s_dump_prefix + "-src.ppm", pixels, width, height);
			write_ppm(s_dump_prefix + "-vk.ppm", s_dump_mapped, width, height);
		}
		else
		{
			vk_log(RETRO_LOG_ERROR, "the read-back never retired: %s\n", vk_result_name(copied));
		}
		s_dump_done = true;
	}

	s_frames++;
	s_context_frames++;
	s_reported_frame_error = false;
	return true;
}

void present_shutdown()
{
	destroy_ring();
	s_context_frames = 0;
	s_reported_frame_error = false;
	// s_frames is deliberately not reset: it counts frames since the content was loaded, so that a
	// read-back armed for frame N lands on frame N however many contexts the run has been through.
	// present_end_run() is what ends a run.
	//
	// s_dump_done is not reset either: one dump per process, so that a context loss partway through
	// a run does not quietly overwrite the fixture with a different frame.
}

void present_abandon()
{
	// The device the ring was built on is gone, so every handle in it is already dead. Dropping them
	// is all that can be done; destroying them would be a use-after-free.
	if (s_slot_count != 0)
		vk_log(RETRO_LOG_WARN, "the context was replaced without being destroyed; %u slots abandoned\n",
				unsigned(s_slot_count));
	forget_ring();
	s_context_frames = 0;
	s_reported_frame_error = false;
}

void present_end_run()
{
	present_shutdown();
	s_frames = 0;
}

} // namespace m2vk
