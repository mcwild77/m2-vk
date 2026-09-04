// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 21 — the polygon pass (T2), composited in PEN-index space (option B).

    See s21_geom.h for the shape of the phase and s21_seam.h for the stream and the pen-space rationale.
    This file is the record the seam fills on the emulation thread, plus the GPU pipeline that turns it —
    together with the 2D-under pens, the layer-0 mix and the OVER band the driver hands over — into the
    finished RGB frame on the frontend's thread. The whole S21 frame is composited as palette pen
    indices, in a private R16_UINT render pass, and resolved to RGB once at the end (the finish pass, in
    the shared present pass) so the C355 palette-shadow OVER sprites can index the polygon-blend banks by
    the pen beneath them.

    Five GPU steps, four pipelines:

      * s_under_pipeline (fullscreen)  — lays the 2D-under pens into the pen attachment.
      * s_geom_pipeline (quads)        — the 3D, depth-tested (S21 z-buffers in hardware: clear 0.0,
                                         COMPARE_GREATER, write; s21.vert maps zsort to z).
      * s_mix_pipeline (fullscreen)    — the layer-0 C355 z-mix, gl_FragDepth thresholded against the
                                         same depth (GREATER_OR_EQUAL, no write).
      * s_finish_pipeline (fullscreen) — samples the composited pen, applies the OVER band (opaque pens,
                                         and the palette-shadow banks against the composited pen),
                                         resolves the final pen through the CLUT to RGB.

    The first three run in the private pen pass (pen_pass); the finish runs in the shared present pass
    (finish_draw), in place of the UNDER background draw. Per-frame host-visible device-local buffers are
    written with no staging copy; the pen/depth images are device-local, sized to the draw extent.

*********************************************************************************************************************************/

#include "renderer_vk/s21_geom.h"

#include "s21_seam.h"

#include "renderer_vk/shaders/fullscreen_vert_spv.h"
#include "renderer_vk/shaders/s21_vert_spv.h"
#include "renderer_vk/shaders/s21_pen_geom_frag_spv.h"
#include "renderer_vk/shaders/s21_pen_under_frag_spv.h"
#include "renderer_vk/shaders/s21_pen_mix_frag_spv.h"
#include "renderer_vk/shaders/s21_finish_frag_spv.h"

#include "renderer_vk/vk_geom.h"     // GEOM_DEPTH_FORMAT — the same depth format the shared pass uses

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

constexpr uint32_t MAX_SLOTS = 8;

// starblad peaks near 800 quads a frame (T1's tap capped at 763), so this rarely reallocates.
constexpr uint32_t INITIAL_QUAD_CAPACITY = 2048;

// A sanity bound on a count that comes from emulated hardware, and headroom for the index type.
constexpr uint32_t MAX_QUAD_CAPACITY = 1u << 21;

// A sanity bound on a pixel-buffer count — well past any real screen size.
constexpr uint32_t MAX_PIXEL_CAPACITY = 1u << 24;

// A true quad: 4 vertices, 2 triangles = 6 indices.
constexpr uint32_t QUAD_VERTS = 4;
constexpr uint32_t QUAD_INDICES = 6;

// The palette CLUT. Namco S21 palette is at most 0x8000 pens; sized to that and re-uploaded each frame.
constexpr VkDeviceSize PALETTE_BYTES = 0x8000 * sizeof(uint32_t);

// The pen attachment: one 16-bit palette index per pixel. Every S21 pen (and every shadow-composited
// pen, 0x4000|... / 0x6000|...) is <= 0x7fff, so R16_UINT holds them all.
constexpr VkFormat PEN_FORMAT = VK_FORMAT_R16_UINT;

// 16 bytes: screen x/y, the NDC depth z (mapped from zsort on the CPU), the flat pen.
struct gpu_vertex
{
	float    x, y, z;
	uint32_t pen;
};

static_assert(sizeof(gpu_vertex) == 16, "the vertex attribute offsets below are written out by hand");

// s21.vert's push: the visible picture's half-extent in pixels (NATIVE, not the draw extent — the quads
// are in native framebuffer pixels and the viewport scales NDC to the draw extent).
struct geom_push
{
	float half_width, half_height;
};

static_assert(sizeof(geom_push) == 8, "s21 geom push block is two floats");

// The fragment passes' push: a source buffer's native dimensions, so the fullscreen v_uv maps back to a
// buffer index regardless of the attachment's own pixel size.
struct dims_push
{
	uint32_t width, height;
};

static_assert(sizeof(dims_push) == 8, "s21 dims push block is two uints");

// The finish pass's push: the OVER buffer dims plus the Winning Run backdrop-sentinel shadow override
// (see s21_finish.frag). shadow_enable is 0 for the C67 games, so their resolve is unchanged.
struct finish_push
{
	uint32_t width, height;
	uint32_t shadow_enable;
	uint32_t sentinel;
	uint32_t opaque_base;
};

