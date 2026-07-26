// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the polygon pass. See vk_geom.h for the three decisions this rests on.

    The shape of one frame here is: walk the record's polygon stream in draw order, fan each polygon
    to n-2 triangles, write vertices, indices and one parameter block per polygon straight into
    mapped device memory, and record a single indexed draw. One draw for the whole frame is possible
    because nothing varies per polygon except data — no scissor yet (P3 step 6), no pipeline change,
    no descriptor rebind.

    Two traps that are already paid for and must stay paid:

      * Indices are 32-bit. Primitive restart cannot be disabled on this implementation — MoltenVK
        says so once per pipeline creation, because Metal has no way to turn it off — so an index of
        0xffff with 16-bit indices would restart the primitive whether or not the pipeline asked.
        32-bit indices put the restart value at 0xffffffff, which the vertex count cannot reach:
        1450 polygons at 8 vertices is 11600, and the capacity check below refuses anything that
        would come near it.

      * The parameter buffer is indexed by the polygon's position in the *record*, not by its
        position among the polygons that were actually drawn. Skipped polygons leave a dead 16-byte
        entry, which is the right trade: the draw-order depth key comes from the same index, so when
        the textured paths arrive they slot in at the depth they always had rather than shifting
        everything that follows.

*********************************************************************************************************************************/

#include "renderer_vk/vk_geom.h"

#include "renderer_vk/shaders/poly_frag_spv.h"
#include "renderer_vk/shaders/poly_vert_spv.h"

#include "m2vk_frame.h"
#include "m2vk_sink.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>


namespace m2vk {

const VkFormat GEOM_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

namespace {

//============================================================
//  constants
//============================================================

// Matches vk_present's ceiling on the sync-index mask; the two arrays are indexed by the same thing.
constexpr uint32_t MAX_SLOTS = 8;

// The record's high-water mark settles within a few frames; this is where it starts, chosen so that
// the observed worst case (1450 polygons in a VF2 frame) never reallocates. A frame that exceeds it
// grows the buffers once and they stay grown.
constexpr uint32_t INITIAL_POLY_CAPACITY = 2048;

// A hard ceiling, and the reason for it is the index type rather than memory: MAX_VERTICES per
// polygon times this must stay far below 0xffffffff, the primitive-restart index. It is also a
// sanity bound on a count that comes from emulated hardware.
constexpr uint32_t MAX_POLY_CAPACITY = 1u << 20;

// 1 - n/65536 for polygon n in draw order. 16 bits is ample for 1450 polygons and D32_SFLOAT
// represents every one of these values exactly.
constexpr float DEPTH_STEP = 1.0f / 65536.0f;
constexpr uint32_t DEPTH_MAX_INDEX = 65535;


//============================================================
//  what crosses to the GPU
//============================================================

// 28 bytes. rz/uz/vz are unused by the untextured path and are carried anyway: they are what the
// textured steps interpolate, and changing the vertex format later would mean re-verifying the
// pipeline rather than just the shader.
struct gpu_vertex
{
	float    x, y, z;       // x,y in m_destmap pixels; z is the draw-order key
	float    rz;            // 1/z, ALWAYS — see normalise_rz below
	float    uz, vz;        // u/z, v/z: MAME's uoz/voz
	uint32_t poly;          // index into the parameter buffer
};

static_assert(sizeof(gpu_vertex) == 28, "the vertex attribute offsets below are written out by hand");

// std430, four words, and it stays four words until a textured step needs more. Mirrors the
// poly_params struct at the top of poly.frag.
struct gpu_poly
{
	uint32_t palcolor;
	uint32_t luma;
	uint32_t flags;
	uint32_t reserved;
};

enum : uint32_t
{
	FLAG_TRANSLUCENT = 1,
	FLAG_TEXTURED    = 2,
	FLAG_CHECKER     = 4
};

// The vertex shader's push constant block: the visible picture's half-extent in pixels.
struct gpu_push
{
	float half_width, half_height;
};


//============================================================
//  state
//============================================================

// A buffer written straight from the host. Every one of these lives in memory that is both
// device-local and host-visible on this device, so there is no staging copy anywhere in this file.
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
	mapped_buffer   params;
	mapped_buffer   colorxlat;
	mapped_buffer   lumaram;

