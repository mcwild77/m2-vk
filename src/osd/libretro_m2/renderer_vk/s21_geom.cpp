// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 21 — the polygon pass (T2: untextured, z-buffered).

    See s21_geom.h for the shape of the phase and s21_seam.h for the stream. This file is the record the
    seam fills on the emulation thread, plus the GPU pipeline that turns it into flat, CLUT-coloured,
    depth-tested triangles on the frontend's thread. Far simpler than s22_geom: no texture system, no
    fog/shade tail, no sprite path, no clip-window batching (S21 does not window the 3D), and quads
    arrive unclipped so the NDC clip and the caller's viewport do the clamping.

    Two things are settled and are the ones to understand before editing:

      * DEPTH IS A REAL PER-QUAD z-BUFFER (the accurate model — S21 z-buffers in hardware). The ring's
        depth attachment is cleared to 0.0; the pipeline tests COMPARE_GREATER and writes, and s21.vert
        maps zsort to z = 1 - zsort/32768, so nearer wins and a coplanar tie falls to the first writer —
        exactly renderscanline_flat's `if (zsort < zbuf[x])` with zbuf cleared to 0x8000.

      * FLAT, CLUT-COLOURED, OPAQUE. One resolved pen per quad; the fragment looks it up in the palette,
        which is the one thing re-uploaded each frame (the game writes palette RAM live). No blend.

    Like s22_geom, per-frame buffers are host-visible device-local and written with no staging copy, and
    the record is turned into buffers on the frontend's thread from data the emulation thread wrote and
    is now parked against, so there is no lock.

*********************************************************************************************************************************/

#include "renderer_vk/s21_geom.h"

#include "s21_seam.h"

#include "renderer_vk/shaders/s21_vert_spv.h"
#include "renderer_vk/shaders/s21_frag_spv.h"
#include "renderer_vk/shaders/fullscreen_vert_spv.h"
#include "renderer_vk/shaders/s21_mix_frag_spv.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace s21 {

// vk_log, vk_result_name, find_memory_type and vk_funcs are the shared renderer's, in namespace m2vk.
using namespace m2vk;

namespace {

//============================================================
//  constants and shapes
//============================================================

// Matches vk_present's ceiling on the sync-index mask; the two are indexed by the same thing.
constexpr uint32_t MAX_SLOTS = 8;

// starblad peaks near 800 quads a frame (T1's tap capped at 763), so this rarely reallocates.
constexpr uint32_t INITIAL_QUAD_CAPACITY = 2048;

// A sanity bound on a count that comes from emulated hardware, and headroom for the index type.
constexpr uint32_t MAX_QUAD_CAPACITY = 1u << 21;

// A sanity bound on the mix buffer's pixel count — well past any real screen size, headroom for a
// buggy capture rather than an expected case.
constexpr uint32_t MAX_MIX_CAPACITY = 1u << 24;

// A true quad: 4 vertices, 2 triangles = 6 indices.
constexpr uint32_t QUAD_VERTS = 4;
constexpr uint32_t QUAD_INDICES = 6;

// The palette CLUT. Namco S21 palette is at most 0x8000 pens; sized to that and re-uploaded each frame.
// The pen indices the seam records live at 0x2000+ (palbase), well inside this.
constexpr VkDeviceSize PALETTE_BYTES = 0x8000 * sizeof(uint32_t);

// 16 bytes: screen x/y, the NDC depth z (already mapped from zsort on the CPU), the flat pen.
struct gpu_vertex
{
	float    x, y, z;
	uint32_t pen;
};

static_assert(sizeof(gpu_vertex) == 16, "the vertex attribute offsets below are written out by hand");

// The push block: the visible picture's half-extent in pixels (s21.vert).
struct push_block
{
	float half_width, half_height;
};

static_assert(sizeof(push_block) == 8, "s21 push block is two floats");

// T2b's push block: the mix buffer's logical dimensions, so s21_mix.frag can turn fullscreen.vert's
// 0..1 v_uv back into a buffer index without caring what the attachment's own pixel size is.
struct mix_push_block
{
	uint32_t width, height;
};

static_assert(sizeof(mix_push_block) == 8, "s21 mix push block is two uints");

struct mapped_buffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void          *mapped = nullptr;
	VkDeviceSize   size = 0;
};