static_assert(sizeof(finish_push) == 20, "s21 finish push block is five uints");

struct mapped_buffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void          *mapped = nullptr;
	VkDeviceSize   size = 0;
};

struct image_target
{
	VkImage        image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView    view = VK_NULL_HANDLE;
};

struct geom_slot
{
	// The 3D geometry.
	mapped_buffer   vertices;
	mapped_buffer   indices;
	uint32_t        capacity = 0;          // quads the vertex/index buffers are sized for
	uint32_t        index_count = 0;       // 0 means no 3D this frame

	// The CLUT, re-uploaded each frame; bound to the finish set.
	mapped_buffer   palette;

	// The captured layers, in pen space. Each sized to whatever the last capture needed (native).
	mapped_buffer   under_buffer;          // 2D-under pens
	mapped_buffer   over_buffer;           // OVER band, tag + pen
	mapped_buffer   mix_buffer;            // layer-0 z-mix, tag + pen
	uint32_t        under_cap = 0, over_cap = 0, mix_cap = 0;
	uint32_t        under_w = 0, under_h = 0;   // native visible dims (also the geom half-extent source)
	uint32_t        over_w = 0, over_h = 0;
	uint32_t        mix_w = 0, mix_h = 0;       // 0 means pen_pass skips the mix draw this frame

	// The private pen render target, sized to the draw extent; rebuilt when that changes.
	image_target    pen;
	image_target    depth;
	VkFramebuffer   framebuffer = VK_NULL_HANDLE;
	uint32_t        pen_w = 0, pen_h = 0;

	// Descriptor sets: under and mix each bind one storage buffer (layout A); finish binds the pen image
	// sampler + the over buffer + the palette (layout B).
	VkDescriptorSet under_set = VK_NULL_HANDLE;
	VkDescriptorSet mix_set = VK_NULL_HANDLE;
	VkDescriptorSet finish_set = VK_NULL_HANDLE;
};


//============================================================
//  state
//============================================================

// The record, DOUBLE-BUFFERED to mirror the hardware's double-buffered poly framebuffer (the seam fires
// frame_end()+frame_begin() back-to-back at the single swap site). The emulation thread fills the WORK
// buffer, record_end swaps it into the VISIBLE buffer, and the frontend reads the VISIBLE one.
std::vector<quad> s_quads;
uint32_t s_quad_count = 0;
std::vector<quad> s_quads_work;
uint32_t s_quad_work_count = 0;
uint64_t s_serial = 0;
bool     s_valid = false;

// GPU handles, stashed by geom_build and used lazily.
std::array<geom_slot, MAX_SLOTS> s_slots{};
uint32_t s_slot_count = 0;
VkRenderPass s_present_pass = VK_NULL_HANDLE;   // the shared pass finish_draw runs in
VkRenderPass s_pen_pass = VK_NULL_HANDLE;       // this file's private R16_UINT pass
const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

VkDescriptorSetLayout s_set_layout_a = VK_NULL_HANDLE;   // one storage buffer (under, mix)
VkDescriptorSetLayout s_set_layout_b = VK_NULL_HANDLE;   // sampler + two storage buffers (finish)
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
VkSampler             s_pen_sampler = VK_NULL_HANDLE;    // nearest, for the finish pass's pen texelFetch

