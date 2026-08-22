// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco (Super) System 22 — the polygon pass (S2: untextured first).

    See s22_geom.h for the shape of the phase and s22_seam.h for the stream. This file is the record
    the seam fills on the emulation thread, plus the GPU pipeline that turns it into flat Gouraud-shaded
    triangles on the frontend's thread. It is deliberately a fraction of vk_geom.cpp: no textures, no
    colour tables, no per-polygon parameter buffer, no scissor, no depth buffer. Just position and a
    resolved colour per vertex, drawn in record order.

    Depth is a painter's algorithm: the System 22 tree is walked back-to-front, so the stream arrives
    far-to-near and the last polygon to touch a pixel is the nearest. That needs no depth test — the
    pipeline disables it and leaves the ring's depth attachment (Model 2's) alone.

    Threading mirrors m2vk_frame.cpp: the record is a single object, written on the emulation thread
    during render_scene and read on the frontend's thread from retro_run. Those never overlap — the
    emulation thread is parked on the OSD's baton for the whole of retro_run — so there is no lock.

*********************************************************************************************************************************/

#include "renderer_vk/s22_geom.h"

#include "s22_seam.h"

#include "renderer_vk/shaders/s22_vert_spv.h"
#include "renderer_vk/shaders/s22_frag_spv.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace s22 {

// vk_log, vk_result_name, find_memory_type and vk_funcs are the shared renderer's, in namespace m2vk.
using namespace m2vk;

namespace {

//============================================================
//  constants and shapes
//============================================================

// Matches vk_present's ceiling on the sync-index mask; the two are indexed by the same thing.
constexpr uint32_t MAX_SLOTS = 8;

// Where the per-slot buffers start. ridgerac peaks near 2000 quads a scene (S1's tap), so this never
// reallocates in the steady state; a frame past it grows the buffers once and they stay grown.
constexpr uint32_t INITIAL_QUAD_CAPACITY = 4096;

// A sanity bound on a count that comes from emulated hardware, and headroom for the index type.
constexpr uint32_t MAX_QUAD_CAPACITY = 1u << 21;

// A fan of up to 6 vertices; 6 verts and 4 triangles = 12 indices are the worst case per quad.
constexpr uint32_t MAX_QUAD_VERTS = 6;
constexpr uint32_t MAX_QUAD_INDICES = 12;

// 16 bytes: position (with the meaningless painter's-order z) and the resolved flat colour.
struct gpu_vertex
{
	float    x, y, z;
	uint32_t rgba;          // R8G8B8A8_UNORM, i.e. R in the low byte
};

static_assert(sizeof(gpu_vertex) == 16, "the vertex attribute offsets below are written out by hand");

// The vertex shader's push block: the visible picture's half-extent in pixels.
struct push_block
{
	float half_width, half_height;
};

static_assert(sizeof(push_block) == 8, "s22.vert's push block is two words");

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
	uint32_t      capacity = 0;     // quads the two buffers are sized for
	uint32_t      index_count = 0;  // what this slot's recorded draw will submit
};


//============================================================
//  state
//============================================================

// The record. Written on the emulation thread, read on the frontend's, never at the same time.
std::vector<quad> s_quads;
uint32_t s_quad_count = 0;
uint64_t s_serial = 0;
bool     s_valid = false;
int      s_variant = 0;

// GPU handles, stashed by geom_build and used lazily.
std::array<geom_slot, MAX_SLOTS> s_slots{};
uint32_t s_slot_count = 0;
VkRenderPass s_render_pass = VK_NULL_HANDLE;
const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

VkPipelineLayout s_pipeline_layout = VK_NULL_HANDLE;
VkPipeline       s_pipeline = VK_NULL_HANDLE;
bool s_pipeline_ready = false;
bool s_pipeline_failed = false;

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
	vk_log(RETRO_LOG_ERROR, "s22: %s failed: %s\n", what, vk_result_name(result));
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