struct geom_slot
{
	mapped_buffer   vertices;
	mapped_buffer   indices;
	mapped_buffer   palette;               // 128 KB, re-uploaded each frame
	VkDescriptorSet descriptor = VK_NULL_HANDLE;
	uint32_t        capacity = 0;          // quads the vertex/index buffers are sized for
	uint32_t        index_count = 0;       // what this slot's recorded draw will submit

	// T2b: the layer-0 z-mix buffer, sized to whatever s21::mix_pixels() last needed (data-dependent,
	// unlike the palette's fixed ceiling) and re-uploaded whenever the driver captured one this frame.
	mapped_buffer   mix_buffer;
	VkDescriptorSet mix_descriptor = VK_NULL_HANDLE;
	uint32_t        mix_capacity = 0;      // pixels the buffer is sized for
	uint32_t        mix_width = 0;         // 0 means geom_draw_mix has nothing to do this frame
	uint32_t        mix_height = 0;
};


//============================================================
//  state
//============================================================

// The record, DOUBLE-BUFFERED to mirror the hardware's double-buffered poly framebuffer. The seam fires
// frame_end()+frame_begin() back-to-back at the swap (there is a single swap site, unlike S22's separate
// begin/end), so a single buffer would be zeroed by record_begin the instant record_end validated it.
// Instead the emulation thread fills the WORK buffer, record_end swaps it into the VISIBLE buffer, and
// the frontend reads the VISIBLE one — which stays put across the next record_begin, exactly as the
// visible framebuffer page survives the next work-page clear.
std::vector<quad> s_quads;              // visible: what the frontend draws
uint32_t s_quad_count = 0;
std::vector<quad> s_quads_work;         // work: what the emulation thread is filling
uint32_t s_quad_work_count = 0;
uint64_t s_serial = 0;
bool     s_valid = false;

// GPU handles, stashed by geom_build and used lazily.
std::array<geom_slot, MAX_SLOTS> s_slots{};
uint32_t s_slot_count = 0;
VkRenderPass s_render_pass = VK_NULL_HANDLE;
const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

VkPipelineLayout      s_pipeline_layout = VK_NULL_HANDLE;
VkPipeline            s_pipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
bool s_ready = false;
bool s_failed = false;

// T2b's fullscreen mix pass. Shares s_set_layout (both it and the 3D pass bind exactly one storage
// buffer, fragment stage) and s_descriptor_pool, sized for two sets per slot; its own pipeline and
// layout because the push constants and depth state differ from the 3D pass.
VkPipelineLayout s_mix_pipeline_layout = VK_NULL_HANDLE;
VkPipeline       s_mix_pipeline = VK_NULL_HANDLE;

// Reporting, once per run.
bool     s_reported_first = false;
uint64_t s_run_quads = 0;
uint32_t s_max_quads = 0;
uint64_t s_drawn_serial = 0;


//============================================================
//  helpers
//============================================================

bool check(VkResult result, char const *what)
{
	if (result == VK_SUCCESS)
		return true;
	vk_log(RETRO_LOG_ERROR, "s21: %s failed: %s\n", what, vk_result_name(result));
	return false;
}

void destroy_buffer(mapped_buffer &b)
{
	if (b.mapped != nullptr)
		s_fns.unmap_memory(s_device, b.memory);
	if (b.buffer != VK_NULL_HANDLE)
		s_fns.destroy_buffer(s_device, b.buffer, nullptr);
	if (b.memory != VK_NULL_HANDLE)
		s_fns.free_memory(s_device, b.memory, nullptr);
	b = mapped_buffer{};
}