VkPipelineLayout s_under_layout = VK_NULL_HANDLE;
VkPipelineLayout s_geom_layout = VK_NULL_HANDLE;
VkPipelineLayout s_mix_layout = VK_NULL_HANDLE;
VkPipelineLayout s_finish_layout = VK_NULL_HANDLE;
VkPipeline s_under_pipeline = VK_NULL_HANDLE;
VkPipeline s_geom_pipeline = VK_NULL_HANDLE;
VkPipeline s_mix_pipeline = VK_NULL_HANDLE;
VkPipeline s_finish_pipeline = VK_NULL_HANDLE;

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
		vk_log(RETRO_LOG_ERROR, "s21: no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
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
			vk_log(RETRO_LOG_WARN, "s21: device-local host-visible heap could not hold a %llu byte buffer (%s); "
					"falling back to plain host-visible memory\n", (unsigned long long)reqs.size, what);
		alloc.memoryTypeIndex = fallback_type;
		ar = s_fns.allocate_memory(s_device, &alloc, nullptr, &b.memory);
	}
	if (ar != VK_SUCCESS)
	{
		vk_log(RETRO_LOG_ERROR, "s21: vkAllocateMemory failed for a %llu byte buffer (%s): %d\n",
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

void destroy_image(image_target &t)
{
	if (t.view != VK_NULL_HANDLE)
		s_fns.destroy_image_view(s_device, t.view, nullptr);
	if (t.image != VK_NULL_HANDLE)
		s_fns.destroy_image(s_device, t.image, nullptr);
	if (t.memory != VK_NULL_HANDLE)
		s_fns.free_memory(s_device, t.memory, nullptr);
	t = image_target{};
}

// A device-local render-target image + view. usage/aspect pick colour vs depth.
bool create_image(image_target &t, uint32_t width, uint32_t height, VkFormat format,
		VkImageUsageFlags usage, VkImageAspectFlags aspect, char const *what)
{
	destroy_image(t);

	VkImageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = { width, height, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!check(s_fns.create_image(s_device, &info, nullptr, &t.image), what))
		return false;

	VkMemoryRequirements reqs{};
	s_fns.get_image_memory_requirements(s_device, t.image, &reqs);

	uint32_t type_index = 0;
	if (!find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index))
	{
		vk_log(RETRO_LOG_ERROR, "s21: no device-local memory type for %s\n", what);
		return false;
	}

	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = type_index;
	if (!check(s_fns.allocate_memory(s_device, &alloc, nullptr, &t.memory), "vkAllocateMemory (s21 image)"))
		return false;
	if (!check(s_fns.bind_image_memory(s_device, t.image, t.memory, 0), "vkBindImageMemory (s21 image)"))
		return false;

	VkImageViewCreateInfo view{};
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = t.image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = { aspect, 0, 1, 0, 1 };
	return check(s_fns.create_image_view(s_device, &view, nullptr, &t.view), "vkCreateImageView (s21 image)");
}

void write_storage_descriptor(VkDescriptorSet set, uint32_t binding, VkBuffer buffer)
{
	VkDescriptorBufferInfo info{};
	info.buffer = buffer;
	info.offset = 0;
	info.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = binding;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &info;
	s_fns.update_descriptor_sets(s_device, 1, &write, 0, nullptr);
}

void write_pen_descriptor(VkDescriptorSet set, VkImageView view)
{
	VkDescriptorImageInfo info{};
	info.sampler = s_pen_sampler;
	info.imageView = view;
	info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	s_fns.update_descriptor_sets(s_device, 1, &write, 0, nullptr);
}

VkShaderModule make_module(uint32_t const *code, size_t bytes)
{
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = bytes;
	info.pCode = code;
	VkShaderModule out = VK_NULL_HANDLE;
	if (!check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule (s21)"))
		return VK_NULL_HANDLE;
	return out;
}

// One graphics pipeline. has_vertex_input toggles the s21_vert geometry attributes; the depth args pick
// the mode (off for under/finish, GREATER+write for geom, GREATER_OR_EQUAL no-write for mix).
bool make_pipeline(VkShaderModule vert, VkShaderModule frag, VkPipelineLayout layout, VkRenderPass pass,
		bool has_vertex_input, bool depth_test, bool depth_write, VkCompareOp depth_op, VkPipeline &out)
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
	if (has_vertex_input)
	{
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = 2;
		vertex_input.pVertexAttributeDescriptions = attrs;
	}

	VkPipelineInputAssemblyStateCreateInfo assembly{};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewport_state{};
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;

	// No culling: blit_single_quad already rejected back faces at the seam, and the fullscreen passes
	// need every fragment.
	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth{};
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
	depth.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
	depth.depthCompareOp = depth_op;
	depth.depthBoundsTestEnable = VK_FALSE;
	depth.stencilTestEnable = VK_FALSE;

	// Opaque. An integer colour attachment (the pen passes) cannot blend anyway; the finish pass covers
	// the whole screen so there is nothing to blend against.
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
	info.renderPass = pass;
	info.subpass = 0;

	return check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &out),
			"vkCreateGraphicsPipelines (s21)");
}

bool build_layouts()
{
	// Layout A: one storage buffer, fragment stage (under, mix).
	VkDescriptorSetLayoutBinding a{};
	a.binding = 0;
	a.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	a.descriptorCount = 1;
	a.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo a_info{};
	a_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	a_info.bindingCount = 1;
	a_info.pBindings = &a;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &a_info, nullptr, &s_set_layout_a),
			"vkCreateDescriptorSetLayout (s21 A)"))
		return false;

	// Layout B: combined image sampler (pen) + two storage buffers (over, palette), fragment stage.
	VkDescriptorSetLayoutBinding b[3]{};
	b[0].binding = 0;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b[0].descriptorCount = 1;
	b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b[1].binding = 1;
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	b[1].descriptorCount = 1;
	b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b[2].binding = 2;
	b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	b[2].descriptorCount = 1;
	b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo b_info{};
	b_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	b_info.bindingCount = 3;
	b_info.pBindings = b;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &b_info, nullptr, &s_set_layout_b),
			"vkCreateDescriptorSetLayout (s21 B)"))
		return false;

	// Three sets per slot: under (A), mix (A), finish (B). B carries a sampled image; A/B carry the
	// storage buffers (under 1 + mix 1 + finish 2 = 4 per slot).
	VkDescriptorPoolSize sizes[2]{};
	sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[0].descriptorCount = s_slot_count * 4;
	sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[1].descriptorCount = s_slot_count;

	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = s_slot_count * 3;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = sizes;
	return check(s_fns.create_descriptor_pool(s_device, &pool_info, nullptr, &s_descriptor_pool),
			"vkCreateDescriptorPool (s21)");
}