	VkDescriptorSet descriptor = VK_NULL_HANDLE;

	uint32_t        capacity = 0;       // polygons the three per-frame buffers are sized for
	uint32_t        index_count = 0;    // what this slot's recorded draw will submit
	uint64_t        tables_serial = 0;  // what is in colorxlat/lumaram right now
	bool            tables_valid = false;
};

std::array<geom_slot, MAX_SLOTS> s_slots{};
uint32_t s_slot_count = 0;

VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout      s_pipeline_layout = VK_NULL_HANDLE;
VkPipeline            s_pipeline = VK_NULL_HANDLE;

const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

// M2VK_NO_3D=1 draws neither the software 3D nor the hardware 3D, leaving the two tilemap layers
// with a hole between them. That is the reference picture the coverage comparison differences
// against: it is bit-identical under both renderers, because neither of them touches those pixels.
bool s_no_3d = false;
bool s_no_3d_known = false;

// Said once rather than 57 times a second.
bool s_reported_skips = false;


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

bool no_3d()
{
	if (!s_no_3d_known)
	{
		s_no_3d = (std::getenv("M2VK_NO_3D") != nullptr);
		s_no_3d_known = true;
	}
	return s_no_3d;
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

// HOST_VISIBLE | HOST_COHERENT is required and DEVICE_LOCAL is asked for first: on unified memory
// the same type is both, and where it is not, correctness does not depend on which one we get.
// Coherent means no vkFlushMappedMemoryRanges anywhere, which is one fewer thing to forget.
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
		vk_log(RETRO_LOG_ERROR, "no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
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

// Points the slot's descriptor set at whatever its five buffers currently are. Called at build and
// again whenever growth has replaced one of them.
void write_descriptor(geom_slot &slot)
{
	const VkBuffer buffers[3] = { slot.params.buffer, slot.colorxlat.buffer, slot.lumaram.buffer };

	VkDescriptorBufferInfo info[3]{};
	VkWriteDescriptorSet write[3]{};
	for (uint32_t i = 0; i < 3; i++)
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

	s_fns.update_descriptor_sets(s_device, 3, write, 0, nullptr);
}

// The three per-frame buffers, sized for `polys` polygons: every polygon may have MAX_VERTICES
// vertices, and a fan of n vertices is (n-2)*3 indices, so 8 and 18 are the exact worst cases.
bool size_slot(geom_slot &slot, uint32_t polys)
{
	if (polys > MAX_POLY_CAPACITY)
	{
		vk_log(RETRO_LOG_ERROR, "a frame of %u polygons is past the %u the geometry buffers will size to\n",
				unsigned(polys), unsigned(MAX_POLY_CAPACITY));
		return false;
	}

	const VkDeviceSize verts = VkDeviceSize(polys) * m2vk::MAX_VERTICES * sizeof(gpu_vertex);
	const VkDeviceSize idx = VkDeviceSize(polys) * 18 * sizeof(uint32_t);
	const VkDeviceSize par = VkDeviceSize(polys) * sizeof(gpu_poly);

	if (!create_buffer(slot.vertices, verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vkCreateBuffer (vertices)")
			|| !create_buffer(slot.indices, idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "vkCreateBuffer (indices)")
			|| !create_buffer(slot.params, par, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (params)"))
	{
		return false;
	}

	slot.capacity = polys;
	return true;
}


//============================================================
//  the pipeline
//============================================================

bool build_descriptors(uint32_t slot_count)
{
	// Three storage buffers: the per-polygon parameters, the baked colour ramps, and the luma
	// translator. The last is unused by the untextured path and is bound anyway — the record already
	// carries it, and an unbound descriptor is a worse thing to leave lying around than an unread one.
	VkDescriptorSetLayoutBinding bindings[3]{};
	for (uint32_t i = 0; i < 3; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 3;
	layout_info.pBindings = bindings;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &layout_info, nullptr, &s_set_layout),
			"vkCreateDescriptorSetLayout (geometry)"))
	{
		return false;
	}

	VkDescriptorPoolSize size{};
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = slot_count * 3;

	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = slot_count;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	return check(s_fns.create_descriptor_pool(s_device, &pool_info, nullptr, &s_descriptor_pool),
			"vkCreateDescriptorPool (geometry)");
}

bool build_pipeline(VkRenderPass render_pass)
{
	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0;
	push.size = sizeof(gpu_push);

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &s_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout),
			"vkCreatePipelineLayout (geometry)"))
	{
		return false;
	}

	VkShaderModule vert = VK_NULL_HANDLE;
	VkShaderModule frag = VK_NULL_HANDLE;
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &out)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule");
	};

	bool ok = make_module(POLY_VERT_SPV, sizeof(POLY_VERT_SPV), vert)
			&& make_module(POLY_FRAG_SPV, sizeof(POLY_FRAG_SPV), frag);

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
		attrs[0].location = 0;
		attrs[0].binding = 0;
		attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;   // x, y, draw-order depth
		attrs[0].offset = 0;
		attrs[1].location = 1;
		attrs[1].binding = 0;
		attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;   // rz, uz, vz
		attrs[1].offset = 12;
		attrs[2].location = 2;
		attrs[2].binding = 0;
		attrs[2].format = VK_FORMAT_R32_UINT;           // parameter index
		attrs[2].offset = 24;

		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = 3;
		vertex_input.pVertexAttributeDescriptions = attrs;

		// primitiveRestartEnable stays false and this implementation will say it cannot honour that.
		// It does not matter: see the note at the top of the file about 32-bit indices.
		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewport_state{};
		viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_state.viewportCount = 1;
		viewport_state.scissorCount = 1;

		// No culling. Backface rejection already happened in the geometry engine's check_culling(),
		// and the fan's winding is whatever the game's vertex order made it — the software renderer
		// has no winding preference either.
		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// GREATER with writes on, against a buffer cleared to 0: the first polygon to reach a pixel
		// carries the largest key and wins, and every later one fails. m_fillmap, in hardware.
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depth.depthTestEnable = VK_TRUE;
		depth.depthWriteEnable = VK_TRUE;
		depth.depthCompareOp = VK_COMPARE_OP_GREATER;
		depth.depthBoundsTestEnable = VK_FALSE;
		depth.stencilTestEnable = VK_FALSE;
		depth.minDepthBounds = 0.0f;
		depth.maxDepthBounds = 1.0f;

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
		info.renderPass = render_pass;
		info.subpass = 0;

		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline),
				"vkCreateGraphicsPipelines (geometry)");
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}


