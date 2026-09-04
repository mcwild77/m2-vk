// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 1 — the polygon pass (M1-2: untextured, painter's order).

    See m1_geom.h for the shape of the phase and m1_seam.h for the stream. This file is the record the
    seam fills on the emulation thread, plus the GPU pipeline that turns it into flat-shaded triangles on
    the frontend's thread. It is a stripped s22_geom: no texture system, no palette buffer, no CLUT and
    no descriptor sets — every quad is four corners and one flat 0x00RRGGBB (+ MOIRE bit), so the colour
    rides the vertex and the only uniform is the push block (the visible half-extent + the stipple divisor).

    DEPTH IS DRAW ORDER, NOT z. The seam records in sort_quads' back-to-front order, so this is a plain
    painter's pass: draw in record order, last writer wins, depth test disabled. No depth attachment use.

    Per-frame buffers are host-visible device-local and written with no staging copy, and the record is
    turned into buffers on the frontend's thread from data the emulation thread wrote and is now parked
    against, so there is no lock — the same argument s22_geom / vk_geom rest on.

*********************************************************************************************************************************/

#include "renderer_vk/m1_geom.h"

#include "m1_seam.h"

#include "renderer_vk/shaders/m1_vert_spv.h"
#include "renderer_vk/shaders/m1_frag_spv.h"
#include "renderer_vk/shaders/m1_smooth_vert_spv.h"
#include "renderer_vk/shaders/m1_smooth_frag_spv.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace m1 {

// vk_log, vk_result_name, find_memory_type and vk_funcs are the shared renderer's, in namespace m2vk.
using namespace m2vk;

namespace {

//============================================================
//  constants and shapes
//============================================================

// Matches vk_present's ceiling on the sync-index mask; the two are indexed by the same thing.
constexpr uint32_t MAX_SLOTS = 8;

// VF peaks near 2800 quads a frame (the M1-1 tap); this never reallocates in the steady state.
constexpr uint32_t INITIAL_QUAD_CAPACITY = 4096;

// A sanity bound on a count that comes from emulated hardware, and headroom for the index type.
constexpr uint32_t MAX_QUAD_CAPACITY = 1u << 21;

// A quad is two triangles: 4 vertices, 6 indices.
constexpr uint32_t VERTS_PER_QUAD = 4;
constexpr uint32_t INDICES_PER_QUAD = 6;

// One quad vertex, shared by BOTH pipelines. The flat pipeline reads only pos/col/albedo (locations
// 0/4/5); the smooth pipeline reads the whole thing. Keeping one layout means the live Smooth Shading
// toggle is a pipeline swap at draw time with no buffer rebuild — the extra fields are simply left zero
// (and unread) when Smooth Shading is off.
//   pos       screen position (sub-pixel float from the seam, M1-3).
//   normal    per-vertex view-space normal, welded from adjacent face normals (Smooth Shading).
//   light     view-space light direction, normalised (Smooth Shading; flat across the quad).
//   lparams   this face's ambient/diffuse/specular/power (Smooth Shading; flat across the quad).
//   col       resolved lit colour: 0x00RRGGBB | MOIRE<<24 | HAS_NORMAL<<25.
//   albedo    pre-luma albedo (0x00RRGGBB, no MOIRE), the "No Lighting" colour.
struct gpu_vertex
{
	float    x, y;             // offset 0
	float    nx, ny, nz;       // offset 8
	float    lx, ly, lz;       // offset 20
	float    amb, dif, spc, pw; // offset 32
	uint32_t col;              // offset 48
	uint32_t albedo;           // offset 52
};

static_assert(sizeof(gpu_vertex) == 56, "the vertex attribute offsets below are written out by hand");

// The push block, shared by m1.vert (half_size) and m1.frag (stipple_div + flat_luma).
struct push_block
{
	float    half_width, half_height;
	uint32_t stipple_div;
	uint32_t flat_luma;   // "No Lighting": non-zero → the fragment emits albedo instead of the lit colour
};

static_assert(sizeof(push_block) == 16, "m1 push block is four words");

struct mapped_buffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void          *mapped = nullptr;
	VkDeviceSize   size = 0;
};

struct geom_slot
{
	mapped_buffer vertices;
	mapped_buffer indices;
	uint32_t      capacity = 0;      // quads the vertex/index buffers are sized for
	uint32_t      index_count = 0;   // what this slot's recorded draw will submit
};


//============================================================
//  state
//============================================================