// The private R16_UINT pen render pass: colour DONT_CARE (the under pass covers every pixel), depth
// CLEAR, colour finalLayout SHADER_READ_ONLY so the finish pass can sample it with no explicit barrier.
bool build_pen_render_pass()
{
	VkAttachmentDescription attachments[2]{};
	attachments[0].format = PEN_FORMAT;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	attachments[1].format = m2vk::GEOM_DEPTH_FORMAT;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colour_ref{};
	colour_ref.attachment = 0;
	colour_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_ref{};
	depth_ref.attachment = 1;
	depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colour_ref;
	subpass.pDepthStencilAttachment = &depth_ref;

	VkSubpassDependency deps[2]{};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	deps[0].srcAccessMask = 0;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	VkRenderPassCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = deps;
	return check(s_fns.create_render_pass(s_device, &info, nullptr, &s_pen_pass), "vkCreateRenderPass (s21 pen)");
}

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
	return check(s_fns.create_sampler(s_device, &info, nullptr, &s_pen_sampler), "vkCreateSampler (s21 pen)");
}

bool build_pipeline_layouts()
{
	auto make_layout = [](VkDescriptorSetLayout const *set, uint32_t set_count, VkShaderStageFlags push_stage,
			uint32_t push_size, VkPipelineLayout &out) -> bool
	{
		VkPushConstantRange push{};
		push.stageFlags = push_stage;
		push.offset = 0;
		push.size = push_size;

		VkPipelineLayoutCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		info.setLayoutCount = set_count;
		info.pSetLayouts = set;
		info.pushConstantRangeCount = 1;
		info.pPushConstantRanges = &push;
		return check(s_fns.create_pipeline_layout(s_device, &info, nullptr, &out), "vkCreatePipelineLayout (s21)");
	};

	return make_layout(nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, sizeof(geom_push), s_geom_layout)
			&& make_layout(&s_set_layout_a, 1, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(dims_push), s_under_layout)
			&& make_layout(&s_set_layout_a, 1, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(dims_push), s_mix_layout)
			&& make_layout(&s_set_layout_b, 1, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(finish_push), s_finish_layout);
}

bool build_pipelines()
{
	VkShaderModule fullscreen = make_module(FULLSCREEN_VERT_SPV, sizeof(FULLSCREEN_VERT_SPV));
	VkShaderModule s21vert = make_module(S21_VERT_SPV, sizeof(S21_VERT_SPV));
	VkShaderModule geom_frag = make_module(S21_PEN_GEOM_FRAG_SPV, sizeof(S21_PEN_GEOM_FRAG_SPV));
	VkShaderModule under_frag = make_module(S21_PEN_UNDER_FRAG_SPV, sizeof(S21_PEN_UNDER_FRAG_SPV));
	VkShaderModule mix_frag = make_module(S21_PEN_MIX_FRAG_SPV, sizeof(S21_PEN_MIX_FRAG_SPV));
	VkShaderModule finish_frag = make_module(S21_FINISH_FRAG_SPV, sizeof(S21_FINISH_FRAG_SPV));

	bool ok = (fullscreen != VK_NULL_HANDLE) && (s21vert != VK_NULL_HANDLE) && (geom_frag != VK_NULL_HANDLE)
			&& (under_frag != VK_NULL_HANDLE) && (mix_frag != VK_NULL_HANDLE) && (finish_frag != VK_NULL_HANDLE);

	// under: fullscreen, no depth, into the pen pass.
	ok = ok && make_pipeline(fullscreen, under_frag, s_under_layout, s_pen_pass,
			false, false, false, VK_COMPARE_OP_ALWAYS, s_under_pipeline);
	// geom: quads, GREATER + write (the real z-buffer), into the pen pass.
	ok = ok && make_pipeline(s21vert, geom_frag, s_geom_layout, s_pen_pass,
			true, true, true, VK_COMPARE_OP_GREATER, s_geom_pipeline);
	// mix: fullscreen, GREATER_OR_EQUAL no-write (pri[bank] <= z), into the pen pass.
	ok = ok && make_pipeline(fullscreen, mix_frag, s_mix_layout, s_pen_pass,
			false, true, false, VK_COMPARE_OP_GREATER_OR_EQUAL, s_mix_pipeline);
	// finish: fullscreen, no depth, into the shared present pass.
	ok = ok && make_pipeline(fullscreen, finish_frag, s_finish_layout, s_present_pass,
			false, false, false, VK_COMPARE_OP_ALWAYS, s_finish_pipeline);

	if (finish_frag != VK_NULL_HANDLE) s_fns.destroy_shader_module(s_device, finish_frag, nullptr);
	if (mix_frag != VK_NULL_HANDLE) s_fns.destroy_shader_module(s_device, mix_frag, nullptr);
	if (under_frag != VK_NULL_HANDLE) s_fns.destroy_shader_module(s_device, under_frag, nullptr);
	if (geom_frag != VK_NULL_HANDLE) s_fns.destroy_shader_module(s_device, geom_frag, nullptr);
	if (s21vert != VK_NULL_HANDLE) s_fns.destroy_shader_module(s_device, s21vert, nullptr);
	if (fullscreen != VK_NULL_HANDLE) s_fns.destroy_shader_module(s_device, fullscreen, nullptr);

	return ok;
}

