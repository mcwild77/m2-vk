// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 23 — the polygon pass (23-3: textured, painter's order).

    See s23_geom.h for the shape of the phase and s23_seam.h for the stream. This file is the record the
    seam fills on the emulation thread, plus the GPU pipeline that turns it into textured triangles on the
    frontend's thread. It follows s22_geom's texture handling: the ROM-derived tile system (tmrom_decoded /
    texattr_decoded / texrom) uploads once as storage buffers, the palette is per-slot and re-uploaded each
    frame, and one descriptor set per slot binds the four to the fragment's texture_lookup. It keeps
    s22_geom's per-run scissor, because System 23 games (the light-gun sets) window the 3D into a viewport
    that is not the full screen. The four push-constant masks (tileid / decoded / texrom / palette) carry
    the game's fixed array sizes to the shader.

    DEPTH IS DRAW ORDER, NOT z. The seam records in render_flush's qsorted back-to-front order, so this is
    a plain painter's pass: draw in record order, last writer wins, depth test disabled. No depth use.

    Per-frame buffers are host-visible device-local and written with no staging copy, and the record is
    turned into buffers on the frontend's thread from data the emulation thread wrote and is now parked
    against, so there is no lock — the same argument s22_geom / m1_geom / vk_geom rest on.

*********************************************************************************************************************************/

#include "renderer_vk/s23_geom.h"

#include "s23_seam.h"

#include "renderer_vk/shaders/s23_vert_spv.h"
#include "renderer_vk/shaders/s23_frag_spv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace s23 {

// vk_log, vk_result_name, find_memory_type and vk_funcs are the shared renderer's, in namespace m2vk.
using namespace m2vk;

namespace {

//============================================================
//  constants and shapes
//============================================================

// Matches vk_present's ceiling on the sync-index mask; the two are indexed by the same thing.
constexpr uint32_t MAX_SLOTS = 8;

// crszone peaks near 6000 polys a scene (the 23-1 tap); this never reallocates in the steady state.
constexpr uint32_t INITIAL_POLY_CAPACITY = 8192;

// A sanity bound on a count that comes from emulated hardware, and headroom for the index type.
constexpr uint32_t MAX_POLY_CAPACITY = 1u << 21;

// A fan of up to 6 vertices; 6 verts and 4 triangles = 12 indices are the worst case per poly.
constexpr uint32_t MAX_POLY_VERTS = 6;
constexpr uint32_t MAX_POLY_INDICES = 12;

// One polygon vertex: screen position (viewport offset baked in), the four screen-linear params the
// fragment divides (ooz, u*ooz, v*ooz, shade*ooz), and the per-poly texture/shade info. flags/tbase/
// peninfo are per-poly constants written identically to every vertex of the poly and read flat.
struct gpu_vertex
{
	float    x, y;         // offset 0
	float    ooz;          // offset 8  — driver param[0]
	float    uoz, voz;     // offset 12 — driver param[1], param[2]
	float    ish;          // offset 20 — driver param[3]
	uint32_t flags;        // offset 24 — bit0 shade, bit1 stencil, bit2 poly-fade, bit3 colour-fade, bit4 blend, bit5 poly-alpha
	uint32_t tbase;        // offset 28 — texture base added to the recovered v
	uint32_t peninfo;      // offset 32 — pens_base | (cmode << 20)
	uint32_t pfade;        // offset 36 — poly-fade: polycolor_r | g<<8 | b<<16
	uint32_t cfade;        // offset 40 — colour-fade: fadefactor | fadecolor_r<<8 | g<<16 | b<<24
	uint32_t ablend;       // offset 44 — poly-alpha: alpha | alpha_pen<<8 | alpha_enabled<<16
};

static_assert(sizeof(gpu_vertex) == 48, "the vertex attribute offsets below are written out by hand");

constexpr uint32_t FLAG_SHADE     = 1u;
constexpr uint32_t FLAG_STENCIL   = 2u;
constexpr uint32_t FLAG_PFADE     = 4u;
constexpr uint32_t FLAG_COLORFADE = 8u;
constexpr uint32_t FLAG_BLEND     = 16u;   // fixed 50% blend (render_hash bit1)
constexpr uint32_t FLAG_POLYALPHA = 32u;   // src*alpha + dst*inv, per-texel gated (render_hash bit0)

// The push block, shared by s23.vert and s23.frag: half_size belongs to the vertex shader, the four
// masks to the fragment's texture fetch. Kept in sync with the layout in both shader sources.
struct push_block
{
	float    half_width, half_height;
	uint32_t tileid_mask;
	uint32_t decoded_mask;    // decoded_count - 1
	uint32_t texrom_mask;     // texrom_bytes - 1
	uint32_t pal_mask;        // palette_count - 1
};

static_assert(sizeof(push_block) == 24, "s23 push block is two floats and four masks");

struct mapped_buffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void          *mapped = nullptr;
	VkDeviceSize   size = 0;
};