//============================================================
//  the frame
//============================================================

// The vertex format's rz is 1/z on every polygon, which is the textured convention. It has to be
// normalised because the seam hands over two different things under one name: model2_3d_render only
// runs its reciprocal loop in the textured branch, so poly.v[i].rz is 1/z for a textured polygon and
// raw z for an untextured one. MAME's own expression is reproduced verbatim, denormal guard and all,
// so that a polygon that later takes the textured path gets the identical float.
float normalise_rz(m2vk::poly const &p, float rz)
{
	if ((p.renderer & 2) != 0)
		return rz;
	return 1.0f / (rz + std::numeric_limits<float>::min());
}

} // anonymous namespace


//============================================================
//  m2vk — the public surface
//============================================================

bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass render_pass, uint32_t slot_count)
{
	geom_destroy();

	if ((slot_count == 0) || (slot_count > MAX_SLOTS))
		return false;

	s_iface = &iface;
	s_fns = fns;
	s_device = iface.device;

	if (!build_descriptors(slot_count) || !build_pipeline(render_pass))
	{
		geom_destroy();
		return false;
	}

	s_slot_count = slot_count;
	for (uint32_t i = 0; i < slot_count; i++)
	{
		geom_slot &slot = s_slots[i];

		VkDescriptorSetAllocateInfo set_alloc{};
		set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		set_alloc.descriptorPool = s_descriptor_pool;
		set_alloc.descriptorSetCount = 1;
		set_alloc.pSetLayouts = &s_set_layout;
		if (!check(s_fns.allocate_descriptor_sets(s_device, &set_alloc, &slot.descriptor),
				"vkAllocateDescriptorSets (geometry)"))
		{
			geom_destroy();
			return false;
		}

		// The two tables are fixed size and are allocated here rather than on the first frame that
		// carries them, so that nothing in the per-frame path allocates in the steady state.
		if (!size_slot(slot, INITIAL_POLY_CAPACITY)
				|| !create_buffer(slot.colorxlat, m2vk::COLORXLAT_ENTRIES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
						"vkCreateBuffer (colorxlat)")
				|| !create_buffer(slot.lumaram, m2vk::LUMARAM_ENTRIES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
						"vkCreateBuffer (lumaram)"))
		{
			geom_destroy();
			return false;
		}

		write_descriptor(slot);
	}

	const VkDeviceSize per_slot = s_slots[0].vertices.size + s_slots[0].indices.size + s_slots[0].params.size
			+ s_slots[0].colorxlat.size + s_slots[0].lumaram.size;
	vk_log(RETRO_LOG_INFO, "geometry: %u slots of %u polygons, %llu KiB each; depth D32_SFLOAT, draw order\n",
			unsigned(slot_count), unsigned(INITIAL_POLY_CAPACITY), (unsigned long long)(per_slot / 1024));

	return true;
}