// Everything the draw needs except the per-frame buffers and the pen images (which are data/size
// dependent): layouts, pipelines, render pass, sampler, pool, descriptor sets, palette buffers. Built
// once, on the first captured frame. A build that never captures never reaches here.
bool ensure_ready()
{
	if (s_ready)
		return true;
	if (s_failed || (s_device == VK_NULL_HANDLE) || (s_present_pass == VK_NULL_HANDLE))
		return false;

	if (!build_layouts() || !build_sampler() || !build_pen_render_pass()
			|| !build_pipeline_layouts() || !build_pipelines())
	{
		s_failed = true;
		return false;
	}

	for (uint32_t i = 0; i < s_slot_count; i++)
	{
		geom_slot &slot = s_slots[i];

		VkDescriptorSetLayout layouts[3] = { s_set_layout_a, s_set_layout_a, s_set_layout_b };
		VkDescriptorSet sets[3] = {};
		VkDescriptorSetAllocateInfo set_alloc{};
		set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		set_alloc.descriptorPool = s_descriptor_pool;
		set_alloc.descriptorSetCount = 3;
		set_alloc.pSetLayouts = layouts;
		if (!check(s_fns.allocate_descriptor_sets(s_device, &set_alloc, sets), "vkAllocateDescriptorSets (s21)"))
		{
			s_failed = true;
			return false;
		}
		slot.under_set = sets[0];
		slot.mix_set = sets[1];
		slot.finish_set = sets[2];

		if (!create_buffer(slot.palette, PALETTE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s21 palette)"))
		{
			s_failed = true;
			return false;
		}
		// The palette binding of the finish set is fixed (the buffer never moves); the other two bindings
		// (pen image, over buffer) are written when those resources are (re)created.
		write_storage_descriptor(slot.finish_set, 2, slot.palette.buffer);
	}

	s_ready = true;
	return true;
}

// (Re)creates a slot's pen/depth images and framebuffer at the draw extent, and rewrites the finish
// set's pen binding. Called from pen_pass once the draw extent is known.
bool ensure_pen_target(geom_slot &slot, uint32_t width, uint32_t height)
{
	if ((slot.framebuffer != VK_NULL_HANDLE) && (slot.pen_w == width) && (slot.pen_h == height))
		return true;

	if (slot.framebuffer != VK_NULL_HANDLE)
	{
		s_fns.destroy_framebuffer(s_device, slot.framebuffer, nullptr);
		slot.framebuffer = VK_NULL_HANDLE;
	}

	if (!create_image(slot.pen, width, height, PEN_FORMAT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_IMAGE_ASPECT_COLOR_BIT, "vkCreateImage (s21 pen)")
			|| !create_image(slot.depth, width, height, m2vk::GEOM_DEPTH_FORMAT,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				VK_IMAGE_ASPECT_DEPTH_BIT, "vkCreateImage (s21 depth)"))
	{
		return false;
	}

	VkImageView views[2] = { slot.pen.view, slot.depth.view };
	VkFramebufferCreateInfo fb{};
	fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb.renderPass = s_pen_pass;
	fb.attachmentCount = 2;
	fb.pAttachments = views;
	fb.width = width;
	fb.height = height;
	fb.layers = 1;
	if (!check(s_fns.create_framebuffer(s_device, &fb, nullptr, &slot.framebuffer), "vkCreateFramebuffer (s21 pen)"))
		return false;

	slot.pen_w = width;
	slot.pen_h = height;
	write_pen_descriptor(slot.finish_set, slot.pen.view);
	return true;
}

// Uploads one captured pen layer into a slot's storage buffer, resizing and rewriting its descriptor on
// growth. set/binding name where the descriptor for it lives (0 = "no descriptor", the over buffer whose
// descriptor is written by the caller). Returns whether the buffer holds this frame's data.
bool upload_layer(mapped_buffer &buf, uint32_t &cap, uint32_t const *src, uint32_t pixels, char const *what)
{
	if ((src == nullptr) || (pixels == 0) || (pixels > MAX_PIXEL_CAPACITY))
		return false;
	if ((cap < pixels) && !create_buffer(buf, VkDeviceSize(pixels) * sizeof(uint32_t),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, what))
		return false;
	if (cap < pixels)
		cap = pixels;
	std::memcpy(buf.mapped, src, size_t(pixels) * sizeof(uint32_t));
	return true;
}

} // anonymous namespace