// One indexed draw: a run of consecutive polys that share a viewport window. Same idea as s22_geom's
// draw_batch; the rectangle is inclusive in visible (640x480) bitmap pixels, scaled to the attachment in
// geom_draw.
struct draw_batch
{
	int16_t  left, top, right, bottom;
	uint32_t first_index;
	uint32_t index_count;
};

struct geom_slot
{
	mapped_buffer   vertices;
	mapped_buffer   indices;
	mapped_buffer   palette;               // 128 KB, re-uploaded each frame (its contents change)
	mapped_buffer   texram;                // 256 KB C412 sram, re-uploaded each frame (stencil, 23-4)
	VkDescriptorSet descriptor = VK_NULL_HANDLE;
	uint32_t        capacity = 0;      // polys the vertex/index buffers are sized for
	uint32_t        index_count = 0;   // what this slot's recorded draw will submit
	std::vector<draw_batch> batches;   // one draw per viewport-window run; host-side, grows on its own
};

// The palette is 0x8000 pens (PALETTE(config, m_palette).set_entries(0x8000) in namcos23.cpp); the shader
// masks every index to it. Fixed size, so the per-slot buffer never resizes.
constexpr VkDeviceSize PALETTE_BYTES = 0x8000 * sizeof(uint32_t);

// The C412 sram is 0x20000 u16 (256 KB). Bound as a u32 storage buffer; the shader loads the u16 out of
// the word. Fixed size, so the per-slot buffer never resizes; re-uploaded each frame (live RAM).
constexpr VkDeviceSize TEXRAM_BYTES = 0x20000 * sizeof(uint16_t);


//============================================================
//  state
//============================================================

// The record. Written on the emulation thread, read on the frontend's, never at the same time.
std::vector<poly> s_polys;
uint32_t s_poly_count = 0;
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
bool s_ready = false;
bool s_failed = false;

// The texture system. tmrom_decoded / texattr / texrom are ROM-derived and stable, so they upload once
// (upload_static); the palette is per-slot and re-uploaded each frame. The descriptor set layout and
// pool serve all slots. The four push-constant masks are cached from the texture_rom on first upload.
VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
mapped_buffer s_tmrom;      // uint32 per tileid (texrom base, already <<8)
mapped_buffer s_texattr;    // uint8 per tileid (orientation), packed
mapped_buffer s_texrom;     // uint8 texels, packed
bool     s_static_uploaded = false;
uint32_t s_tileid_mask = 0;
uint32_t s_decoded_mask = 0;
uint32_t s_texrom_mask = 0;
uint32_t s_pal_mask = 0x7fff;

// Reporting, once per run.
bool     s_reported_first = false;
uint64_t s_run_polys = 0;
uint32_t s_max_polys = 0;
uint64_t s_drawn_serial = 0;

// M2VK_NO_SCISSOR=1 collapses every poly's viewport window to full-screen — one run for the frame. The
// same attribution switch s22_geom / vk_geom expose.
bool s_no_scissor = false;
bool s_no_scissor_known = false;

bool no_scissor()
{
	if (!s_no_scissor_known)
	{
		s_no_scissor = (std::getenv("M2VK_NO_SCISSOR") != nullptr);
		s_no_scissor_known = true;
	}
	return s_no_scissor;
}


//============================================================
//  helpers
//============================================================