// Device-local host-visible, exactly as vk_geom's create_buffer: unified memory, so no staging copy.
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
		vk_log(RETRO_LOG_ERROR, "s22: no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
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
		vk_log(RETRO_LOG_ERROR, "s22: a frame of %u quads is past the %u the buffers will size to\n",
				unsigned(quads), unsigned(MAX_QUAD_CAPACITY));
		return false;
	}

	const VkDeviceSize verts = VkDeviceSize(quads) * MAX_QUAD_VERTS * sizeof(gpu_vertex);
	const VkDeviceSize idx = VkDeviceSize(quads) * MAX_QUAD_INDICES * sizeof(uint32_t);

	if (!create_buffer(slot.vertices, verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vkCreateBuffer (s22 vertices)")
			|| !create_buffer(slot.indices, idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "vkCreateBuffer (s22 indices)"))
	{
		return false;
	}

	slot.capacity = quads;
	return true;
}

// The polygon's base palette colour (pens[0], 0x00RRGGBB) modulated by the per-vertex hardware
// brightness. renderscanline_poly computes shade = (bri + 0.5) and scales each channel by
// (shade << 2) / 256 = shade / 64; bri arrives here premultiplied by ooz, so bri/ooz recovers it.
uint32_t shade_color(uint32_t base, float bri_over_z, float ooz)
{
	float shade = 1.0f;
	if (ooz > 1e-9f || ooz < -1e-9f)
		shade = (bri_over_z / ooz) / 64.0f;
	if (shade < 0.0f)
		shade = 0.0f;

	auto ch = [shade](uint32_t c) -> uint32_t
	{
		int v = int(float(c) * shade + 0.5f);
		if (v > 255) v = 255;
		return uint32_t(v);
	};

	const uint32_t r = ch((base >> 16) & 0xff);
	const uint32_t g = ch((base >> 8) & 0xff);
	const uint32_t b = ch(base & 0xff);
	return r | (g << 8) | (b << 16) | 0xff000000u;
}

bool ensure_pipeline()
{
	if (s_pipeline_ready)
		return true;
	if (s_pipeline_failed || (s_device == VK_NULL_HANDLE) || (s_render_pass == VK_NULL_HANDLE))
		return false;

	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0;
	push.size = sizeof(push_block);

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 0;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout),
			"vkCreatePipelineLayout (s22)"))
	{
		s_pipeline_failed = true;
		return false;
	}

	VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &out)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule (s22)");
	};

	bool ok = make_module(S22_VERT_SPV, sizeof(S22_VERT_SPV), vert)
			&& make_module(S22_FRAG_SPV, sizeof(S22_FRAG_SPV), frag);

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
		attrs[0].location = 0;
		attrs[0].binding = 0;
		attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;   // x, y, painter's-order z
		attrs[0].offset = 0;
		attrs[1].location = 1;
		attrs[1].binding = 0;
		attrs[1].format = VK_FORMAT_R8G8B8A8_UNORM;     // resolved flat colour
		attrs[1].offset = 12;

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

		// No culling: the geometry engine's check_culling already rejected back faces and the fan's
		// winding is whatever the game's vertex order made it, exactly as the software path.
		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// Painter's order: draw in record order, last writer wins. So the depth test is off — but the
		// state is still supplied, because the ring's render pass carries a depth attachment (Model 2's)
		// and a pipeline used with it must declare depth-stencil state even when it does nothing.
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depth.depthTestEnable = VK_FALSE;
		depth.depthWriteEnable = VK_FALSE;
		depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
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
		info.layout = s_pipeline_layout;
		info.renderPass = s_render_pass;
		info.subpass = 0;

		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline),
				"vkCreateGraphicsPipelines (s22)");
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	if (!ok)
	{
		s_pipeline_failed = true;
		return false;
	}

	s_pipeline_ready = true;
	return true;
}

} // anonymous namespace