//============================================================
//  the record — emulation thread
//============================================================

void record_begin()
{
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
	s_quads.swap(s_quads_work);
	s_quad_count = s_quad_work_count;
	s_valid = true;
	s_serial++;
}


//============================================================
//  the GPU — frontend thread
//============================================================

bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass present_render_pass, uint32_t slot_count)
{
	s_iface = &iface;
	s_fns = fns;
	s_device = iface.device;
	s_present_pass = present_render_pass;
	s_slot_count = (slot_count < MAX_SLOTS) ? slot_count : MAX_SLOTS;

	s_ready = false;
	s_failed = false;
	s_pen_pass = VK_NULL_HANDLE;
	s_pen_sampler = VK_NULL_HANDLE;
	s_set_layout_a = VK_NULL_HANDLE;
	s_set_layout_b = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_under_layout = s_geom_layout = s_mix_layout = s_finish_layout = VK_NULL_HANDLE;
	s_under_pipeline = s_geom_pipeline = s_mix_pipeline = s_finish_pipeline = VK_NULL_HANDLE;
	for (geom_slot &slot : s_slots)
		slot = geom_slot{};
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
		destroy_buffer(slot.under_buffer);
		destroy_buffer(slot.over_buffer);
		destroy_buffer(slot.mix_buffer);
		if (slot.framebuffer != VK_NULL_HANDLE)
			s_fns.destroy_framebuffer(s_device, slot.framebuffer, nullptr);
		destroy_image(slot.pen);
		destroy_image(slot.depth);
		slot = geom_slot{};
	}

	if (s_finish_pipeline != VK_NULL_HANDLE) s_fns.destroy_pipeline(s_device, s_finish_pipeline, nullptr);
	if (s_mix_pipeline != VK_NULL_HANDLE) s_fns.destroy_pipeline(s_device, s_mix_pipeline, nullptr);
	if (s_geom_pipeline != VK_NULL_HANDLE) s_fns.destroy_pipeline(s_device, s_geom_pipeline, nullptr);
	if (s_under_pipeline != VK_NULL_HANDLE) s_fns.destroy_pipeline(s_device, s_under_pipeline, nullptr);
	if (s_finish_layout != VK_NULL_HANDLE) s_fns.destroy_pipeline_layout(s_device, s_finish_layout, nullptr);
	if (s_mix_layout != VK_NULL_HANDLE) s_fns.destroy_pipeline_layout(s_device, s_mix_layout, nullptr);
	if (s_geom_layout != VK_NULL_HANDLE) s_fns.destroy_pipeline_layout(s_device, s_geom_layout, nullptr);
	if (s_under_layout != VK_NULL_HANDLE) s_fns.destroy_pipeline_layout(s_device, s_under_layout, nullptr);
	if (s_pen_pass != VK_NULL_HANDLE) s_fns.destroy_render_pass(s_device, s_pen_pass, nullptr);
	if (s_pen_sampler != VK_NULL_HANDLE) s_fns.destroy_sampler(s_device, s_pen_sampler, nullptr);
	if (s_descriptor_pool != VK_NULL_HANDLE) s_fns.destroy_descriptor_pool(s_device, s_descriptor_pool, nullptr);
	if (s_set_layout_b != VK_NULL_HANDLE) s_fns.destroy_descriptor_set_layout(s_device, s_set_layout_b, nullptr);
	if (s_set_layout_a != VK_NULL_HANDLE) s_fns.destroy_descriptor_set_layout(s_device, s_set_layout_a, nullptr);

	s_finish_pipeline = s_mix_pipeline = s_geom_pipeline = s_under_pipeline = VK_NULL_HANDLE;
	s_finish_layout = s_mix_layout = s_geom_layout = s_under_layout = VK_NULL_HANDLE;
	s_pen_pass = VK_NULL_HANDLE;
	s_pen_sampler = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout_a = s_set_layout_b = VK_NULL_HANDLE;
	s_ready = false;
	s_failed = false;
	s_present_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
}

void geom_forget()
{
	for (geom_slot &slot : s_slots)
		slot = geom_slot{};
	s_finish_pipeline = s_mix_pipeline = s_geom_pipeline = s_under_pipeline = VK_NULL_HANDLE;
	s_finish_layout = s_mix_layout = s_geom_layout = s_under_layout = VK_NULL_HANDLE;
	s_pen_pass = VK_NULL_HANDLE;
	s_pen_sampler = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout_a = s_set_layout_b = VK_NULL_HANDLE;
	s_ready = false;
	s_failed = false;
	s_present_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
	s_device = VK_NULL_HANDLE;
}