// Device-local host-visible, exactly as s22_geom's create_buffer: unified memory, so no staging copy.
bool create_buffer(mapped_buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, char const *what)
{
	destroy_buffer(b);

	VkBufferCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!check(s_fns.create_buffer(s_device, &info, nullptr, &b.buffer), what))
		return false;

	VkMemoryRequirements reqs{};
	s_fns.get_buffer_memory_requirements(s_device, b.buffer, &reqs);

	const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type_index = 0;
	if (!find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits, need | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index)
			&& !find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits, need, type_index))
	{
		vk_log(RETRO_LOG_ERROR, "s21: no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
				(unsigned long long)size, what);
		return false;
	}

	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = type_index;
	if (!check(s_fns.allocate_memory(s_device, &alloc, nullptr, &b.memory), "vkAllocateMemory"))
		return false;
	if (!check(s_fns.bind_buffer_memory(s_device, b.buffer, b.memory, 0), "vkBindBufferMemory"))
		return false;
	if (!check(s_fns.map_memory(s_device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory"))
		return false;

	b.size = size;
	return true;
}

bool size_slot(geom_slot &slot, uint32_t quads)
{
	if (quads > MAX_QUAD_CAPACITY)
	{
		vk_log(RETRO_LOG_ERROR, "s21: a frame of %u quads is past the %u the buffers will size to\n",
				unsigned(quads), unsigned(MAX_QUAD_CAPACITY));
		return false;
	}

	const VkDeviceSize verts = VkDeviceSize(quads) * QUAD_VERTS * sizeof(gpu_vertex);
	const VkDeviceSize idx = VkDeviceSize(quads) * QUAD_INDICES * sizeof(uint32_t);

	if (!create_buffer(slot.vertices, verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vkCreateBuffer (s21 vertices)")
			|| !create_buffer(slot.indices, idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "vkCreateBuffer (s21 indices)"))
	{
		return false;
	}

	slot.capacity = quads;
	return true;
}

// Points a slot's descriptor set at its own palette buffer (the one CLUT binding).
void write_descriptor(geom_slot &slot)
{
	VkDescriptorBufferInfo info{};
	info.buffer = slot.palette.buffer;
	info.offset = 0;
	info.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = slot.descriptor;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &info;

	s_fns.update_descriptor_sets(s_device, 1, &write, 0, nullptr);
}

// Points a slot's mix descriptor set at its own mix buffer. Called whenever that buffer is (re)created,
// unlike the palette (whose buffer, and so whose descriptor write, is fixed at ensure_ready()).
void write_mix_descriptor(geom_slot &slot)
{
	VkDescriptorBufferInfo info{};
	info.buffer = slot.mix_buffer.buffer;
	info.offset = 0;
	info.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = slot.mix_descriptor;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &info;

	s_fns.update_descriptor_sets(s_device, 1, &write, 0, nullptr);
}

bool build_descriptor_layout()
{
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &layout_info, nullptr, &s_set_layout),
			"vkCreateDescriptorSetLayout (s21)"))
	{
		return false;
	}

	// Two sets per slot, same shape (one storage buffer, fragment stage): the 3D pass's palette CLUT and
	// T2b's mix buffer. Neither cares what the other's buffer holds — the layout only fixes the binding
	// shape, not the GLSL struct type behind it.
	VkDescriptorPoolSize size{};
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = s_slot_count * 2;

	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = s_slot_count * 2;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	return check(s_fns.create_descriptor_pool(s_device, &pool_info, nullptr, &s_descriptor_pool),
			"vkCreateDescriptorPool (s21)");
}