//============================================================
//  the record — emulation thread
//============================================================

void record_begin(int variant)
{
	s_variant = variant;
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

	// The pipeline and per-slot buffers are built lazily on the first upload that carries geometry, so
	// a build that never captures (the Model 2 core) makes no Vulkan call from here.
	s_pipeline_ready = false;
	s_pipeline_failed = false;
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
	s_pipeline_ready = false;
	s_pipeline_failed = false;
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
	s_pipeline_ready = false;
	s_pipeline_failed = false;
	s_render_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
	s_device = VK_NULL_HANDLE;
}

bool geom_upload(uint32_t slot_index)
{
	// Before any Vulkan work, so the Model 2 build (which never captures) returns here every frame and
	// never builds the pipeline.
	if (!s_valid || (s_quad_count == 0) || (slot_index >= s_slot_count))
		return false;
	if (!ensure_pipeline())
		return false;

	geom_slot &slot = s_slots[slot_index];
	if ((slot.capacity < s_quad_count) && !size_slot(slot, s_quad_count + (s_quad_count / 2)))
		return false;

	auto *const verts = static_cast<gpu_vertex *>(slot.vertices.mapped);
	auto *const idx = static_cast<uint32_t *>(slot.indices.mapped);
	uint32_t vcount = 0, icount = 0;

	for (uint32_t qi = 0; qi < s_quad_count; qi++)
	{
		quad const &q = s_quads[qi];
		uint32_t n = q.num_verts;
		if (n < 3)
			continue;
		if (n > MAX_QUAD_VERTS)
			n = MAX_QUAD_VERTS;

		const uint32_t base = vcount;
		for (uint32_t i = 0; i < n; i++)
		{
			gpu_vertex &v = verts[vcount++];
			v.x = q.x[i];
			v.y = q.y[i];
			v.z = 0.5f;     // painter's order: depth test is off, so this is a placeholder in [0,1]
			v.rgba = shade_color(q.basecolor, q.bri[i], q.ooz[i]);
		}
		for (uint32_t i = 1; i + 1 < n; i++)
		{
			idx[icount++] = base;
			idx[icount++] = base + i;
			idx[icount++] = base + i + 1;
		}
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
		vk_log(RETRO_LOG_INFO, "s22: first GPU geometry — %u quads, %u indices (%s)\n",
				unsigned(s_quad_count), unsigned(icount), s_variant ? "ss22" : "s22");
	}

	return icount != 0;
}

void geom_draw(uint32_t slot_index, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height)
{
	(void)draw_width;
	(void)draw_height;

	if (!s_pipeline_ready || (slot_index >= s_slot_count))
		return;

	geom_slot &slot = s_slots[slot_index];
	if (slot.index_count == 0)
		return;

	// The VISIBLE half-extent, resolution-invariant for the reason poly.vert gives: the vertex shader
	// turns bitmap pixels into NDC, so the attachment size never enters here. The caller has already set
	// the viewport and scissor to the (possibly larger) attachment extent.
	push_block push{};
	push.half_width = float(width) * 0.5f;
	push.half_height = float(height) * 0.5f;

	const VkDeviceSize offset = 0;
	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
	s_fns.cmd_push_constants(cmd, s_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
	s_fns.cmd_bind_vertex_buffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
	s_fns.cmd_bind_index_buffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
	s_fns.cmd_draw_indexed(cmd, slot.index_count, 1, 0, 0, 0);
}

void geom_end_run()
{
	if (s_reported_first)
	{
		vk_log(RETRO_LOG_INFO, "s22: run end — %llu quads over drawn frames (max/scene %u)\n",
				(unsigned long long)s_run_quads, unsigned(s_max_quads));
	}
	s_reported_first = false;
	s_run_quads = 0;
	s_max_quads = 0;
	s_drawn_serial = 0;
	s_valid = false;
	s_quad_count = 0;
}

} // namespace s22