bool geom_upload(uint32_t slot_index)
{
	int under_w = 0, under_h = 0;
	uint32_t const *const under_src = under_pixels(under_w, under_h);
	// The 2D-under is captured on every S21 GPU frame; its absence means S21 does not own this frame
	// (software mode, M2VK_NO_3D, or a build that never captures). Cheap early-out for model2 / namcos22.
	if (under_src == nullptr)
		return false;
	if (slot_index >= s_slot_count)
		return false;
	if (!ensure_ready())
		return false;

	geom_slot &slot = s_slots[slot_index];

	// The CLUT, re-uploaded each frame (the game writes palette RAM live). Copy up to the buffer's
	// capacity; the shader guards a pen past the array length.
	if (palette_ram const &p = get_palette(); p.pens != nullptr)
	{
		const VkDeviceSize want = VkDeviceSize(p.count) * sizeof(uint32_t);
		std::memcpy(slot.palette.mapped, p.pens, size_t((want < PALETTE_BYTES) ? want : PALETTE_BYTES));
	}

	// The 2D-under pen buffer.
	{
		const uint32_t pixels = uint32_t(under_w) * uint32_t(under_h);
		const uint32_t prev_cap = slot.under_cap;
		if (!upload_layer(slot.under_buffer, slot.under_cap, under_src, pixels, "vkCreateBuffer (s21 under)"))
			return false;
		if (slot.under_cap != prev_cap)
			write_storage_descriptor(slot.under_set, 0, slot.under_buffer.buffer);
		slot.under_w = uint32_t(under_w);
		slot.under_h = uint32_t(under_h);
	}

	// The OVER band (tag + pen). Bound to the finish set (binding 1).
	{
		int ow = 0, oh = 0;
		uint32_t const *const over_src = over_pixels(ow, oh);
		const uint32_t pixels = uint32_t(ow) * uint32_t(oh);
		const uint32_t prev_cap = slot.over_cap;
		if (upload_layer(slot.over_buffer, slot.over_cap, over_src, pixels, "vkCreateBuffer (s21 over)"))
		{
			if (slot.over_cap != prev_cap)
				write_storage_descriptor(slot.finish_set, 1, slot.over_buffer.buffer);
			slot.over_w = uint32_t(ow);
			slot.over_h = uint32_t(oh);
		}
		// If no OVER was captured this frame (should not happen in GPU mode), the last one stays bound —
		// but the dims below fall back to the under extent so the finish pass still samples in range.
		if (slot.over_w == 0 || slot.over_h == 0)
		{
			slot.over_w = slot.under_w;
			slot.over_h = slot.under_h;
		}
	}

	// The layer-0 z-mix (tag + pen), only on pri1==4 frames. Bound to the mix set.
	{
		int mw = 0, mh = 0;
		uint32_t const *const mix_src = mix_pixels(mw, mh);
		const uint32_t pixels = uint32_t(mw) * uint32_t(mh);
		const uint32_t prev_cap = slot.mix_cap;
		if (upload_layer(slot.mix_buffer, slot.mix_cap, mix_src, pixels, "vkCreateBuffer (s21 mix)"))
		{
			if (slot.mix_cap != prev_cap)
				write_storage_descriptor(slot.mix_set, 0, slot.mix_buffer.buffer);
			slot.mix_w = uint32_t(mw);
			slot.mix_h = uint32_t(mh);
		}
		else
		{
			slot.mix_w = 0;
			slot.mix_h = 0;
		}
	}

	// The 3D geometry. A frame can own the picture (2D + OVER) with no polygons at all.
	slot.index_count = 0;
	const bool have_quads = s_valid && (s_quad_count != 0);
	if (have_quads)
	{
		if ((slot.capacity < s_quad_count) && (s_quad_count <= MAX_QUAD_CAPACITY))
		{
			const uint32_t want = s_quad_count + (s_quad_count / 2);
			const VkDeviceSize verts = VkDeviceSize(want) * QUAD_VERTS * sizeof(gpu_vertex);
			const VkDeviceSize idx = VkDeviceSize(want) * QUAD_INDICES * sizeof(uint32_t);
			if (create_buffer(slot.vertices, verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vkCreateBuffer (s21 vertices)")
					&& create_buffer(slot.indices, idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "vkCreateBuffer (s21 indices)"))
				slot.capacity = want;
		}

		if (slot.capacity >= s_quad_count)
		{
			auto *const verts = static_cast<gpu_vertex *>(slot.vertices.mapped);
			auto *const idxb = static_cast<uint32_t *>(slot.indices.mapped);
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
				idxb[icount++] = vbase;     idxb[icount++] = vbase + 1; idxb[icount++] = vbase + 2;
				idxb[icount++] = vbase + 2; idxb[icount++] = vbase + 3; idxb[icount++] = vbase;
			}
			slot.index_count = icount;
		}
	}

	// Stats and the once-per-run first line.
	if (have_quads && (s_serial != s_drawn_serial))
	{
		s_drawn_serial = s_serial;
		s_run_quads += s_quad_count;
		if (s_quad_count > s_max_quads)
			s_max_quads = s_quad_count;
	}
	if (!s_reported_first && (slot.index_count != 0))
	{
		s_reported_first = true;
		vk_log(RETRO_LOG_INFO, "s21: first GPU geometry — %u quads, %u indices\n",
				unsigned(s_quad_count), unsigned(slot.index_count));
	}

	return true;
}