bool check(VkResult result, char const *what)
{
	if (result == VK_SUCCESS)
		return true;
	vk_log(RETRO_LOG_ERROR, "s23: %s failed: %s\n", what, vk_result_name(result));
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

// Device-local host-visible, exactly as s22_geom / m1_geom: unified memory, so no staging copy.
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
		vk_log(RETRO_LOG_ERROR, "s23: no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
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
			vk_log(RETRO_LOG_WARN, "s23: device-local host-visible heap could not hold a %llu byte buffer (%s); "
					"falling back to plain host-visible memory\n", (unsigned long long)reqs.size, what);
		alloc.memoryTypeIndex = fallback_type;
		ar = s_fns.allocate_memory(s_device, &alloc, nullptr, &b.memory);
	}
	if (ar != VK_SUCCESS)
	{
		vk_log(RETRO_LOG_ERROR, "s23: vkAllocateMemory failed for a %llu byte buffer (%s): %d\n",
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

bool size_slot(geom_slot &slot, uint32_t polys)
{
	if (polys > MAX_POLY_CAPACITY)
	{
		vk_log(RETRO_LOG_ERROR, "s23: a frame of %u polys is past the %u the buffers will size to\n",
				unsigned(polys), unsigned(MAX_POLY_CAPACITY));
		return false;
	}

	const VkDeviceSize verts = VkDeviceSize(polys) * MAX_POLY_VERTS * sizeof(gpu_vertex);
	const VkDeviceSize idx = VkDeviceSize(polys) * MAX_POLY_INDICES * sizeof(uint32_t);

	if (!create_buffer(slot.vertices, verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vkCreateBuffer (s23 vertices)")
			|| !create_buffer(slot.indices, idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "vkCreateBuffer (s23 indices)"))
	{
		return false;
	}

	slot.capacity = polys;
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
	layout_info.setLayoutCount = 1;              // the texture system's four storage buffers
	layout_info.pSetLayouts = &s_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout),
			"vkCreatePipelineLayout (s23)"))
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
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule (s23)");
	};

	bool ok = make_module(S23_VERT_SPV, sizeof(S23_VERT_SPV), vert)
			&& make_module(S23_FRAG_SPV, sizeof(S23_FRAG_SPV), frag);

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

		VkVertexInputAttributeDescription attrs[9]{};
		attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT;    attrs[0].offset = 0;
		attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 8;
		attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32_SFLOAT;       attrs[2].offset = 20;
		attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32_UINT;         attrs[3].offset = 24;
		attrs[4].location = 4; attrs[4].binding = 0; attrs[4].format = VK_FORMAT_R32_UINT;         attrs[4].offset = 28;
		attrs[5].location = 5; attrs[5].binding = 0; attrs[5].format = VK_FORMAT_R32_UINT;         attrs[5].offset = 32;
		attrs[6].location = 6; attrs[6].binding = 0; attrs[6].format = VK_FORMAT_R32_UINT;         attrs[6].offset = 36;
		attrs[7].location = 7; attrs[7].binding = 0; attrs[7].format = VK_FORMAT_R32_UINT;         attrs[7].offset = 40;
		attrs[8].location = 8; attrs[8].binding = 0; attrs[8].format = VK_FORMAT_R32_UINT;         attrs[8].offset = 44;

		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = 9;
		vertex_input.pVertexAttributeDescriptions = attrs;

		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewport_state{};
		viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_state.viewportCount = 1;
		viewport_state.scissorCount = 1;

		// No culling: render_flush draws whatever winding the fan has, so does this.
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

		// 23-4 blend + poly-alpha. The painter's pass is already back-to-front (the seam records in
		// render_flush's qsorted order), so fixed-function over-blend reproduces render_scanline's dst read
		// for free — no self-referencing input attachment, no deferred pass, and it is free on the tile GPUs
		// this core targets (blend runs in tile memory). The fragment emits a per-pixel weight a in [0,1] as
		// out_color.a (a = alpha/256 where the poly-alpha gate passes, 0.5 for blend, 1.0 opaque), and the
		// blend unit computes src*a + dst*(1-a): SRC_ALPHA / ONE_MINUS_SRC_ALPHA. An opaque fragment (a=1)
		// yields src exactly, so this one always-on pipeline serves opaque and translucent alike — no
		// per-poly pipeline switch. The only divergence from software's integer (x*a + dx*inv)>>8 is unorm
		// rounding (<1 LSB). Alpha channel is NOT written (RGB mask): the 3D over-pass must not clobber the
		// target's alpha with a fractional weight — it leaves alpha as the background pass established it.
		VkPipelineColorBlendAttachmentState blend_attachment{};
		blend_attachment.blendEnable = VK_TRUE;
		blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
		blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
				| VK_COLOR_COMPONENT_B_BIT;

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
				"vkCreateGraphicsPipelines (s23)");
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}