bool build_pipeline()
{
	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0;
	push.size = sizeof(push_block);

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &s_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout),
			"vkCreatePipelineLayout (s21)"))
	{
		return false;
	}

	VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &out)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule (s21)");
	};

	bool ok = make_module(S21_VERT_SPV, sizeof(S21_VERT_SPV), vert)
			&& make_module(S21_FRAG_SPV, sizeof(S21_FRAG_SPV), frag);

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

		VkVertexInputBindingDescription binding{};
		binding.binding = 0;
		binding.stride = sizeof(gpu_vertex);
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		VkVertexInputAttributeDescription attrs[2]{};
		attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
		attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32_UINT;         attrs[1].offset = 12;

		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = 2;
		vertex_input.pVertexAttributeDescriptions = attrs;

		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewport_state{};
		viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_state.viewportCount = 1;
		viewport_state.scissorCount = 1;

		// No culling: blit_single_quad already rejected back faces at the seam, so what is recorded is what
		// the software path drew, whatever the fan's winding.
		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// The real z-buffer. The ring's render pass clears depth to 0.0; s21.vert maps nearer (smaller
		// zsort) to a larger z, so COMPARE_GREATER + write reproduces `if (zsort < zbuf[x])` and a coplanar
		// tie (equal z) falls to the first writer, matching the software's strict `<`. Same attachment and
		// clear Model 2's geom pass already uses (which is why S21 borrows GREATER rather than LESS).
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depth.depthTestEnable = VK_TRUE;
		depth.depthWriteEnable = VK_TRUE;
		depth.depthCompareOp = VK_COMPARE_OP_GREATER;
		depth.depthBoundsTestEnable = VK_FALSE;
		depth.stencilTestEnable = VK_FALSE;

		// Opaque: every S21 pen is a solid colour (palbase | ..., never 0), so no blend. The 2D background
		// was drawn by the caller before this pass; covered pixels take the poly colour, the rest keep it.
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
		info.pDepthStencilState = &depth;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = s_pipeline_layout;
		info.renderPass = s_render_pass;
		info.subpass = 0;

		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline),
				"vkCreateGraphicsPipelines (s21)");
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}

// T2b's fullscreen mix pass: no vertex buffer (rides fullscreen.vert off gl_VertexIndex, like
// vk_present.cpp's UNDER/OVER draws), tests GREATER_OR_EQUAL against the depth attachment geom_draw just
// wrote and writes nothing back — a sprite's own threshold, output as gl_FragDepth by s21_mix.frag, is
// what gets compared, not this pipeline's rasterised position.
bool build_mix_pipeline()
{
	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = sizeof(mix_push_block);

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &s_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_mix_pipeline_layout),
			"vkCreatePipelineLayout (s21 mix)"))
	{
		return false;
	}

	VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &out)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule (s21 mix)");
	};

	bool ok = make_module(FULLSCREEN_VERT_SPV, sizeof(FULLSCREEN_VERT_SPV), vert)
			&& make_module(S21_MIX_FRAG_SPV, sizeof(S21_MIX_FRAG_SPV), frag);

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

		// No vertex input: fullscreen.vert computes its three positions from gl_VertexIndex alone.
		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewport_state{};
		viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_state.viewportCount = 1;
		viewport_state.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// GREATER_OR_EQUAL mirrors mix_layer0_sprites' `pri[bank] <= z[x]` exactly (see s21_mix.frag) against
		// the SAME attachment the 3D pass just wrote (LOAD, not cleared, for this pipeline). Write off: a
		// sprite's threshold must not become part of the depth buffer the next frame's 3D tests against, and
		// sprite-vs-sprite ordering is plain draw order (the capture already resolved that on the CPU), not a
		// depth comparison.
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depth.depthTestEnable = VK_TRUE;
		depth.depthWriteEnable = VK_FALSE;
		depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
		depth.depthBoundsTestEnable = VK_FALSE;
		depth.stencilTestEnable = VK_FALSE;

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
		info.pDepthStencilState = &depth;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = s_mix_pipeline_layout;
		info.renderPass = s_render_pass;
		info.subpass = 0;

		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_mix_pipeline),
				"vkCreateGraphicsPipelines (s21 mix)");
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}