// The record. Written on the emulation thread, read on the frontend's, never at the same time.
std::vector<quad> s_quads;
uint32_t s_quad_count = 0;
uint64_t s_serial = 0;
bool     s_valid = false;

// GPU handles, stashed by geom_build and used lazily.
std::array<geom_slot, MAX_SLOTS> s_slots{};
uint32_t s_slot_count = 0;
VkRenderPass s_render_pass = VK_NULL_HANDLE;
const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

VkPipelineLayout s_pipeline_layout = VK_NULL_HANDLE;
VkPipeline       s_pipeline = VK_NULL_HANDLE;
bool s_ready = false;
bool s_failed = false;

// "Smooth Shading" (Model 1 only, opt-in): a second pipeline over the same vertex buffer, plus a storage
// buffer holding a snapshot of the game's color_xlat LUT and a single descriptor set pointing at it. Built
// once alongside the flat pipeline; if the build fails, Smooth Shading silently falls back to flat. The
// LUT buffer is a fixed max size (one byte per possible LUT entry, packed four per uint) so its descriptor
// can be written once; geom_upload memcpys the current snapshot into it.
constexpr uint32_t XLAT_MAX_ENTRIES = 0x6000;   // 24576, ≥ the 0x5f40 the driver's index range reaches
VkPipelineLayout      s_smooth_layout = VK_NULL_HANDLE;
VkPipeline            s_smooth_pipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout s_smooth_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_smooth_pool = VK_NULL_HANDLE;
VkDescriptorSet       s_smooth_set = VK_NULL_HANDLE;
mapped_buffer         s_xlat_buffer;
bool s_smooth_ready = false;    // the smooth pipeline + descriptor built and usable
bool s_xlat_filled = false;     // the LUT buffer holds a valid snapshot this run
bool s_smooth_reported = false; // one-time "smooth pass active" log

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
	vk_log(RETRO_LOG_ERROR, "m1: %s failed: %s\n", what, vk_result_name(result));
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

// Device-local host-visible, exactly as vk_geom / s22_geom: unified memory, so no staging copy.
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

	// DEVICE_LOCAL on top of HOST_VISIBLE|HOST_COHERENT is preferred (BAR memory) but can live in a tiny
	// heap — the ~256 MB PCIe aperture on a discrete GPU without resizable BAR — that a frontend sharing
	// the device may have already spent, so a failed allocation there falls back to plain host-visible
	// rather than being fatal. See vk_geom.cpp create_buffer for the full reasoning.
	const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t preferred_type = 0, fallback_type = 0;
	const bool have_preferred = find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits,
			need | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, preferred_type);
	const bool have_fallback = find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits, need, fallback_type);
	if (!have_preferred && !have_fallback)
	{
		vk_log(RETRO_LOG_ERROR, "m1: no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
				(unsigned long long)size, what);
		return false;
	}

	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;

	VkResult ar = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (have_preferred)
	{
		alloc.memoryTypeIndex = preferred_type;
		ar = s_fns.allocate_memory(s_device, &alloc, nullptr, &b.memory);
	}
	if (ar != VK_SUCCESS && have_fallback && fallback_type != preferred_type)
	{
		if (have_preferred)
			vk_log(RETRO_LOG_WARN, "m1: device-local host-visible heap could not hold a %llu byte buffer (%s); "
					"falling back to plain host-visible memory\n", (unsigned long long)reqs.size, what);
		alloc.memoryTypeIndex = fallback_type;
		ar = s_fns.allocate_memory(s_device, &alloc, nullptr, &b.memory);
	}
	if (ar != VK_SUCCESS)
	{
		vk_log(RETRO_LOG_ERROR, "m1: vkAllocateMemory failed for a %llu byte buffer (%s): %d\n",
				(unsigned long long)reqs.size, what, int(ar));
		return false;
	}
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
		vk_log(RETRO_LOG_ERROR, "m1: a frame of %u quads is past the %u the buffers will size to\n",
				unsigned(quads), unsigned(MAX_QUAD_CAPACITY));
		return false;
	}

	const VkDeviceSize verts = VkDeviceSize(quads) * VERTS_PER_QUAD * sizeof(gpu_vertex);
	const VkDeviceSize idx = VkDeviceSize(quads) * INDICES_PER_QUAD * sizeof(uint32_t);

	if (!create_buffer(slot.vertices, verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vkCreateBuffer (m1 vertices)")
			|| !create_buffer(slot.indices, idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "vkCreateBuffer (m1 indices)"))
	{
		return false;
	}

	slot.capacity = quads;
	return true;
}