// Five storage buffers, all read only in the fragment shader: tmrom_decoded, texattr, texrom (the three
// static ROM-derived arrays), the per-slot palette, and the per-slot C412 sram (stencil, 23-4).
bool build_descriptor_layout()
{
	VkDescriptorSetLayoutBinding bindings[5]{};
	for (uint32_t i = 0; i < 5; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 5;
	layout_info.pBindings = bindings;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &layout_info, nullptr, &s_set_layout),
			"vkCreateDescriptorSetLayout (s23)"))
	{
		return false;
	}

	VkDescriptorPoolSize size{};
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = s_slot_count * 5;

	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = s_slot_count;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	return check(s_fns.create_descriptor_pool(s_device, &pool_info, nullptr, &s_descriptor_pool),
			"vkCreateDescriptorPool (s23)");
}

// Points a slot's descriptor set at the three shared static buffers and its own palette buffer. Written
// once, after everything exists — the static buffers never move and the palette buffer is fixed size.
void write_descriptor(geom_slot &slot)
{
	VkBuffer const buffers[5] = { s_tmrom.buffer, s_texattr.buffer, s_texrom.buffer,
			slot.palette.buffer, slot.texram.buffer };

	VkDescriptorBufferInfo info[5]{};
	VkWriteDescriptorSet write[5]{};
	for (uint32_t i = 0; i < 5; i++)
	{
		info[i].buffer = buffers[i];
		info[i].offset = 0;
		info[i].range = VK_WHOLE_SIZE;

		write[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write[i].dstSet = slot.descriptor;
		write[i].dstBinding = i;
		write[i].descriptorCount = 1;
		write[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write[i].pBufferInfo = &info[i];
	}
	s_fns.update_descriptor_sets(s_device, 5, write, 0, nullptr);
}

// Uploads the three ROM-derived buffers once and caches the four push-constant masks. The arrays are
// byte-for-byte the driver's, so each is a plain memcpy; the unpack lives in the shader. The decoded
// arrays and texrom are sized from the region, so a game whose textile ROM is short still copies exactly
// its real bytes (the s22 fix) — decoded_count and texrom_bytes are both derived from the real region.
bool upload_static()
{
	if (s_static_uploaded)
		return true;

	texture_rom const &t = get_texture_rom();
	if ((t.tmrom_decoded == nullptr) || (t.texattr_decoded == nullptr) || (t.texrom == nullptr)
			|| (t.decoded_count == 0) || (t.texrom_bytes == 0))
	{
		return false;
	}

	const VkDeviceSize tmrom_bytes = VkDeviceSize(t.decoded_count) * sizeof(uint32_t);
	// texattr and texrom are byte arrays; round the buffer up to a word so the shader's word loads stay
	// in bounds when the count is not a multiple of four.
	const VkDeviceSize texattr_bytes = (VkDeviceSize(t.decoded_count) + 3u) & ~VkDeviceSize(3);
	const VkDeviceSize texrom_bytes = (VkDeviceSize(t.texrom_bytes) + 3u) & ~VkDeviceSize(3);

	if (!create_buffer(s_tmrom, tmrom_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s23 tmrom)")
			|| !create_buffer(s_texattr, texattr_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s23 texattr)")
			|| !create_buffer(s_texrom, texrom_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s23 texrom)"))
	{
		return false;
	}

	std::memcpy(s_tmrom.mapped, t.tmrom_decoded, size_t(t.decoded_count) * sizeof(uint32_t));
	std::memcpy(s_texattr.mapped, t.texattr_decoded, size_t(t.decoded_count));
	if (texattr_bytes > VkDeviceSize(t.decoded_count))
		std::memset(static_cast<uint8_t *>(s_texattr.mapped) + t.decoded_count, 0,
				size_t(texattr_bytes - t.decoded_count));
	std::memcpy(s_texrom.mapped, t.texrom, size_t(t.texrom_bytes));
	if (texrom_bytes > VkDeviceSize(t.texrom_bytes))
		std::memset(static_cast<uint8_t *>(s_texrom.mapped) + t.texrom_bytes, 0,
				size_t(texrom_bytes - t.texrom_bytes));

	// Caches for the push constant. decoded_count and texrom_bytes are powers of two (both derive from a
	// ROM_REGION size), so count-1 is a valid AND-mask; palette is 0x8000.
	s_tileid_mask  = t.tileid_mask;
	s_decoded_mask = t.decoded_count - 1;
	s_texrom_mask  = t.texrom_bytes - 1;
	s_pal_mask     = (t.palette_count ? t.palette_count : 0x8000) - 1;

	s_static_uploaded = true;
	return true;
}

// The pipeline and the texture system, built once on the first captured frame. A build that never
// captures System 23 never reaches here, so it makes no Vulkan call from this file.
bool ensure_ready()
{
	if (s_ready)
		return true;
	if (s_failed || (s_device == VK_NULL_HANDLE) || (s_render_pass == VK_NULL_HANDLE))
		return false;

	if (!build_descriptor_layout() || !build_pipeline() || !upload_static())
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
				"vkAllocateDescriptorSets (s23)"))
		{
			s_failed = true;
			return false;
		}

		if (!create_buffer(slot.palette, PALETTE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s23 palette)")
				|| !create_buffer(slot.texram, TEXRAM_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s23 texram)"))
		{
			s_failed = true;
			return false;
		}

		write_descriptor(slot);
	}

	s_ready = true;
	return true;
}

} // anonymous namespace


