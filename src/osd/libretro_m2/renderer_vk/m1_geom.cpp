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

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// One flat quad vertex: screen position, the resolved lit colour (0x00RRGGBB | MOIRE<<24), and the
// pre-luma albedo (0x00RRGGBB, no MOIRE) the "No Lighting" toggle emits instead.
struct gpu_vertex
{
	float    x, y;
	uint32_t col;
	uint32_t albedo;
};

static_assert(sizeof(gpu_vertex) == 16, "the vertex attribute offsets below are written out by hand");

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

	const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type_index = 0;
	if (!find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits, need | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index)
			&& !find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits, need, type_index))
	{
		vk_log(RETRO_LOG_ERROR, "m1: no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
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

	VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &out)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule (m1)");
	};

	bool ok = make_module(M1_VERT_SPV, sizeof(M1_VERT_SPV), vert)
			&& make_module(M1_FRAG_SPV, sizeof(M1_FRAG_SPV), frag);

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

		VkVertexInputAttributeDescription attrs[3]{};
		attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT; attrs[0].offset = 0;
		attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32_UINT;      attrs[1].offset = 8;
		attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32_UINT;      attrs[2].offset = 12;

		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = 3;
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
		info.layout = s_pipeline_layout;
		info.renderPass = s_render_pass;
		info.subpass = 0;

		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline),
				"vkCreateGraphicsPipelines (m1)");
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}

// The pipeline, built once on the first captured frame. A build that never captures Model 1 never
// reaches here, so it makes no Vulkan call from this file.
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

	if (s_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline, nullptr);
	if (s_pipeline_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_pipeline_layout, nullptr);

	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
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
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_ready = false;
	s_failed = false;
	s_render_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
	s_device = VK_NULL_HANDLE;
}

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

	auto *const verts = static_cast<gpu_vertex *>(slot.vertices.mapped);
	auto *const idx = static_cast<uint32_t *>(slot.indices.mapped);
	uint32_t vcount = 0, icount = 0;

	// Painter's: emit the quads in record order (already sort_quads' back-to-front), two triangles each.
	for (uint32_t qi = 0; qi < s_quad_count; qi++)
	{
		quad const &q = s_quads[qi];
		const uint32_t vbase = vcount;
		for (uint32_t i = 0; i < VERTS_PER_QUAD; i++)
		{
			gpu_vertex &v = verts[vcount++];
			v.x = q.x[i];      // sub-pixel float from the seam (M1-3); the viewport scales it to draw res
			v.y = q.y[i];
			v.col = q.col;
			v.albedo = q.albedo;   // pre-luma pen; m1.frag picks this over col when No Lighting is on
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

	const VkDeviceSize offset = 0;
	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
	s_fns.cmd_push_constants(cmd, s_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
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
}

uint32_t geom_primitive_count()
{
	return s_quad_count;
}

} // namespace m1