// Everything the draw needs: pipeline, descriptor pool/sets, per-slot palette buffers. Built once, on
// the first captured frame. A build that never captures never reaches here, so it makes no Vulkan call.
bool ensure_ready()
{
	if (s_ready)
		return true;
	if (s_failed || (s_device == VK_NULL_HANDLE) || (s_render_pass == VK_NULL_HANDLE))
		return false;

	if (!build_descriptor_layout() || !build_pipeline() || !build_mix_pipeline())
	{
		s_failed = true;
		return false;
	}

	for (uint32_t i = 0; i < s_slot_count; i++)
	{
		geom_slot &slot = s_slots[i];

		VkDescriptorSetAllocateInfo set_alloc{};
		set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		set_alloc.descriptorPool = s_descriptor_pool;
		set_alloc.descriptorSetCount = 1;
		set_alloc.pSetLayouts = &s_set_layout;
		if (!check(s_fns.allocate_descriptor_sets(s_device, &set_alloc, &slot.descriptor),
				"vkAllocateDescriptorSets (s21)"))
		{
			s_failed = true;
			return false;
		}
		if (!check(s_fns.allocate_descriptor_sets(s_device, &set_alloc, &slot.mix_descriptor),
				"vkAllocateDescriptorSets (s21 mix)"))
		{
			s_failed = true;
			return false;
		}

		if (!create_buffer(slot.palette, PALETTE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s21 palette)"))
		{
			s_failed = true;
			return false;
		}

		write_descriptor(slot);
		// slot.mix_buffer is sized lazily in geom_upload() (data-dependent, unlike the palette's fixed
		// ceiling), so its descriptor is left unwritten until then; geom_draw_mix() never binds it before
		// mix_width is set, which only happens after that first write.
	}

	s_ready = true;
	return true;
}

} // anonymous namespace


//============================================================
//  the record — emulation thread
//============================================================

void record_begin()
{
	// Start a new work list; the visible list (what the frontend draws) is untouched.
	s_quad_work_count = 0;
}

void record_quad(quad const &q)
{
	if (s_quad_work_count == s_quads_work.size())
		s_quads_work.resize(s_quads_work.size() + (s_quads_work.size() / 2) + INITIAL_QUAD_CAPACITY);
	s_quads_work[s_quad_work_count++] = q;
}

void record_end()
{
	// The work list just completed; publish it as the visible list the frontend reads. A vector swap, so
	// the previous visible buffer becomes the next work buffer — no allocation in the steady state.
	s_quads.swap(s_quads_work);
	s_quad_count = s_quad_work_count;
	s_valid = true;
	s_serial++;
}


//============================================================
//  the GPU — frontend thread
//============================================================

bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass render_pass, uint32_t slot_count)
{
	s_iface = &iface;
	s_fns = fns;
	s_device = iface.device;
	s_render_pass = render_pass;
	s_slot_count = (slot_count < MAX_SLOTS) ? slot_count : MAX_SLOTS;

	s_ready = false;
	s_failed = false;
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_mix_pipeline = VK_NULL_HANDLE;
	s_mix_pipeline_layout = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	for (geom_slot &slot : s_slots)
	{
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;
		slot.mix_capacity = 0;
		slot.mix_width = 0;
		slot.mix_height = 0;
		slot.mix_descriptor = VK_NULL_HANDLE;
	}
	return true;
}