// Builds one graphics pipeline over the shared fat vertex. Everything but the shaders, the vertex
// attributes and the pipeline layout is identical between the flat and smooth passes (no cull, depth test
// off, no blend, painter's order), so it lives here once. `out` receives the pipeline; the shader modules
// are created and destroyed within.
bool create_pipeline(uint32_t const *vcode, size_t vbytes, uint32_t const *fcode, size_t fbytes,
		VkVertexInputAttributeDescription const *attrs, uint32_t attr_count,
		VkPipelineLayout layout, VkPipeline &out, char const *what)
{
	VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &mod)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &mod), "vkCreateShaderModule (m1)");
	};

	bool ok = make_module(vcode, vbytes, vert) && make_module(fcode, fbytes, frag);

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

		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = attr_count;
		vertex_input.pVertexAttributeDescriptions = attrs;

		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewport_state{};
		viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_state.viewportCount = 1;
		viewport_state.scissorCount = 1;

		// No culling: Model 1's software path fills whatever winding the quad has, so does this.
		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// Painter's order: draw in record order, last writer wins, depth test off. The state is still
		// supplied because the ring's render pass carries a depth attachment (Model 2's) and a pipeline
		// used with it must declare depth-stencil state even when it does nothing.
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depth.depthTestEnable = VK_FALSE;
		depth.depthWriteEnable = VK_FALSE;
		depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
		depth.depthBoundsTestEnable = VK_FALSE;
		depth.stencilTestEnable = VK_FALSE;

		// Every surviving fragment is opaque (translucency is the MOIRE stipple, a discard). No blend.
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
		info.layout = layout;
		info.renderPass = s_render_pass;
		info.subpass = 0;

		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &out), what);
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}

bool build_pipeline()
{
	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = sizeof(push_block);

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 0;              // no descriptor sets — colour rides the vertex
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout),
			"vkCreatePipelineLayout (m1)"))
	{
		return false;
	}

	// The flat pass reads only position, colour and albedo out of the shared fat vertex — locations
	// 0/1/2 in m1.vert map to offsets 0/48/52. The smooth-only fields in between are simply not declared.
	VkVertexInputAttributeDescription attrs[3]{};
	attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT; attrs[0].offset = 0;
	attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32_UINT;      attrs[1].offset = 48;
	attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32_UINT;      attrs[2].offset = 52;

	return create_pipeline(M1_VERT_SPV, sizeof(M1_VERT_SPV), M1_FRAG_SPV, sizeof(M1_FRAG_SPV),
			attrs, 3, s_pipeline_layout, s_pipeline, "vkCreateGraphicsPipelines (m1)");
}

// The Smooth Shading pipeline: one descriptor set (the color_xlat LUT storage buffer) plus the six-attr
// vertex layout. Best-effort — a failure here leaves s_smooth_ready false and the draw silently uses the
// flat pipeline, so the core still runs. The LUT buffer is created at a fixed max size and its descriptor
// written once; geom_upload memcpys the current snapshot into it.
bool build_smooth()
{
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo set_layout_info{};
	set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = 1;
	set_layout_info.pBindings = &binding;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &set_layout_info, nullptr, &s_smooth_set_layout),
			"vkCreateDescriptorSetLayout (m1 smooth)"))
		return false;

	VkDescriptorPoolSize pool_size{};
	pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_size.descriptorCount = 1;

	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	if (!check(s_fns.create_descriptor_pool(s_device, &pool_info, nullptr, &s_smooth_pool),
			"vkCreateDescriptorPool (m1 smooth)"))
		return false;

	VkDescriptorSetAllocateInfo set_alloc{};
	set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_alloc.descriptorPool = s_smooth_pool;
	set_alloc.descriptorSetCount = 1;
	set_alloc.pSetLayouts = &s_smooth_set_layout;
	if (!check(s_fns.allocate_descriptor_sets(s_device, &set_alloc, &s_smooth_set),
			"vkAllocateDescriptorSets (m1 smooth)"))
		return false;

	// One byte per LUT entry, packed four to a uint32 in the buffer (the shader unpacks). Fixed max size,
	// so the descriptor below can be written once and stay valid for the run.
	if (!create_buffer(s_xlat_buffer, XLAT_MAX_ENTRIES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			"vkCreateBuffer (m1 xlat)"))
		return false;

	VkDescriptorBufferInfo buf_info{};
	buf_info.buffer = s_xlat_buffer.buffer;
	buf_info.offset = 0;
	buf_info.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = s_smooth_set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &buf_info;
	s_fns.update_descriptor_sets(s_device, 1, &write, 0, nullptr);

	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = sizeof(push_block);

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &s_smooth_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_smooth_layout),
			"vkCreatePipelineLayout (m1 smooth)"))
		return false;

	// The full fat vertex: pos, normal, light, lparams, col, albedo — locations 0..5 in m1_smooth.vert.
	VkVertexInputAttributeDescription attrs[6]{};
	attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT;       attrs[0].offset = 0;
	attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[1].offset = 8;
	attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[2].offset = 20;
	attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[3].offset = 32;
	attrs[4].location = 4; attrs[4].binding = 0; attrs[4].format = VK_FORMAT_R32_UINT;            attrs[4].offset = 48;
	attrs[5].location = 5; attrs[5].binding = 0; attrs[5].format = VK_FORMAT_R32_UINT;            attrs[5].offset = 52;

	return create_pipeline(M1_SMOOTH_VERT_SPV, sizeof(M1_SMOOTH_VERT_SPV),
			M1_SMOOTH_FRAG_SPV, sizeof(M1_SMOOTH_FRAG_SPV),
			attrs, 6, s_smooth_layout, s_smooth_pipeline, "vkCreateGraphicsPipelines (m1 smooth)");
}