void geom_destroy()
{
	if (s_device == VK_NULL_HANDLE)
	{
		geom_forget();
		return;
	}

	for (uint32_t i = 0; i < s_slot_count; i++)
	{
		geom_slot &slot = s_slots[i];
		destroy_buffer(slot.vertices);
		destroy_buffer(slot.indices);
		destroy_buffer(slot.params);
		destroy_buffer(slot.colorxlat);
		destroy_buffer(slot.lumaram);
	}

	if (s_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline, nullptr);
	if (s_pipeline_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_pipeline_layout, nullptr);
	// The sets are freed with the pool.
	if (s_descriptor_pool != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_pool(s_device, s_descriptor_pool, nullptr);
	if (s_set_layout != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_set_layout(s_device, s_set_layout, nullptr);

	geom_forget();
}

void geom_forget()
{
	for (geom_slot &slot : s_slots)
		slot = geom_slot{};
	s_slot_count = 0;

	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_device = VK_NULL_HANDLE;
	s_iface = nullptr;
}

bool geom_upload(uint32_t slot_index, frame_record const &record)
{
	if ((slot_index >= s_slot_count) || (s_pipeline == VK_NULL_HANDLE))
		return false;

	// The software rasteriser still owning the 3D is not an error: it is what M2VK_SW_3D=1 asks for,
	// and drawing the same polygons twice would be the bug. Its output is already inside the under
	// layer, exactly as it was in steps 1 and 2.
	if (sw_owns_3d() || no_3d())
		return false;
	if (!record.geometry_valid || (record.poly_count == 0) || !record.tables_valid)
		return false;

	geom_slot &slot = s_slots[slot_index];
	slot.index_count = 0;

	// Growth happens here, inside the caller's fence wait, so nothing being replaced is in flight.
	// It happens at most a handful of times a run and then never again.
	if (record.poly_count > slot.capacity)
	{
		const uint32_t want = (record.poly_count > (slot.capacity * 2)) ? record.poly_count : (slot.capacity * 2);
		vk_log(RETRO_LOG_INFO, "geometry slot %u grown from %u to %u polygons\n",
				unsigned(slot_index), unsigned(slot.capacity), unsigned(want));
		if (!size_slot(slot, want))
			return false;
		write_descriptor(slot);
	}

	auto *const vertices = static_cast<gpu_vertex *>(slot.vertices.mapped);
	auto *const indices = static_cast<uint32_t *>(slot.indices.mapped);
	auto *const params = static_cast<gpu_poly *>(slot.params.mapped);

	uint32_t vcount = 0;
	uint32_t icount = 0;
	uint32_t skipped_textured = 0;
	uint32_t skipped_translucent = 0;

	for (uint32_t n = 0; n < record.poly_count; n++)
	{
		m2vk::poly const &p = record.polys[n];
		const uint8_t cls = p.renderer & 3;

		// cls is bit0 = translucent, bit1 = textured — the same two bits, in the same order, that
		// m_render_callbacks is indexed by. Spelled out rather than passed through, so that the flag
		// words stay independent of that coincidence when the textured steps add to them.
		params[n].palcolor = p.palcolor;
		params[n].luma = p.luma;
		params[n].flags = ((cls & 1) ? FLAG_TRANSLUCENT : 0u)
				| ((cls & 2) ? FLAG_TEXTURED : 0u)
				| (p.checker ? FLAG_CHECKER : 0u);
		params[n].reserved = 0;

		// A translucent untextured polygon draws nothing at all: draw_scanline_solid<true> returns
		// before writing a pixel. Dropping it here rather than discarding it in the shader is not an
		// optimisation — a discarded fragment still would not write depth, but a dropped polygon is
		// visibly the same decision the software renderer makes, in the same place.
		if (cls == 1)
		{
			skipped_translucent++;
			continue;
		}
		if ((cls & 2) != 0)
		{
			// The textured paths are the next two steps. Counted rather than approximated: drawing
			// these flat would produce a plausible-looking picture that is not the game.
			skipped_textured++;
			continue;
		}
		// Clamped rather than trusted. The buffers above are sized at MAX_VERTICES per polygon, which
		// is what model2_state::polygon declares, and the one place this could exceed it is a
		// corrupted display list — where the cost of believing it is a write past the end of mapped
		// device memory.
		const uint32_t nverts = (p.num_verts > m2vk::MAX_VERTICES) ? uint32_t(m2vk::MAX_VERTICES) : p.num_verts;
		if (nverts < 3)
			continue;

		const uint32_t base = vcount;
		const float z = 1.0f - (float((n > DEPTH_MAX_INDEX) ? DEPTH_MAX_INDEX : n) * DEPTH_STEP);

		for (uint32_t i = 0; i < nverts; i++)
		{
			gpu_vertex &v = vertices[vcount++];
			v.x = p.v[i].x;
			v.y = p.v[i].y;
			v.z = z;
			v.rz = normalise_rz(p, p.v[i].rz);
			v.uz = p.v[i].uz;
			v.vz = p.v[i].vz;
			v.poly = n;
		}

		// A fan from vertex 0. Model 2 polygons are convex — the geometry engine clips them against
		// four planes — so a fan is exactly what MAME's render_polygon<N,3> covers.
		for (uint32_t i = 1; i + 1 < nverts; i++)
		{
			indices[icount++] = base;
			indices[icount++] = base + i;
			indices[icount++] = base + i + 1;
		}
	}

	if (!s_reported_skips && ((skipped_textured != 0) || (skipped_translucent != 0)))
	{
		s_reported_skips = true;
		vk_log(RETRO_LOG_INFO, "geometry: of %u polygons, %u drawn, %u textured (steps 4 and 5), %u untextured translucent (drawn by neither renderer)\n",
				unsigned(record.poly_count),
				unsigned(record.poly_count - skipped_textured - skipped_translucent),
				unsigned(skipped_textured), unsigned(skipped_translucent));
	}

	// The tables are 56 KB and change six times in nine hundred frames of VF2, so the serial is worth
	// having: it is bumped only when the bytes actually differed, and this slot holds whatever it was
	// last given.
	if ((!slot.tables_valid || (slot.tables_serial != record.tables_serial))
			&& (record.colorxlat.size() == m2vk::COLORXLAT_ENTRIES)
			&& (record.lumaram.size() == m2vk::LUMARAM_ENTRIES))
	{
		std::memcpy(slot.colorxlat.mapped, record.colorxlat.data(), m2vk::COLORXLAT_ENTRIES);
		std::memcpy(slot.lumaram.mapped, record.lumaram.data(), m2vk::LUMARAM_ENTRIES);
		slot.tables_serial = record.tables_serial;
		slot.tables_valid = true;
	}

	slot.index_count = icount;
	return icount != 0;
}

void geom_draw(uint32_t slot_index, VkCommandBuffer cmd, unsigned width, unsigned height)
{
	if ((slot_index >= s_slot_count) || (s_pipeline == VK_NULL_HANDLE))
		return;

	geom_slot &slot = s_slots[slot_index];
	if (slot.index_count == 0)
		return;

	gpu_push push{};
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

void geom_end_run()
{
	s_reported_skips = false;
	s_no_3d_known = false;
}

} // namespace m2vk