//============================================================
//  the record — emulation thread
//============================================================

void record_begin(int variant)
{
	s_variant = variant;
	s_poly_count = 0;
	s_valid = false;
}

void record_poly(poly const &p)
{
	if (s_poly_count == s_polys.size())
		s_polys.resize(s_polys.size() + (s_polys.size() / 2) + INITIAL_POLY_CAPACITY);
	s_polys[s_poly_count++] = p;
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
	s_static_uploaded = false;
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	for (geom_slot &slot : s_slots)
	{
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;
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
		destroy_buffer(slot.texram);
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;   // freed with the pool below
	}

	destroy_buffer(s_tmrom);
	destroy_buffer(s_texattr);
	destroy_buffer(s_texrom);

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
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_static_uploaded = false;
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
		slot.texram = mapped_buffer{};
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;
	}
	s_tmrom = mapped_buffer{};
	s_texattr = mapped_buffer{};
	s_texrom = mapped_buffer{};
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_static_uploaded = false;
	s_ready = false;
	s_failed = false;
	s_render_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
	s_device = VK_NULL_HANDLE;
}

bool geom_upload(uint32_t slot_index)
{
	// Before any Vulkan work, so a build that never captures System 23 returns here every frame and never
	// builds anything.
	if (!s_valid || (s_poly_count == 0) || (slot_index >= s_slot_count))
		return false;
	if (!ensure_ready())
		return false;

	geom_slot &slot = s_slots[slot_index];
	if ((slot.capacity < s_poly_count) && !size_slot(slot, s_poly_count + (s_poly_count / 2)))
		return false;

	// The palette is the one part of the texture system that changes per frame; re-upload the live pens.
	// (The ROM-derived buffers were uploaded once in ensure_ready.)
	// The palette and the C412 sram (stencil) are the two parts of the texture system that change per frame.
	if (texture_rom const &t = get_texture_rom(); t.palette != nullptr)
	{
		const size_t have = std::min<size_t>(size_t(t.palette_count) * sizeof(uint32_t), size_t(PALETTE_BYTES));
		std::memcpy(slot.palette.mapped, t.palette, have);
		if (t.texram != nullptr)
		{
			const size_t tr = std::min<size_t>(size_t(t.texram_count) * sizeof(uint16_t), size_t(TEXRAM_BYTES));
			std::memcpy(slot.texram.mapped, t.texram, tr);
		}
	}

	auto *const verts = static_cast<gpu_vertex *>(slot.vertices.mapped);
	auto *const idx = static_cast<uint32_t *>(slot.indices.mapped);
	uint32_t vcount = 0, icount = 0;
	slot.batches.clear();

	// Walk the sorted (draw) order and fan each poly into triangles. sprites and degenerate entries are
	// skipped (sprites are 2D, out of scope until 23-6). The viewport window is baked into the position and
	// carried as the batch scissor — render_scanline draws pv at (pv.x + clip_left, pv.y + clip_top) and
	// clips to [clip_left,clip_right) x [clip_top,clip_bottom).
	for (uint32_t pi = 0; pi < s_poly_count; pi++)
	{
		poly const &p = s_polys[pi];
		if (p.sprite)
			continue;
		uint32_t nv = p.num_verts;
		if (nv < 3)
			continue;
		if (nv > MAX_POLY_VERTS)
			nv = MAX_POLY_VERTS;

		// render_scanline's per-poly viewport window (clip_left/right/top/bottom), the offset it blits at.
		const int off_x = (320 - p.vp_size_x) + p.vp_offset_x;   // clip_left
		const int off_y = (240 - p.vp_size_y) - p.vp_offset_y;   // clip_top
		const int clip_r = (320 + p.vp_size_x) + p.vp_offset_x;  // clip_right (exclusive)
		const int clip_b = (240 + p.vp_size_y) - p.vp_offset_y;  // clip_bottom (exclusive)

		// The batch rectangle, inclusive in visible pixels, clamped to the 640x480 picture. Full-screen
		// under the attribution switch. clip_right/bottom are exclusive, so the inclusive edge is -1.
		int16_t cl, ct, cr, cb;
		if (no_scissor())
		{
			cl = 0; ct = 0; cr = 639; cb = 479;
		}
		else
		{
			cl = int16_t(off_x < 0 ? 0 : (off_x > 639 ? 639 : off_x));
			ct = int16_t(off_y < 0 ? 0 : (off_y > 479 ? 479 : off_y));
			const int r = clip_r - 1, b = clip_b - 1;
			cr = int16_t(r > 639 ? 639 : (r < 0 ? 0 : r));
			cb = int16_t(b > 479 ? 479 : (b < 0 ? 0 : b));
		}

		// Start a new run whenever the window changes; the walk is in draw order, so a run is a maximal
		// span of consecutive polys sharing a scissor.
		if (slot.batches.empty()
				|| (slot.batches.back().left != cl) || (slot.batches.back().top != ct)
				|| (slot.batches.back().right != cr) || (slot.batches.back().bottom != cb))
		{
			slot.batches.push_back(draw_batch{ cl, ct, cr, cb, icount, 0 });
		}

		const uint32_t flags = (p.shade_enabled ? FLAG_SHADE : 0u)
				| (p.stencil_enabled ? FLAG_STENCIL : 0u)
				| (p.pfade_enabled ? FLAG_PFADE : 0u)
				| (p.colorfade ? FLAG_COLORFADE : 0u)
				| (p.blend_enabled ? FLAG_BLEND : 0u)
				| (p.poly_alpha ? FLAG_POLYALPHA : 0u);
		const uint32_t tbase = uint32_t(p.tbase);
		const uint32_t peninfo = (p.pens_base & 0xfffffu) | (uint32_t(p.cmode) << 20);
		const uint32_t pfade = uint32_t(p.polycolor_r) | (uint32_t(p.polycolor_g) << 8)
				| (uint32_t(p.polycolor_b) << 16);
		const uint32_t cfade = uint32_t(p.fadefactor) | (uint32_t(p.fadecolor_r) << 8)
				| (uint32_t(p.fadecolor_g) << 16) | (uint32_t(p.fadecolor_b) << 24);
		const uint32_t ablend = uint32_t(p.alpha) | (uint32_t(p.alpha_pen) << 8)
				| (p.alpha_enabled ? (1u << 16) : 0u);
		const uint32_t vbase = vcount;
		for (uint32_t i = 0; i < nv; i++)
		{
			gpu_vertex &v = verts[vcount++];
			v.x = p.x[i] + float(off_x);
			v.y = p.y[i] + float(off_y);
			v.ooz = p.p0[i];
			v.uoz = p.uoz[i];
			v.voz = p.voz[i];
			v.ish = p.ish[i];
			v.flags = flags;
			v.tbase = tbase;
			v.peninfo = peninfo;
			v.pfade = pfade;
			v.cfade = cfade;
			v.ablend = ablend;
		}
		for (uint32_t i = 1; i + 1 < nv; i++)
		{
			idx[icount++] = vbase;
			idx[icount++] = vbase + i;
			idx[icount++] = vbase + i + 1;
		}

		slot.batches.back().index_count = icount - slot.batches.back().first_index;
	}

	slot.index_count = icount;

	// Stats and the once-per-run first line.
	if (s_serial != s_drawn_serial)
	{
		s_drawn_serial = s_serial;
		s_run_polys += s_poly_count;
		if (s_poly_count > s_max_polys)
			s_max_polys = s_poly_count;
	}
	if (!s_reported_first && (icount != 0))
	{
		s_reported_first = true;
		vk_log(RETRO_LOG_INFO, "s23: first GPU geometry — %u polys, %u indices, %u batches (variant %d)\n",
				unsigned(s_poly_count), unsigned(icount), unsigned(slot.batches.size()), s_variant);
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
	push.tileid_mask = s_tileid_mask;
	push.decoded_mask = s_decoded_mask;
	push.texrom_mask = s_texrom_mask;
	push.pal_mask = s_pal_mask;

	const VkDeviceSize offset = 0;
	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
			0, 1, &slot.descriptor, 0, nullptr);
	s_fns.cmd_push_constants(cmd, s_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s_fns.cmd_bind_vertex_buffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
	s_fns.cmd_bind_index_buffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);

	// One draw per viewport-window run, each with its own scissor. The rectangle is inclusive in visible
	// (640x480) pixels; scale it to the (possibly larger) attachment the same way s22_geom does, rounding
	// outward so a fractional scale never shaves a boundary column.
	const float scale_x = float(draw_width) / float(width);
	const float scale_y = float(draw_height) / float(height);

	VkRect2D set_to{};
	bool     scissor_set = false;
	for (draw_batch const &b : slot.batches)
	{
		if (b.index_count == 0)
			continue;

		const int32_t x0 = (b.left < 0) ? 0 : int32_t(std::floor(float(b.left) * scale_x));
		const int32_t y0 = (b.top < 0) ? 0 : int32_t(std::floor(float(b.top) * scale_y));
		const double right_f = (double(b.right) + 1.0) * double(scale_x);
		const double bottom_f = (double(b.bottom) + 1.0) * double(scale_y);
		const int32_t x1 = (right_f >= double(draw_width)) ? int32_t(draw_width) - 1
				: int32_t(std::ceil(right_f)) - 1;
		const int32_t y1 = (bottom_f >= double(draw_height)) ? int32_t(draw_height) - 1
				: int32_t(std::ceil(bottom_f)) - 1;
		if ((x1 < x0) || (y1 < y0))
			continue;

		VkRect2D rect{};
		rect.offset = { x0, y0 };
		rect.extent = { uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1) };
		if (!scissor_set || (rect.offset.x != set_to.offset.x) || (rect.offset.y != set_to.offset.y)
				|| (rect.extent.width != set_to.extent.width) || (rect.extent.height != set_to.extent.height))
		{
			s_fns.cmd_set_scissor(cmd, 0, 1, &rect);
			set_to = rect;
			scissor_set = true;
		}

		s_fns.cmd_draw_indexed(cmd, b.index_count, 1, b.first_index, 0, 0);
	}

	// Put the full extent back so a leftover viewport window does not clip the overlays drawn after.
	VkRect2D full{};
	full.offset = { 0, 0 };
	full.extent = { draw_width, draw_height };
	s_fns.cmd_set_scissor(cmd, 0, 1, &full);
}

void geom_end_run()
{
	if (s_reported_first)
	{
		vk_log(RETRO_LOG_INFO, "s23: run end — %llu polys over drawn frames (max/scene %u)\n",
				(unsigned long long)s_run_polys, unsigned(s_max_polys));
	}
	s_reported_first = false;
	s_run_polys = 0;
	s_max_polys = 0;
	s_drawn_serial = 0;
	s_valid = false;
	s_poly_count = 0;
}

uint32_t geom_primitive_count()
{
	return s_poly_count;
}

} // namespace s23