// The pipeline, built once on the first captured frame. A build that never captures Model 1 never
// reaches here, so it makes no Vulkan call from this file. The flat pipeline is required; the smooth one
// is best-effort (its failure just disables Smooth Shading).
bool ensure_ready()
{
	if (s_ready)
		return true;
	if (s_failed || (s_device == VK_NULL_HANDLE) || (s_render_pass == VK_NULL_HANDLE))
		return false;

	if (!build_pipeline())
	{
		s_failed = true;
		return false;
	}

	s_smooth_ready = build_smooth();
	if (!s_smooth_ready)
		vk_log(RETRO_LOG_WARN, "m1: Smooth Shading pipeline unavailable; that option will draw flat\n");

	s_ready = true;
	return true;
}

} // anonymous namespace


//============================================================
//  the record — emulation thread
//============================================================

void record_begin()
{
	s_quad_count = 0;
	s_valid = false;
}

void record_quad(quad const &q)
{
	if (s_quad_count == s_quads.size())
		s_quads.resize(s_quads.size() + (s_quads.size() / 2) + INITIAL_QUAD_CAPACITY);
	s_quads[s_quad_count++] = q;
}

void record_end()
{
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
	s_smooth_pipeline = VK_NULL_HANDLE;
	s_smooth_layout = VK_NULL_HANDLE;
	s_smooth_set_layout = VK_NULL_HANDLE;
	s_smooth_pool = VK_NULL_HANDLE;
	s_smooth_set = VK_NULL_HANDLE;
	s_smooth_ready = false;
	s_xlat_filled = false;
	s_smooth_reported = false;
	for (geom_slot &slot : s_slots)
	{
		slot.capacity = 0;
		slot.index_count = 0;
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
		slot.capacity = 0;
		slot.index_count = 0;
	}

	destroy_buffer(s_xlat_buffer);
	if (s_smooth_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_smooth_pipeline, nullptr);
	if (s_smooth_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_smooth_layout, nullptr);
	if (s_smooth_pool != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_pool(s_device, s_smooth_pool, nullptr);   // frees the set too
	if (s_smooth_set_layout != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_set_layout(s_device, s_smooth_set_layout, nullptr);

	if (s_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline, nullptr);
	if (s_pipeline_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_pipeline_layout, nullptr);

	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_smooth_pipeline = VK_NULL_HANDLE;
	s_smooth_layout = VK_NULL_HANDLE;
	s_smooth_set_layout = VK_NULL_HANDLE;
	s_smooth_pool = VK_NULL_HANDLE;
	s_smooth_set = VK_NULL_HANDLE;
	s_smooth_ready = false;
	s_xlat_filled = false;
	s_smooth_reported = false;
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
		slot.capacity = 0;
		slot.index_count = 0;
	}
	s_xlat_buffer = mapped_buffer{};
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_smooth_pipeline = VK_NULL_HANDLE;
	s_smooth_layout = VK_NULL_HANDLE;
	s_smooth_set_layout = VK_NULL_HANDLE;
	s_smooth_pool = VK_NULL_HANDLE;
	s_smooth_set = VK_NULL_HANDLE;
	s_smooth_ready = false;
	s_xlat_filled = false;
	s_smooth_reported = false;
	s_ready = false;
	s_failed = false;
	s_render_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
	s_device = VK_NULL_HANDLE;
}

namespace {

// Smooth Shading normal welding. Model 1 has no per-vertex normals, so we synthesise them by summing the
// authored face normals of every quad meeting at a vertex — keyed on the bit-exact view-space corner
// position, so strip-shared vertices (the same driver point_t → identical floats) weld while coplanar
// decals do not. The map is reused across frames (clear() keeps its capacity) so the steady state does no
// allocation.
struct weld_key
{
	uint32_t a, b, c;
	bool operator==(weld_key const &o) const { return a == o.a && b == o.b && c == o.c; }
};

struct weld_hash
{
	size_t operator()(weld_key const &k) const
	{
		uint64_t h = 1469598103934665603ull;
		for (uint32_t w : { k.a, k.b, k.c })
			h = (h ^ w) * 1099511628211ull;
		return size_t(h);
	}
};

struct normal_sum { float x = 0, y = 0, z = 0; };

weld_key make_weld_key(float x, float y, float z)
{
	weld_key k;
	std::memcpy(&k.a, &x, 4);
	std::memcpy(&k.b, &y, 4);
	std::memcpy(&k.c, &z, 4);
	return k;
}

std::unordered_map<weld_key, normal_sum, weld_hash> s_weld;

} // anonymous namespace

bool geom_upload(uint32_t slot_index)
{
	// Before any Vulkan work, so a build that never captures Model 1 returns here every frame and never
	// builds anything.
	if (!s_valid || (s_quad_count == 0) || (slot_index >= s_slot_count))
		return false;
	if (!ensure_ready())
		return false;

	geom_slot &slot = s_slots[slot_index];
	if ((slot.capacity < s_quad_count) && !size_slot(slot, s_quad_count + (s_quad_count / 2)))
		return false;

	// Smooth Shading is live: on only when the toggle is set AND its pipeline built. When on, refresh the
	// LUT storage buffer from the seam's snapshot and weld a per-vertex normal for every smooth-capable
	// quad. When off, none of this runs and the fat vertex's normal fields are left zero (and unread).
	const bool smooth_on = smooth() && s_smooth_ready;
	if (smooth_on)
	{
		unsigned xc = 0;
		uint8_t const *const xd = color_xlat_snapshot(xc);
		if ((xd != nullptr) && (xc != 0))
		{
			const unsigned n = (xc < XLAT_MAX_ENTRIES) ? xc : XLAT_MAX_ENTRIES;
			std::memcpy(s_xlat_buffer.mapped, xd, n);
			s_xlat_filled = true;
		}

		s_weld.clear();
		if (s_weld.bucket_count() < s_quad_count * 4)
			s_weld.reserve(s_quad_count * 4);
		for (uint32_t qi = 0; qi < s_quad_count; qi++)
		{
			quad const &q = s_quads[qi];
			if (!q.has_normal)
				continue;
			for (uint32_t i = 0; i < VERTS_PER_QUAD; i++)
			{
				normal_sum &s = s_weld[make_weld_key(q.vx[i], q.vy[i], q.vz[i])];
				s.x += q.fnx; s.y += q.fny; s.z += q.fnz;
			}
		}
	}

	auto *const verts = static_cast<gpu_vertex *>(slot.vertices.mapped);
	auto *const idx = static_cast<uint32_t *>(slot.indices.mapped);
	uint32_t vcount = 0, icount = 0;

	// Painter's: emit the quads in record order (already sort_quads' back-to-front), two triangles each.
	for (uint32_t qi = 0; qi < s_quad_count; qi++)
	{
		quad const &q = s_quads[qi];
		const uint32_t vbase = vcount;
		// HAS_NORMAL rides the colour word so the smooth fragment can tell a smooth-capable quad from a flat
		// draw_direct one; harmless to the flat pass (bits above 24 do not touch its RGB or MOIRE reads).
		const uint32_t col = q.col | (q.has_normal ? COL_HAS_NORMAL : 0u);
		for (uint32_t i = 0; i < VERTS_PER_QUAD; i++)
		{
			gpu_vertex &v = verts[vcount++];
			v.x = q.x[i];      // sub-pixel float from the seam (M1-3); the viewport scales it to draw res
			v.y = q.y[i];
			v.col = col;
			v.albedo = q.albedo;   // pre-luma pen; the frag picks this over col when No Lighting is on
			if (smooth_on && q.has_normal)
			{
				// The welded normal (fall back to the face normal at a vertex that welded with nothing, e.g.
				// a clip-edge point), normalised so the interpolation is between unit vectors.
				normal_sum const &s = s_weld[make_weld_key(q.vx[i], q.vy[i], q.vz[i])];
				float nx = s.x, ny = s.y, nz = s.z;
				float len2 = nx * nx + ny * ny + nz * nz;
				if (len2 < 1e-12f)
				{
					nx = q.fnx; ny = q.fny; nz = q.fnz;
					len2 = nx * nx + ny * ny + nz * nz;
				}
				const float inv = (len2 > 0.0f) ? (1.0f / std::sqrt(len2)) : 0.0f;
				v.nx = nx * inv; v.ny = ny * inv; v.nz = nz * inv;
				v.lx = q.lx; v.ly = q.ly; v.lz = q.lz;
				v.amb = q.la; v.dif = q.ld; v.spc = q.ls; v.pw = q.lp;
			}
			else
			{
				v.nx = v.ny = v.nz = 0.0f;
				v.lx = v.ly = v.lz = 0.0f;
				v.amb = v.dif = v.spc = v.pw = 0.0f;
			}
		}
		idx[icount++] = vbase;     idx[icount++] = vbase + 1; idx[icount++] = vbase + 2;
		idx[icount++] = vbase;     idx[icount++] = vbase + 2; idx[icount++] = vbase + 3;
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
		vk_log(RETRO_LOG_INFO, "m1: first GPU geometry — %u quads, %u indices\n",
				unsigned(s_quad_count), unsigned(icount));
	}

	return icount != 0;
}

void geom_draw(uint32_t slot_index, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height)
{
	if (!s_ready || (slot_index >= s_slot_count))
		return;

	geom_slot &slot = s_slots[slot_index];
	if (slot.index_count == 0)
		return;

	push_block push{};
	push.half_width = float(width) * 0.5f;
	push.half_height = float(height) * 0.5f;
	// One moiré checker square is one BITMAP pixel; at a raised internal resolution it spans this many
	// attachment pixels. Rounded to nearest, floored at 1 — the picture is never drawn below native.
	{
		const float ratio = float(draw_width) / float(width);
		uint32_t div = uint32_t(ratio + 0.5f);
		push.stipple_div = (div < 1u) ? 1u : div;
	}
	// "No Lighting" (model2_flat_luma): read live here, so a menu toggle takes effect on the next frame with
	// no re-capture — the seam always carries both the lit colour and the pre-luma albedo.
	push.flat_luma = no_lighting() ? 1u : 0u;

	// Smooth Shading is a live pipeline swap over the same vertex buffer: only when the toggle is on, the
	// pipeline built, and the LUT snapshot uploaded — otherwise the flat pass draws, unchanged.
	const bool use_smooth = smooth() && s_smooth_ready && s_xlat_filled;
	if (use_smooth && !s_smooth_reported)
	{
		s_smooth_reported = true;
		vk_log(RETRO_LOG_INFO, "m1: Smooth Shading pass active\n");
	}

	const VkDeviceSize offset = 0;
	if (use_smooth)
	{
		s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_smooth_pipeline);
		s_fns.cmd_push_constants(cmd, s_smooth_layout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_smooth_layout,
				0, 1, &s_smooth_set, 0, nullptr);
	}
	else
	{
		s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
		s_fns.cmd_push_constants(cmd, s_pipeline_layout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	}
	s_fns.cmd_bind_vertex_buffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
	s_fns.cmd_bind_index_buffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
	s_fns.cmd_draw_indexed(cmd, slot.index_count, 1, 0, 0, 0);
}

void geom_end_run()
{
	if (s_reported_first)
	{
		vk_log(RETRO_LOG_INFO, "m1: run end — %llu quads over drawn frames (max/frame %u)\n",
				(unsigned long long)s_run_quads, unsigned(s_max_quads));
	}
	s_reported_first = false;
	s_run_quads = 0;
	s_max_quads = 0;
	s_drawn_serial = 0;
	s_valid = false;
	s_quad_count = 0;
	// Smooth Shading per-run flags: the LUT must be re-uploaded and the "active" line re-logged next run.
	s_xlat_filled = false;
	s_smooth_reported = false;
}

uint32_t geom_primitive_count()
{
	return s_quad_count;
}

} // namespace m1