void geom_destroy()
{
	if (s_device == VK_NULL_HANDLE)
		return;

	for (geom_slot &slot : s_slots)
	{
		destroy_buffer(slot.vertices);
		destroy_buffer(slot.indices);
		destroy_buffer(slot.palette);
		destroy_buffer(slot.mix_buffer);
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;
		slot.mix_capacity = 0;
		slot.mix_width = 0;
		slot.mix_height = 0;
		slot.mix_descriptor = VK_NULL_HANDLE;
	}

	if (s_mix_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_mix_pipeline, nullptr);
	if (s_mix_pipeline_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_mix_pipeline_layout, nullptr);
	if (s_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline, nullptr);
	if (s_pipeline_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_pipeline_layout, nullptr);
	if (s_descriptor_pool != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_pool(s_device, s_descriptor_pool, nullptr);
	if (s_set_layout != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_set_layout(s_device, s_set_layout, nullptr);

	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_mix_pipeline = VK_NULL_HANDLE;
	s_mix_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_ready = false;
	s_failed = false;
	s_render_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
}

void geom_forget()
{
	for (geom_slot &slot : s_slots)
	{
		slot.vertices = mapped_buffer{};
		slot.indices = mapped_buffer{};
		slot.palette = mapped_buffer{};
		slot.mix_buffer = mapped_buffer{};
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;
		slot.mix_capacity = 0;
		slot.mix_width = 0;
		slot.mix_height = 0;
		slot.mix_descriptor = VK_NULL_HANDLE;
	}
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_mix_pipeline = VK_NULL_HANDLE;
	s_mix_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_ready = false;
	s_failed = false;
	s_render_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
	s_device = VK_NULL_HANDLE;
}

bool geom_upload(uint32_t slot_index)
{
	int mix_w = 0, mix_h = 0;
	uint32_t const *const mix_src = mix_pixels(mix_w, mix_h);
	const bool have_quads = s_valid && (s_quad_count != 0);

	// Before any Vulkan work, so a build that captures neither returns here every frame and never builds.
	if (!have_quads && (mix_src == nullptr))
		return false;
	if (slot_index >= s_slot_count)
		return false;
	if (!ensure_ready())
		return false;

	geom_slot &slot = s_slots[slot_index];

	// T2b: the layer-0 mix buffer, independent of whether this frame has any 3D geometry — logically
	// unrelated to the quad stream below, even though in practice a mix capture only ever coincides with
	// active gameplay (pri1==4), which always has polygons too. Its dimensions are the screen's and so are
	// effectively constant frame to frame, so this reallocates once and then just re-copies in place.
	if (mix_src != nullptr)
	{
		const uint32_t pixels = uint32_t(mix_w) * uint32_t(mix_h);
		const bool sized = (pixels != 0) && (pixels <= MAX_MIX_CAPACITY)
				&& ((slot.mix_capacity >= pixels)
					|| create_buffer(slot.mix_buffer, VkDeviceSize(pixels) * sizeof(uint32_t),
							VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s21 mix)"));
		if (sized)
		{
			if (slot.mix_capacity < pixels)
			{
				slot.mix_capacity = pixels;
				write_mix_descriptor(slot);
			}
			std::memcpy(slot.mix_buffer.mapped, mix_src, size_t(pixels) * sizeof(uint32_t));
			slot.mix_width = uint32_t(mix_w);
			slot.mix_height = uint32_t(mix_h);
		}
		else
		{
			slot.mix_width = 0;
			slot.mix_height = 0;
		}
	}
	else
	{
		slot.mix_width = 0;
		slot.mix_height = 0;
	}

	if (!have_quads)
		return false;

	if ((slot.capacity < s_quad_count) && !size_slot(slot, s_quad_count + (s_quad_count / 2)))
		return false;

	// The palette is re-uploaded each frame (the game writes palette RAM live). Copy up to the buffer's
	// capacity; the shader guards a pen past the array length.
	if (palette_ram const &p = get_palette(); p.pens != nullptr)
	{
		const VkDeviceSize want = VkDeviceSize(p.count) * sizeof(uint32_t);
		std::memcpy(slot.palette.mapped, p.pens, size_t((want < PALETTE_BYTES) ? want : PALETTE_BYTES));
	}

	auto *const verts = static_cast<gpu_vertex *>(slot.vertices.mapped);
	auto *const idx = static_cast<uint32_t *>(slot.indices.mapped);
	uint32_t vcount = 0, icount = 0;

	for (uint32_t qi = 0; qi < s_quad_count; qi++)
	{
		quad const &q = s_quads[qi];

		// zsort -> NDC depth: nearer (smaller zsort) maps to a larger z, so COMPARE_GREATER selects it.
		// Constant across the four corners. 32768.0 = the 0x8000 the software zbuffer clears to.
		const float z = 1.0f - float(q.zsort) / 32768.0f;

		const uint32_t vbase = vcount;
		for (int i = 0; i < 4; i++)
		{
			gpu_vertex &v = verts[vcount++];
			v.x = float(q.x[i]);
			v.y = float(q.y[i]);
			v.z = z;
			v.pen = uint32_t(q.color);
		}
		// The two triangles blit_single_quad draws: rendertri(v0,v1,v2) and rendertri(v2,v3,v0).
		idx[icount++] = vbase;     idx[icount++] = vbase + 1; idx[icount++] = vbase + 2;
		idx[icount++] = vbase + 2; idx[icount++] = vbase + 3; idx[icount++] = vbase;
	}

	slot.index_count = icount;

	// Stats and the once-per-run first line.
	if (s_serial != s_drawn_serial)
	{
		s_drawn_serial = s_serial;
		s_run_quads += s_quad_count;
		if (s_quad_count > s_max_quads)
			s_max_quads = s_quad_count;
	}
	if (!s_reported_first && (icount != 0))
	{
		s_reported_first = true;
		vk_log(RETRO_LOG_INFO, "s21: first GPU geometry — %u quads, %u indices\n",
				unsigned(s_quad_count), unsigned(icount));
	}

	return icount != 0;
}

void geom_draw(uint32_t slot_index, VkCommandBuffer cmd, unsigned width, unsigned height)
{
	if (!s_ready || (slot_index >= s_slot_count))
		return;

	geom_slot &slot = s_slots[slot_index];
	if ((slot.index_count == 0) || (slot.descriptor == VK_NULL_HANDLE))
		return;

	// The VISIBLE half-extent, resolution-invariant: the vertex shader turns framebuffer pixels into
	// NDC, so the attachment size never enters here. The caller has already set the viewport and scissor
	// to the attachment extent. S21 does not window the 3D, so there is one draw covering everything;
	// quads that overspill the frame are clamped by the NDC clip, as the software path clamps per scanline.
	push_block push{};
	push.half_width = float(width) * 0.5f;
	push.half_height = float(height) * 0.5f;

	const VkDeviceSize offset = 0;
	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
			0, 1, &slot.descriptor, 0, nullptr);
	s_fns.cmd_push_constants(cmd, s_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
	s_fns.cmd_bind_vertex_buffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
	s_fns.cmd_bind_index_buffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
	s_fns.cmd_draw_indexed(cmd, slot.index_count, 1, 0, 0, 0);
}

void geom_draw_mix(uint32_t slot_index, VkCommandBuffer cmd)
{
	if (!s_ready || (slot_index >= s_slot_count))
		return;

	geom_slot &slot = s_slots[slot_index];
	if ((slot.mix_width == 0) || (slot.mix_height == 0) || (slot.mix_descriptor == VK_NULL_HANDLE))
		return;

	// The caller's viewport/scissor already cover the attachment (set once, before the sandwich); this
	// pass needs neither the attachment size nor slot.vertices — s21_mix.frag turns fullscreen.vert's
	// 0..1 v_uv back into a buffer index using only the mix buffer's own logical dimensions.
	mix_push_block push{};
	push.width = slot.mix_width;
	push.height = slot.mix_height;

	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mix_pipeline);
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mix_pipeline_layout,
			0, 1, &slot.mix_descriptor, 0, nullptr);
	s_fns.cmd_push_constants(cmd, s_mix_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s_fns.cmd_draw(cmd, 3, 1, 0, 0);
}

void geom_end_run()
{
	if (s_reported_first)
	{
		vk_log(RETRO_LOG_INFO, "s21: run end — %llu quads over drawn frames (max/frame %u)\n",
				(unsigned long long)s_run_quads, unsigned(s_max_quads));
	}
	s_reported_first = false;
	s_run_quads = 0;
	s_max_quads = 0;
	s_drawn_serial = 0;
	s_valid = false;
	s_quad_count = 0;
	s_quad_work_count = 0;
}

} // namespace s21