void pen_pass(VkCommandBuffer cmd, uint32_t slot_index, unsigned width, unsigned height)
{
	if (!s_ready || (slot_index >= s_slot_count) || (width == 0) || (height == 0))
		return;

	geom_slot &slot = s_slots[slot_index];
	if (!ensure_pen_target(slot, width, height))
		return;

	VkClearValue clear[2]{};
	clear[0].color.uint32[0] = 0;         // colour is DONT_CARE (the under pass covers all), but valid
	clear[1].depthStencil = { 0.0f, 0 };  // depth cleared to 0 — no polygon has claimed a pixel yet

	VkRenderPassBeginInfo pass{};
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = s_pen_pass;
	pass.framebuffer = slot.framebuffer;
	pass.renderArea.offset = { 0, 0 };
	pass.renderArea.extent = { width, height };
	pass.clearValueCount = 2;
	pass.pClearValues = clear;
	s_fns.cmd_begin_render_pass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = float(width);
	viewport.height = float(height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	s_fns.cmd_set_viewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = { width, height };
	s_fns.cmd_set_scissor(cmd, 0, 1, &scissor);

	// 1. The 2D-under pens, fullscreen (point-upscaled from native to the draw extent).
	dims_push under_dims{ slot.under_w, slot.under_h };
	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_under_pipeline);
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_under_layout, 0, 1, &slot.under_set, 0, nullptr);
	s_fns.cmd_push_constants(cmd, s_under_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(under_dims), &under_dims);
	s_fns.cmd_draw(cmd, 3, 1, 0, 0);

	// 2. The 3D quads, depth-tested. half_size is the NATIVE visible half-extent (the quads are in native
	// framebuffer pixels; the viewport scales NDC to the draw extent), which is the under capture's size.
	if (slot.index_count != 0)
	{
		geom_push push{};
		push.half_width = float(slot.under_w) * 0.5f;
		push.half_height = float(slot.under_h) * 0.5f;
		const VkDeviceSize offset = 0;
		s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_geom_pipeline);
		s_fns.cmd_push_constants(cmd, s_geom_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
		s_fns.cmd_bind_vertex_buffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
		s_fns.cmd_bind_index_buffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
		s_fns.cmd_draw_indexed(cmd, slot.index_count, 1, 0, 0, 0);
	}

	// 3. The layer-0 C355 z-mix, fullscreen, thresholded against the depth the geom pass just wrote.
	if ((slot.mix_w != 0) && (slot.mix_h != 0))
	{
		dims_push mix_dims{ slot.mix_w, slot.mix_h };
		s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mix_pipeline);
		s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mix_layout, 0, 1, &slot.mix_set, 0, nullptr);
		s_fns.cmd_push_constants(cmd, s_mix_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mix_dims), &mix_dims);
		s_fns.cmd_draw(cmd, 3, 1, 0, 0);
	}

	s_fns.cmd_end_render_pass(cmd);
}

void finish_draw(VkCommandBuffer cmd, uint32_t slot_index, unsigned width, unsigned height)
{
	(void)width;
	(void)height;
	if (!s_ready || (slot_index >= s_slot_count))
		return;

	geom_slot &slot = s_slots[slot_index];
	if ((slot.framebuffer == VK_NULL_HANDLE) || (slot.finish_set == VK_NULL_HANDLE))
		return;

	// The push is the OVER buffer's native dimensions, for the fullscreen v_uv -> buffer index map; the
	// pen attachment is sampled 1:1 by gl_FragCoord (it is the draw extent, == this pass's extent). The
	// caller has already set the viewport/scissor to the draw extent.
	over_shadow_params const osp = get_over_shadow();
	finish_push fp{ slot.over_w, slot.over_h, osp.enabled ? 1u : 0u, osp.sentinel, osp.opaque_base };
	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_finish_pipeline);
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_finish_layout, 0, 1, &slot.finish_set, 0, nullptr);
	s_fns.cmd_push_constants(cmd, s_finish_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(fp), &fp);
	s_fns.cmd_draw(cmd, 3, 1, 0, 0);
}

// The recorded frame's quad count, for the polygon-counter HUD. Latched at record_begin, so after a
// frame it is that frame's total; a frame that recorded nothing new keeps the previous count.
uint32_t geom_primitive_count()
{
	return s_quad_count;
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
