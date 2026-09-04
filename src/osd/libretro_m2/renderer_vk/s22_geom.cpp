// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco (Super) System 22 — the polygon pass (S2b: textured).

    See s22_geom.h for the shape of the phase and s22_seam.h for the stream. This file is the record
    the seam fills on the emulation thread, plus the GPU pipeline that turns it into textured, shaded
    triangles on the frontend's thread. The texel fetch is renderscanline_poly's, done per fragment in
    s22.frag; this file uploads the tile system it reads and builds the per-frame geometry.

    Two things are settled and are the ones to understand before editing:

      * DEPTH IS DRAW ORDER, NOT z, as it is for Model 2 — but the System 22 tree is walked
        BACK-TO-FRONT (see s22_seam.h), the opposite of Model 2's stream, so the ordering is a plain
        painter's algorithm: draw in record order, last writer wins the pixel. That needs no depth
        buffer at all, so this pipeline disables the depth test and leaves the ring's depth attachment
        (which Model 2 needs) untouched.

      * THE TEXTURE SYSTEM IS STATIC AND SHARED. ttmap / ttattr / ttdata / ayx are ROM-derived and
        fixed after init_tables, so they are uploaded ONCE into buffers shared by every slot. Only the
        palette changes per frame, and it is small (128 KB), so it is the one thing re-uploaded each
        frame, per slot. The Model 2 build never captures, so none of this is ever allocated there.

    Like vk_geom, per-frame buffers are host-visible device-local and written with no staging copy, and
    the record is turned into buffers on the frontend's thread from data the emulation thread wrote and
    is now parked against, so there is no lock.

*********************************************************************************************************************************/

#include "renderer_vk/s22_geom.h"

#include "s22_seam.h"

#include "renderer_vk/shaders/s22_vert_spv.h"
#include "renderer_vk/shaders/s22_frag_spv.h"

#include <algorithm>
#include <array>
#include <cmath>
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

// ridgerac peaks near 2000 quads a scene (S1's tap), so this never reallocates in the steady state.
constexpr uint32_t INITIAL_QUAD_CAPACITY = 4096;

// A sanity bound on a count that comes from emulated hardware, and headroom for the index type.
constexpr uint32_t MAX_QUAD_CAPACITY = 1u << 21;

// A fan of up to 6 vertices; 6 verts and 4 triangles = 12 indices are the worst case per quad.
constexpr uint32_t MAX_QUAD_VERTS = 6;
constexpr uint32_t MAX_QUAD_INDICES = 12;

// The tile system's fixed sizes (bytes), from init_tables in namcos22_v.cpp. ttdata is the "textile"
// region (0x1000000); ttmap is 0x100000 halfwords; ttattr is 0x100000 unpacked bytes; ayx is 16*16*16;
// palette is 0x8000 pens. The shader masks every index to these, so they are the ground truth for both.
constexpr VkDeviceSize TTDATA_BYTES  = 0x1000000;
constexpr VkDeviceSize TTATTR_BYTES  = 0x100000;
constexpr VkDeviceSize TTMAP_BYTES   = 0x100000 * 2;
constexpr VkDeviceSize AYX_BYTES     = 0x1000;
constexpr VkDeviceSize PALETTE_BYTES = 0x8000 * sizeof(uint32_t);

// The SS22 z-fog tables (recalc_czram): four banks of 0x2000 bytes, concatenated. Per-slot and
// re-uploaded each frame (contents change), the same handling the palette gets.
constexpr VkDeviceSize CZRAM_BANK    = 0x2000;
constexpr VkDeviceSize CZRAM_BYTES   = CZRAM_BANK * 4;

// The plain System 22 final gamma LUT (m_gamma_proms): rlut|glut|blut, 0x100 bytes each. Static,
// ROM-derived, shared like the tile system. Applied as the last step in s22.frag on plain-S22 quads.
constexpr VkDeviceSize GAMMA_BYTES   = 0x300;

// The sprite gfx (gfx(2), the "sprite" ROM region): 8bpp 32x32 tiles, char_modulo 1024, line_modulo 32.
// Static and ROM-derived, uploaded once like the tile system. 0x1000000 bytes on Super System 22; a
// small placeholder on plain System 22, which emits no sprites — the buffer is bound but never sampled.
constexpr VkDeviceSize SPRITE_MAX_BYTES = 0x1000000;

// 44 bytes: screen position, the three screen-linear params the fragment divides, the shade param, the
// three flat per-quad words, then the two flat per-quad shading-tail words. Attribute offsets below are
// written out by hand.
struct gpu_vertex
{
	float    x, y;            // screen space
	float    uoz, voz, ooz;   // (u+0.5)*ooz, (v+0.5)*ooz, ooz  (driver clipv.p[1], p[2], p[0])
	float    iw;              // (bri+0.5)*ooz                   (driver clipv.p[3])
	uint32_t attr;            // flags | (color & 0x7f) << 8 | cmode << 16
	uint32_t bn;              // texturebank
	uint32_t base;            // untextured fill, 0x00RRGGBB
	uint32_t sf0;             // fogfactor | cz_bank<<8 | zfog<<10 | alpha_en<<11 | ss22<<12 | (sdelta+256)<<13
	uint32_t sf1;             // fog colour, 0x00RRGGBB
};

static_assert(sizeof(gpu_vertex) == 44, "the vertex attribute offsets below are written out by hand");

// Attribute flag bits, matched in s22.frag.
constexpr uint32_t ATTR_TEXTURED = 1u;
constexpr uint32_t ATTR_SHADE    = 2u;
constexpr uint32_t ATTR_SPRITE   = 4u;    // this vertex is a sprite tile — s22.frag takes the sprite fetch
constexpr uint32_t ATTR_SFLIPX   = 8u;    // sprite: the one-texel x sampling shift
constexpr uint32_t ATTR_SFLIPY   = 16u;   // sprite: the one-texel y sampling shift

// sf0 bit layout for a POLYGON, matched in s22.frag.
constexpr uint32_t SF0_ZFOG      = 1u << 10;
constexpr uint32_t SF0_ALPHA_EN  = 1u << 11;
constexpr uint32_t SF0_SS22      = 1u << 12;

// sf0 bit layout for a SPRITE (read only under ATTR_SPRITE): fogfactor | fadefactor<<8 | alpha<<16 |
// flags. A sprite is always Super System 22, so the SS22 final gamma is implied — no ss22 bit needed.
constexpr uint32_t SSF_ALPHA_EN  = 1u << 24;

// The push block: the visible picture's half-extent in pixels (s22.vert), then the SS22 shading tail's
// per-frame globals (s22.frag) — screen fade, poly fade and poly alpha, the same for every quad.
struct push_block
{
	float    half_width, half_height;
	uint32_t alpha_pen;
	uint32_t alpha_factor;
	uint32_t fade_factor;
	uint32_t fade_r, fade_g, fade_b;
	uint32_t poly_flags;              // bit0 = poly_fade_enabled
	uint32_t poly_r, poly_g, poly_b;
	uint32_t tex_filter;              // 0 = point sample (hardware), 1 = bilinear on the 3D texture tail
	float    depth_scale;             // 0 = painter's (z is the constant depth_bias, test off in the
	float    depth_bias;              // pipeline); else z = ooz*depth_scale + depth_bias — see s22.vert
};

static_assert(sizeof(push_block) == 60, "s22 push block is fifteen words");

constexpr uint32_t PFLAG_POLY_FADE = 1u;
// The debug/enhancement view toggles, ORed into push.poly_flags per frame from the option statics below.
// Must match the PFLAG_* constants in s22.frag.
constexpr uint32_t PFLAG_NO_FOG   = 2u;
constexpr uint32_t PFLAG_NO_TEX   = 4u;
constexpr uint32_t PFLAG_NO_LIGHT = 8u;

// system22_texture_filter, parked by retro_entry when the option changes. M2VK_S22_FILTER overrides it
// in the standing direction (presence-or-value): the switch wins so a harness run can pin it either way.
// Off is the hardware-accurate default; on is a pure enhancement (System 22 point-sampled its textures).
bool s_option_filter = false;
int  s_env_filter = -2;   // -2 = not yet read; -1 = no switch set; 0/1 = pinned

bool filter_enabled()
{
	if (s_env_filter == -2)
	{
		char const *const env = std::getenv("M2VK_S22_FILTER");
		s_env_filter = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
	}
	return (s_env_filter < 0) ? s_option_filter : (s_env_filter != 0);
}

// s22_depth_buffer — SHELVED / DORMANT. The per-pixel depth experiment (a real GREATER_OR_EQUAL depth
// buffer replacing System 22's painter's sort) corrupted ground textures and UVs and broke the layered
// UI — insets, the rear-view mirror, direct 2D primitives — and was never accurate to the hardware
// (System 22 is a sorting rasteriser, not a z-buffer). The menu option was removed before release and
// depth_enabled() is FORCED OFF here, so build_pipeline() never builds the depth pipeline, the no-depth
// companion is never built, and the draw path is the untouched painter's algorithm. The code below
// (s_pipeline_nodepth, the batch split on draw_batch::nodepth, the ooz remap) stays in place, dormant,
// so it can be revisited. Do not re-enable without solving the corruption. See devnotes/zfighting.md.
bool s_option_depth = false;   // still parked by retro_entry; ignored while depth_enabled() is forced off
bool s_depth_pipeline = false;

bool depth_enabled()
{
	return false;   // SHELVED: option removed, code dormant — see the note above and devnotes/zfighting.md
}

// The three view toggles, each parked by retro_entry and each overridable by its M2VK_* switch in the
// standing presence-or-value direction. All are push-constant flag bits read at draw time, so they apply
// on the next frame with no pipeline rebuild. fog defaults ON (the accurate picture); the other two OFF.
bool s_option_fog = true;
int  s_env_fog = -2;   // -2 = not read; -1 = no switch; 0/1 = pinned
bool fog_enabled()
{
	if (s_env_fog == -2)
	{
		char const *const env = std::getenv("M2VK_S22_FOG");
		s_env_fog = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
	}
	return (s_env_fog < 0) ? s_option_fog : (s_env_fog != 0);
}

bool s_option_notex = false;
int  s_env_notex = -2;
bool no_textures_enabled()
{
	if (s_env_notex == -2)
	{
		char const *const env = std::getenv("M2VK_S22_NOTEX");
		s_env_notex = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
	}
	return (s_env_notex < 0) ? s_option_notex : (s_env_notex != 0);
}

// "No Lighting" — shares M2VK_FLAT_LUMA with the Model 2 sink so the switch reads the same on both.
bool s_option_nolight = false;
int  s_env_nolight = -2;
bool no_lighting_enabled()
{
	if (s_env_nolight == -2)
	{
		char const *const env = std::getenv("M2VK_FLAT_LUMA");
		s_env_nolight = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
	}
	return (s_env_nolight < 0) ? s_option_nolight : (s_env_nolight != 0);
}

struct mapped_buffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void          *mapped = nullptr;
	VkDeviceSize   size = 0;
};

// One indexed draw: a run of consecutive quads that share a clip window. Same idea as vk_geom's
// draw_batch — the rectangle is carried in bitmap pixels rather than a VkRect2D, because geom_draw is
// where the attachment extent (and the internal-resolution scale) is known and the clamp belongs.
struct draw_batch
{
	int16_t  left, top, right, bottom;   // inclusive, m_cliprect as MAME spelled it
	uint32_t first_index;
	uint32_t index_count;

	// A "no-depth" batch is direct (pre-projected 2D) quads and sprite tiles: the hardware composites
	// them purely by list order, never z-testing them against the 3D scene. With the depth buffer on
	// they draw through a second pipeline that neither tests nor writes depth, so a direct backing (a
	// rear-view mirror's black fill, a HUD panel) drawn before the 3D behind it does not reject that 3D,
	// and one drawn after sits on top — either way by draw order. Solid 3D polygons are depth batches.
	bool     nodepth = false;

	// The batch's own 1/z range over its solid-3D vertices, for the M2VK_S22_BATCHDUMP diagnostic. The
	// depth remap itself is frame-wide (geom_slot::min/max_ooz), not per batch.
	float    min_ooz = 1e30f, max_ooz = -1e30f;
};

struct geom_slot
{
	mapped_buffer   vertices;
	mapped_buffer   indices;
	mapped_buffer   palette;               // 128 KB, re-uploaded each frame
	mapped_buffer   czram;                 // 32 KB (4 z-fog banks), re-uploaded each frame
	mapped_buffer   gamma;                 // 768 B final gamma LUT, re-uploaded each frame (SS22's is
	                                       // in mixer RAM and changes; plain S22's PROM just re-copies)
	VkDescriptorSet descriptor = VK_NULL_HANDLE;
	uint32_t        capacity = 0;          // quads the vertex/index buffers are sized for
	uint32_t        index_count = 0;       // what this slot's recorded draw will submit

	// The frame's 1/z range over the SOLID-3D vertices (direct 2D quads and sprites excluded — they carry
	// no meaningful z and take the no-depth pipeline), recorded by geom_upload and read by draw_batches to
	// build the depth_scale/depth_bias remap. Only meaningful when the depth buffer is on; a frame with no
	// such polygons leaves max < min, which draw_batches treats as "no usable range" and falls to painter's.
	float           min_ooz = 1e30f, max_ooz = -1e30f;

	// One draw per clip-window run; host-side, so it grows on its own. One entry for a game that never
	// windows the 3D (ridgerac); SS22 letterbox games (tokyowar) alternate a couple of windows a frame.
	std::vector<draw_batch> batches;

	// The prioverchar over-pass: one entry per primitive flagged "priority over the text layer", each a
	// range into the SAME index buffer. Drawn a second time by geom_draw_over, after the OVER text
	// overlay, so those primitives sit above the text. Empty on plain S22 and on SS22 frames with none.
	std::vector<draw_batch> over_batches;
};


//============================================================
//  state
//============================================================

// The record. Written on the emulation thread, read on the frontend's, never at the same time. Quads
// and sprite tiles live in separate vectors; s_order is the single interleaved draw order (the tree
// walk), so painter's order across both kinds is preserved. A sprite is Super System 22 only.
std::vector<quad> s_quads;
uint32_t s_quad_count = 0;
std::vector<sprite_tile> s_sprites;
uint32_t s_sprite_count = 0;

enum : uint8_t { ITEM_QUAD = 0, ITEM_SPRITE = 1 };
struct order_item { uint8_t kind; uint32_t index; };
std::vector<order_item> s_order;
uint32_t s_order_count = 0;

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

// The static, shared texture system. Built once, when the first captured frame has the pointers.
mapped_buffer s_ttdata;
mapped_buffer s_ttattr;
mapped_buffer s_ttmap;
mapped_buffer s_ayx;
mapped_buffer s_sprite;    // gfx(2) sprite tiles; a placeholder on plain S22 (bound, never sampled)
bool s_static_uploaded = false;

VkPipelineLayout      s_pipeline_layout = VK_NULL_HANDLE;
VkPipeline            s_pipeline = VK_NULL_HANDLE;
VkPipeline            s_pipeline_nodepth = VK_NULL_HANDLE;   // direct/sprite: no depth test or write,
                                                            // built only when the depth buffer is on
VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
bool s_ready = false;
bool s_failed = false;

// Reporting, once per run.
bool     s_reported_first = false;
uint64_t s_run_quads = 0;
uint32_t s_max_quads = 0;
uint64_t s_drawn_serial = 0;

// M2VK_NO_SCISSOR=1 collapses every quad's clip window to full-screen — one run for the frame, the
// pre-scissor behaviour. The same attribution switch vk_geom exposes: it answers "did the per-quad
// scissor move these pixels", which for SS22 letterbox games (tokyowar) it does, hard.
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
		vk_log(RETRO_LOG_ERROR, "s22: no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
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
			vk_log(RETRO_LOG_WARN, "s22: device-local host-visible heap could not hold a %llu byte buffer (%s); "
					"falling back to plain host-visible memory\n", (unsigned long long)reqs.size, what);
		alloc.memoryTypeIndex = fallback_type;
		ar = s_fns.allocate_memory(s_device, &alloc, nullptr, &b.memory);
	}
	if (ar != VK_SUCCESS)
	{
		vk_log(RETRO_LOG_ERROR, "s22: vkAllocateMemory failed for a %llu byte buffer (%s): %d\n",
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

// Points a slot's descriptor set at the four shared static buffers and its own palette buffer. Written
// once, after everything exists — the static buffers never move and the palette buffer is fixed size.
void write_descriptor(geom_slot &slot)
{
	const VkBuffer buffers[8] = {
		s_ttdata.buffer, s_ttattr.buffer, s_ttmap.buffer, s_ayx.buffer,
		slot.palette.buffer, slot.czram.buffer, slot.gamma.buffer, s_sprite.buffer };

	VkDescriptorBufferInfo info[8]{};
	VkWriteDescriptorSet write[8]{};
	for (uint32_t i = 0; i < 8; i++)
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

	s_fns.update_descriptor_sets(s_device, 8, write, 0, nullptr);
}

bool build_descriptor_layout()
{
	// Eight storage buffers: the four static tile-system arrays, the per-slot palette, the per-slot
	// z-fog tables (czram), the static plain-S22 gamma LUT, then the static sprite gfx (gfx(2)).
	VkDescriptorSetLayoutBinding bindings[8]{};
	for (uint32_t i = 0; i < 8; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 8;
	layout_info.pBindings = bindings;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &layout_info, nullptr, &s_set_layout),
			"vkCreateDescriptorSetLayout (s22)"))
	{
		return false;
	}

	VkDescriptorPoolSize size{};
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = s_slot_count * 8;

	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = s_slot_count;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	return check(s_fns.create_descriptor_pool(s_device, &pool_info, nullptr, &s_descriptor_pool),
			"vkCreateDescriptorPool (s22)");
}

bool build_pipeline()
{
	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = sizeof(push_block);

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &s_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout),
			"vkCreatePipelineLayout (s22)"))
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

		VkVertexInputAttributeDescription attrs[8]{};
		attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT;     attrs[0].offset = 0;
		attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;  attrs[1].offset = 8;
		attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32_SFLOAT;        attrs[2].offset = 20;
		attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32_UINT;          attrs[3].offset = 24;
		attrs[4].location = 4; attrs[4].binding = 0; attrs[4].format = VK_FORMAT_R32_UINT;          attrs[4].offset = 28;
		attrs[5].location = 5; attrs[5].binding = 0; attrs[5].format = VK_FORMAT_R32_UINT;          attrs[5].offset = 32;
		attrs[6].location = 6; attrs[6].binding = 0; attrs[6].format = VK_FORMAT_R32_UINT;          attrs[6].offset = 36;
		attrs[7].location = 7; attrs[7].binding = 0; attrs[7].format = VK_FORMAT_R32_UINT;          attrs[7].offset = 40;

		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = 8;
		vertex_input.pVertexAttributeDescriptions = attrs;

		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewport_state{};
		viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_state.viewportCount = 1;
		viewport_state.scissorCount = 1;

		// No culling: check_culling already rejected back faces and the fan's winding is whatever the
		// game's vertex order made it, exactly as the software path.
		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// Painter's order (the default): draw in record order, last writer wins, depth test off. The state
		// is still supplied, because the ring's render pass carries a depth attachment (Model 2's) and a
		// pipeline used with it must declare depth-stencil state even when it does nothing.
		//
		// s22_depth_buffer=on instead tests GREATER_OR_EQUAL and writes: the render pass clears depth to
		// 0.0 and s22.vert maps nearest to 1.0, so the nearest fragment wins (GREATER), while a coplanar
		// tie (equal z) falls to the LATER draw (OR_EQUAL) — which keeps the painter's ordering the
		// hardware's per-poly sort key produced, and only the genuine interpenetration it could not
		// express gets resolved per-pixel. Same clear value and format Model 2's geom pass already uses.
		s_depth_pipeline = depth_enabled();
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depth.depthTestEnable = s_depth_pipeline ? VK_TRUE : VK_FALSE;
		depth.depthWriteEnable = s_depth_pipeline ? VK_TRUE : VK_FALSE;
		depth.depthCompareOp = s_depth_pipeline ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
		depth.depthBoundsTestEnable = VK_FALSE;
		depth.stencilTestEnable = VK_FALSE;

		// SS22 poly alpha (renderscanline_poly_ss22's final rgb.blend(dest, ...)) is done here as a
		// per-pixel destination blend: the fragment emits alpha = (0xff - poly_alpha)/255 for a pixel that
		// alpha-blends and 1.0 for an opaque one, and SRC_ALPHA/ONE_MINUS_SRC_ALPHA reproduces the mix.
		// Opaque pixels (every plain-S22 pixel, and every non-alpha SS22 pixel) emit alpha 1.0, so the
		// blend passes the source through unchanged and is bit-exact. The software mix is integer >>8 and
		// this is UNORM float, so an actual alpha pixel carries a small rounding residual — expected, and
		// noted for the SS22 tail (its accuracy ground truth is looser than Model 2's).
		VkPipelineColorBlendAttachmentState blend_attachment{};
		blend_attachment.blendEnable = VK_TRUE;
		blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
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

		// The no-depth companion, built only when the depth buffer is on: identical but for a depth state
		// that neither tests nor writes, so direct 2D quads and sprites composite by draw order alone. In
		// painter's mode the main pipeline is already this, so no companion is needed.
		if (ok && s_depth_pipeline)
		{
			VkPipelineDepthStencilStateCreateInfo nodepth = depth;
			nodepth.depthTestEnable = VK_FALSE;
			nodepth.depthWriteEnable = VK_FALSE;
			nodepth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
			VkGraphicsPipelineCreateInfo ninfo = info;
			ninfo.pDepthStencilState = &nodepth;
			ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &ninfo, nullptr, &s_pipeline_nodepth),
					"vkCreateGraphicsPipelines (s22 no-depth)");
		}
	}

	if (frag != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag, nullptr);
	if (vert != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, vert, nullptr);

	return ok;
}

// Uploads the four static tile-system buffers once. On little-endian hardware the packed uint buffers
// are byte-for-byte the driver's arrays, so each is a plain memcpy — the unpack lives in the shader.
bool upload_static()
{
	if (s_static_uploaded)
		return true;

	texture_ram const &t = get_texture_ram();
	if ((t.ttdata == nullptr) || (t.ttattr == nullptr) || (t.ttmap == nullptr) || (t.ayx == nullptr))
		return false;

	if (!create_buffer(s_ttdata, TTDATA_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 ttdata)")
			|| !create_buffer(s_ttattr, TTATTR_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 ttattr)")
			|| !create_buffer(s_ttmap, TTMAP_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 ttmap)")
			|| !create_buffer(s_ayx, AYX_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 ayx)"))
	{
		return false;
	}

	// The "textile" region is short on cybrcycc/alpinr2b/alpines (0xe/0xc/0xa00000). The GPU buffer is
	// always the full TTDATA_BYTES; copy only what the region actually holds and zero the tail, so a tile
	// index that lands past real data fetches pen 0 rather than walking off the source (EXC_BAD_ACCESS).
	const size_t ttdata_have = t.ttdata_bytes ? std::min<size_t>(t.ttdata_bytes, size_t(TTDATA_BYTES))
			: size_t(TTDATA_BYTES);
	std::memcpy(s_ttdata.mapped, t.ttdata, ttdata_have);
	if (ttdata_have < size_t(TTDATA_BYTES))
		std::memset(static_cast<uint8_t *>(s_ttdata.mapped) + ttdata_have, 0, size_t(TTDATA_BYTES) - ttdata_have);
	std::memcpy(s_ttattr.mapped, t.ttattr, size_t(TTATTR_BYTES));
	std::memcpy(s_ttmap.mapped, t.ttmap, size_t(TTMAP_BYTES));
	std::memcpy(s_ayx.mapped, t.ayx, size_t(AYX_BYTES));

	// The sprite gfx (gfx(2)). On Super System 22 it is the "sprite" region; on plain System 22 there is
	// no gfx(2), so a tiny placeholder keeps binding 7 valid (the sprite path never fires there).
	const bool have_sprite = (t.sprite != nullptr) && (t.sprite_bytes != 0);
	const VkDeviceSize sprite_size = have_sprite ? VkDeviceSize(t.sprite_bytes) : VkDeviceSize(16);
	if (!create_buffer(s_sprite, sprite_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 sprite)"))
		return false;
	if (have_sprite)
		std::memcpy(s_sprite.mapped, t.sprite, size_t(sprite_size));
	else
		std::memset(s_sprite.mapped, 0, size_t(sprite_size));

	s_static_uploaded = true;
	return true;
}

// Everything the draw needs: pipeline, descriptor pool/sets, static tile buffers, per-slot palette
// buffers. Built once, on the first captured frame that has the texture pointers. The Model 2 build —
// which never captures — never reaches here, so it makes no Vulkan call from this file.
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
				"vkAllocateDescriptorSets (s22)"))
		{
			s_failed = true;
			return false;
		}

		if (!create_buffer(slot.palette, PALETTE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 palette)")
				|| !create_buffer(slot.czram, CZRAM_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 czram)")
				|| !create_buffer(slot.gamma, GAMMA_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (s22 gamma)"))
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
	s_quad_count = 0;
	s_sprite_count = 0;
	s_order_count = 0;
	s_valid = false;
}

static void push_order(uint8_t kind, uint32_t index)
{
	if (s_order_count == s_order.size())
		s_order.resize(s_order.size() + (s_order.size() / 2) + INITIAL_QUAD_CAPACITY);
	s_order[s_order_count++] = order_item{ kind, index };
}

void record_quad(quad const &q)
{
	if (s_quad_count == s_quads.size())
		s_quads.resize(s_quads.size() + (s_quads.size() / 2) + INITIAL_QUAD_CAPACITY);
	push_order(ITEM_QUAD, s_quad_count);
	s_quads[s_quad_count++] = q;
}

void record_sprite(sprite_tile const &s)
{
	if (s_sprite_count == s_sprites.size())
		s_sprites.resize(s_sprites.size() + (s_sprites.size() / 2) + INITIAL_QUAD_CAPACITY);
	push_order(ITEM_SPRITE, s_sprite_count);
	s_sprites[s_sprite_count++] = s;
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

	// Everything is built lazily on the first upload that carries geometry, so a build that never
	// captures (the Model 2 core) makes no Vulkan call from here.
	s_ready = false;
	s_failed = false;
	s_static_uploaded = false;
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_nodepth = VK_NULL_HANDLE;
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
		destroy_buffer(slot.czram);
		destroy_buffer(slot.gamma);
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;
	}
	destroy_buffer(s_ttdata);
	destroy_buffer(s_ttattr);
	destroy_buffer(s_ttmap);
	destroy_buffer(s_ayx);
	destroy_buffer(s_sprite);

	if (s_pipeline != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline, nullptr);
	if (s_pipeline_nodepth != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline_nodepth, nullptr);
	if (s_pipeline_layout != VK_NULL_HANDLE)
		s_fns.destroy_pipeline_layout(s_device, s_pipeline_layout, nullptr);
	if (s_descriptor_pool != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_pool(s_device, s_descriptor_pool, nullptr);
	if (s_set_layout != VK_NULL_HANDLE)
		s_fns.destroy_descriptor_set_layout(s_device, s_set_layout, nullptr);

	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_nodepth = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_ready = false;
	s_failed = false;
	s_static_uploaded = false;
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
		slot.czram = mapped_buffer{};
		slot.gamma = mapped_buffer{};
		slot.capacity = 0;
		slot.index_count = 0;
		slot.descriptor = VK_NULL_HANDLE;
	}
	s_ttdata = mapped_buffer{};
	s_ttattr = mapped_buffer{};
	s_ttmap = mapped_buffer{};
	s_ayx = mapped_buffer{};
	s_sprite = mapped_buffer{};
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_ready = false;
	s_failed = false;
	s_static_uploaded = false;
	s_render_pass = VK_NULL_HANDLE;
	s_slot_count = 0;
	s_device = VK_NULL_HANDLE;
}

bool geom_upload(uint32_t slot_index)
{
	// Before any Vulkan work, so the Model 2 build (which never captures) returns here every frame and
	// never builds anything.
	if (!s_valid || (s_order_count == 0) || (slot_index >= s_slot_count))
		return false;
	if (!ensure_ready())
		return false;

	geom_slot &slot = s_slots[slot_index];
	// Each item (quad or sprite tile) fits in one per-quad slot (6 verts / 12 indices), so size by the
	// total item count. A sprite tile uses 4 verts / 6 indices, comfortably within that.
	if ((slot.capacity < s_order_count) && !size_slot(slot, s_order_count + (s_order_count / 2)))
		return false;

	// The palette is the one thing in the tile system that changes per frame; re-upload the live pens.
	if (texture_ram const &t = get_texture_ram(); t.palette != nullptr)
		std::memcpy(slot.palette.mapped, t.palette, size_t(PALETTE_BYTES));

	// The four z-fog tables change per frame too (recalc_czram); re-upload them concatenated. A plain
	// System 22 game has no czram (the tables are SS22-only), so a null bank is zero-filled — its quads
	// never set zfog anyway, so the contents are never read.
	{
		shading_globals const &g = get_shading_globals();
		auto *const cz = static_cast<uint8_t *>(slot.czram.mapped);
		for (int b = 0; b < 4; b++)
		{
			if (g.czram[b] != nullptr)
				std::memcpy(cz + b * size_t(CZRAM_BANK), g.czram[b], size_t(CZRAM_BANK));
			else
				std::memset(cz + b * size_t(CZRAM_BANK), 0, size_t(CZRAM_BANK));
		}
	}

	// The final gamma LUT (rlut|glut|blut). Plain System 22's is a static PROM, Super System 22's is in
	// mixer RAM and changes per frame; either way the driver hands over the active source each frame. A
	// null LUT (a game with no gamma) is an identity ramp so the shader step is a no-op.
	{
		auto *const gm = static_cast<uint8_t *>(slot.gamma.mapped);
		if (texture_ram const &t = get_texture_ram(); t.gamma != nullptr)
			std::memcpy(gm, t.gamma, size_t(GAMMA_BYTES));
		else
			for (int c = 0; c < 3; c++)
				for (int i = 0; i < 0x100; i++)
					gm[c * 0x100 + i] = uint8_t(i);
	}

	auto *const verts = static_cast<gpu_vertex *>(slot.vertices.mapped);
	auto *const idx = static_cast<uint32_t *>(slot.indices.mapped);
	uint32_t vcount = 0, icount = 0;
	slot.batches.clear();
	slot.over_batches.clear();
	slot.min_ooz = 1e30f;
	slot.max_ooz = -1e30f;

	// Walk the interleaved draw order (the tree walk) so quads and sprites keep their relative depth: the
	// painter's pass draws them in this exact sequence, last writer wins. One pipeline handles both; the
	// vertex's ATTR_SPRITE flag switches the fragment fetch.
	for (uint32_t oi = 0; oi < s_order_count; oi++)
	{
		const order_item it = s_order[oi];

		int16_t rcl, rct, rcr, rcb;
		int prioverchar;
		bool item_nodepth;   // direct 2D quad or sprite: composited by order, never depth-tested
		uint32_t nq = 0;   // quad vertex count; 0 marks a sprite tile below
		if (it.kind == ITEM_QUAD)
		{
			quad const &q = s_quads[it.index];
			nq = q.num_verts;
			if (nq < 3)
				continue;
			if (nq > MAX_QUAD_VERTS)
				nq = MAX_QUAD_VERTS;
			rcl = q.clip_l; rct = q.clip_t; rcr = q.clip_r; rcb = q.clip_b;
			prioverchar = q.prioverchar;
			item_nodepth = q.direct;
		}
		else
		{
			sprite_tile const &s = s_sprites[it.index];
			rcl = s.clip_l; rct = s.clip_t; rcr = s.clip_r; rcb = s.clip_b;
			prioverchar = s.prioverchar;
			item_nodepth = true;
		}

		// The item's clip window, or full-screen under the attribution switch.
		const int16_t cl = no_scissor() ? int16_t(0)   : rcl;
		const int16_t ct = no_scissor() ? int16_t(0)   : rct;
		const int16_t cr = no_scissor() ? int16_t(639) : rcr;
		const int16_t cb = no_scissor() ? int16_t(479) : rcb;

		// Start a new run whenever the clip window OR the depth mode changes; the walk is in draw order,
		// so a run is a maximal span of consecutive items that share a scissor and a pipeline (depth vs
		// no-depth). geom_draw binds the matching pipeline per batch.
		if (slot.batches.empty()
				|| (slot.batches.back().left != cl) || (slot.batches.back().top != ct)
				|| (slot.batches.back().right != cr) || (slot.batches.back().bottom != cb)
				|| (slot.batches.back().nodepth != item_nodepth))
		{
			draw_batch nb{ cl, ct, cr, cb, icount, 0 };
			nb.nodepth = item_nodepth;
			slot.batches.push_back(nb);
		}

		const uint32_t item_first = icount;

		if (it.kind == ITEM_QUAD)
		{
			quad const &q = s_quads[it.index];

			const uint32_t attr = (q.textured ? ATTR_TEXTURED : 0u)
					| (q.shade_enabled ? ATTR_SHADE : 0u)
					| ((uint32_t(q.color) & 0x7fu) << 8u)
					| ((uint32_t(q.cmode) & 0xffu) << 16u);
			const uint32_t bn = uint32_t(q.texturebank);
			const uint32_t base = q.basecolor;

			// The per-quad shading-tail words. sf0 packs the small scalars and flags; sf1 is the fog colour.
			// cz_sdelta is signed (-256..255), stored biased by 256 into a 9-bit field.
			const uint32_t sf0 = uint32_t(q.fogfactor)
					| ((uint32_t(q.cz_bank) & 3u) << 8u)
					| (q.zfog_enabled ? SF0_ZFOG : 0u)
					| (q.alpha_enabled ? SF0_ALPHA_EN : 0u)
					| (q.ss22 ? SF0_SS22 : 0u)
					| ((uint32_t(int(q.cz_sdelta) + 256) & 0x1ffu) << 13u);
			const uint32_t sf1 = q.fogcolor & 0x00ffffffu;

			const uint32_t vbase = vcount;
			for (uint32_t i = 0; i < nq; i++)
			{
				gpu_vertex &v = verts[vcount++];
				v.x = q.x[i];
				v.y = q.y[i];
				v.uoz = q.uoz[i];
				v.voz = q.voz[i];
				v.ooz = q.ooz[i];
				v.iw = q.bri[i];
				if (!item_nodepth)                                      // depth-remap range, solid 3D only
				{                                                       // (direct 2D carries a sentinel z);
					draw_batch &cb2 = slot.batches.back();              // per batch for a scissored inset,
					if (q.ooz[i] < cb2.min_ooz) cb2.min_ooz = q.ooz[i]; // per frame for the main scene
					if (q.ooz[i] > cb2.max_ooz) cb2.max_ooz = q.ooz[i];
					if (q.ooz[i] < slot.min_ooz) slot.min_ooz = q.ooz[i];
					if (q.ooz[i] > slot.max_ooz) slot.max_ooz = q.ooz[i];
				}
				v.attr = attr;
				v.bn = bn;
				v.base = base;
				v.sf0 = sf0;
				v.sf1 = sf1;
			}
			for (uint32_t i = 1; i + 1 < nq; i++)
			{
				idx[icount++] = vbase;
				idx[icount++] = vbase + i;
				idx[icount++] = vbase + i + 1;
			}
		}
		else
		{
			sprite_tile const &s = s_sprites[it.index];

			// A sprite reuses the vertex layout: affine u/v ride uoz/voz with ooz = 1 (no perspective), the
			// pal bank rides attr (color<<8, the poly pens_base), the tile byte offset rides bn, the fade
			// colour rides base, and sf0/sf1 carry the sprite shading tail (own layout, see the constants).
			const uint32_t attr = ATTR_SPRITE
					| (s.flipx ? ATTR_SFLIPX : 0u)
					| (s.flipy ? ATTR_SFLIPY : 0u)
					| ((uint32_t(s.color) & 0x7fu) << 8u);
			const uint32_t bn = s.code_base;
			const uint32_t base = s.fadecolor & 0x00ffffffu;
			const uint32_t sf0 = uint32_t(s.fogfactor)
					| (uint32_t(s.fadefactor) << 8u)
					| (uint32_t(s.alpha) << 16u)
					| (s.alpha_enabled ? SSF_ALPHA_EN : 0u);
			const uint32_t sf1 = s.fogcolor & 0x00ffffffu;

			const uint32_t vbase = vcount;
			for (int i = 0; i < 4; i++)
			{
				gpu_vertex &v = verts[vcount++];
				v.x = s.x[i];
				v.y = s.y[i];
				v.uoz = s.u[i];
				v.voz = s.v[i];
				v.ooz = 1.0f;
				v.iw = 0.0f;
				v.attr = attr;
				v.bn = bn;
				v.base = base;
				v.sf0 = sf0;
				v.sf1 = sf1;
			}
			idx[icount++] = vbase;     idx[icount++] = vbase + 1; idx[icount++] = vbase + 2;
			idx[icount++] = vbase;     idx[icount++] = vbase + 2; idx[icount++] = vbase + 3;
		}

		slot.batches.back().index_count = icount - slot.batches.back().first_index;

		// A prioverchar primitive (SS22 only) is redrawn over the text; record its index range so
		// geom_draw_over can replay it after the OVER overlay. Its triangles are contiguous here.
		if ((s_variant == 1) && (prioverchar & 1) && (icount > item_first))
			slot.over_batches.push_back(draw_batch{ cl, ct, cr, cb, item_first, icount - item_first });
	}

	slot.index_count = icount;

	// M2VK_S22_BATCHDUMP=1 — one line per scissor batch: its clip window and index count, plus the
	// frame's global 1/z range. Diagnostic only; costs nothing when unset.
	if (char const *const bd = std::getenv("M2VK_S22_BATCHDUMP"); bd && *bd)
	{
		static uint64_t last = ~0ull;
		if (s_serial != last)
		{
			last = s_serial;
			vk_log(RETRO_LOG_INFO, "s22 batchdump serial=%llu batches=%u over=%u\n",
					(unsigned long long)s_serial,
					unsigned(slot.batches.size()), unsigned(slot.over_batches.size()));
			for (size_t bi = 0; bi < slot.batches.size(); bi++)
			{
				draw_batch const &b = slot.batches[bi];
				vk_log(RETRO_LOG_INFO, "  batch %2zu clip=(%d,%d)-(%d,%d) idx=%u ooz=[%g..%g] %s\n",
						bi, int(b.left), int(b.top), int(b.right), int(b.bottom), unsigned(b.index_count),
						b.min_ooz, b.max_ooz, b.nodepth ? "NODEPTH" : "depth");
			}
		}
	}

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
		vk_log(RETRO_LOG_INFO, "s22: first GPU geometry — %u quads, %u sprite tiles, %u indices (%s)\n",
				unsigned(s_quad_count), unsigned(s_sprite_count), unsigned(icount), s_variant ? "ss22" : "s22");
	}

	return icount != 0;
}

// Binds the pipeline/descriptor/buffers, pushes the per-frame globals, then draws a batch list — one
// scissored indexed draw per clip-window run — and restores the full attachment scissor. Shared by the
// main pass (slot.batches) and the prioverchar over-pass (slot.over_batches): the two differ only in
// which batch list they replay and when the caller invokes them, not in state.
static void draw_batches(VkCommandBuffer cmd, geom_slot &slot, std::vector<draw_batch> const &batches,
		unsigned width, unsigned height, unsigned draw_width, unsigned draw_height)
{
	// The VISIBLE half-extent, resolution-invariant for the reason s22.vert gives: the vertex shader
	// turns bitmap pixels into NDC, so the attachment size never enters here. The caller has already set
	// the viewport and scissor to the (possibly larger) attachment extent.
	push_block push{};
	push.half_width = float(width) * 0.5f;
	push.half_height = float(height) * 0.5f;

	// The SS22 shading tail's per-frame globals — screen fade, poly fade, poly alpha — the same for every
	// quad. The z-fog tables came over the czram buffer; these ride the push constant.
	shading_globals const &g = get_shading_globals();
	if (char const *const dbg = std::getenv("M2VK_S22_SHADEDUMP"); dbg && *dbg)
	{
		static uint64_t last = ~0ull;
		if (s_serial != last)
		{
			last = s_serial;
			vk_log(RETRO_LOG_INFO, "s22 globals: pfade=%d poly=(%d,%d,%d) fade_factor=%d fade=(%d,%d,%d) alpha_factor=%d alpha_pen=%d\n",
					g.poly_fade_enabled ? 1 : 0, g.poly_r, g.poly_g, g.poly_b,
					g.fade_factor, g.fade_r, g.fade_g, g.fade_b, g.alpha_factor, g.alpha_pen);
		}
	}
	push.alpha_pen    = uint32_t(g.alpha_pen & 0xff);
	push.alpha_factor = uint32_t(g.alpha_factor & 0xff);
	push.fade_factor  = uint32_t(g.fade_factor & 0xff);
	push.fade_r       = uint32_t(g.fade_r & 0xffff);
	push.fade_g       = uint32_t(g.fade_g & 0xffff);
	push.fade_b       = uint32_t(g.fade_b & 0xffff);
	push.poly_flags   = (g.poly_fade_enabled ? PFLAG_POLY_FADE : 0u)
			| (fog_enabled()         ? 0u : PFLAG_NO_FOG)
			| (no_textures_enabled() ? PFLAG_NO_TEX : 0u)
			| (no_lighting_enabled() ? PFLAG_NO_LIGHT : 0u);
	push.tex_filter   = filter_enabled() ? 1u : 0u;

	// The depth remap: painter's (scale 0 / bias 0.5, a constant z) for the depth-off pipeline and for a
	// frame that spans no 1/z range; else z = ooz*scale + bias mapping the frame-wide [min, max] onto
	// [0, 1] so nearest (largest 1/z) is 1.0, matching GREATER_OR_EQUAL. Solid-3D batches all share it.
	push.depth_scale  = 0.0f;
	push.depth_bias   = 0.5f;
	if (s_depth_pipeline)
	{
		const float span = slot.max_ooz - slot.min_ooz;
		if (span > 1e-9f)
		{
			push.depth_scale = 1.0f / span;
			push.depth_bias  = -slot.min_ooz / span;
		}
	}
	push.poly_r       = uint32_t(g.poly_r & 0xffff);
	push.poly_g       = uint32_t(g.poly_g & 0xffff);
	push.poly_b       = uint32_t(g.poly_b & 0xffff);

	const VkDeviceSize offset = 0;
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
			0, 1, &slot.descriptor, 0, nullptr);
	s_fns.cmd_push_constants(cmd, s_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s_fns.cmd_bind_vertex_buffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
	s_fns.cmd_bind_index_buffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);

	// The pipeline is bound per batch below: solid 3D through s_pipeline (depth on/off per the option),
	// direct 2D quads and sprites through s_pipeline_nodepth when the depth buffer is on. In painter's
	// mode there is no companion, so everything binds s_pipeline. -1 forces the first bind.
	int bound = -1;

	// One draw per clip-window run, each with its own scissor. The rectangle is inclusive in bitmap
	// pixels; the caller passes width/height as the visible extent MAME clipped against (640x480), so an
	// m_destmap pixel spans scale_x by scale_y attachment pixels — the same internal-resolution scale
	// vk_geom applies, rounded OUTWARD so a fractional scale never shaves a boundary column. Most games
	// window nothing (one full-screen run), so the scissor is a no-op there; SS22 letterbox games window
	// the 3D into a black-barred viewport, which is the whole point of applying it (tokyowar's sky).
	const float scale_x = float(draw_width) / float(width);
	const float scale_y = float(draw_height) / float(height);

	VkRect2D set_to{};
	bool     scissor_set = false;
	for (draw_batch const &b : batches)
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

		// Bind the pipeline this batch needs: the no-depth companion for a direct/sprite batch when the
		// depth buffer is on, the main pipeline otherwise.
		const int want = (s_depth_pipeline && b.nodepth) ? 1 : 0;
		if (want != bound)
		{
			s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
					want ? s_pipeline_nodepth : s_pipeline);
			bound = want;
		}

		s_fns.cmd_draw_indexed(cmd, b.index_count, 1, b.first_index, 0, 0);
	}

	// Put the full extent back: the caller drew the 2D under-layer before this and draws the over layer
	// after, so a leftover polygon window would clip the foreground overlay to it. (Mirrors vk_geom.)
	VkRect2D full{};
	full.offset = { 0, 0 };
	full.extent = { draw_width, draw_height };
	s_fns.cmd_set_scissor(cmd, 0, 1, &full);
}

void geom_draw(uint32_t slot_index, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height)
{
	if (!s_ready || (slot_index >= s_slot_count))
		return;

	geom_slot &slot = s_slots[slot_index];
	if ((slot.index_count == 0) || (slot.descriptor == VK_NULL_HANDLE))
		return;

	draw_batches(cmd, slot, slot.batches, width, height, draw_width, draw_height);
}

void geom_draw_over(uint32_t slot_index, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height)
{
	if (!s_ready || (slot_index >= s_slot_count))
		return;

	geom_slot &slot = s_slots[slot_index];
	if (slot.over_batches.empty() || (slot.descriptor == VK_NULL_HANDLE))
		return;

	draw_batches(cmd, slot, slot.over_batches, width, height, draw_width, draw_height);
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
	s_sprite_count = 0;
	s_order_count = 0;
}

// system22_texture_filter, parked by retro_entry (in the load block and on every live change). Read at
// the top of each draw via filter_enabled(), which lets M2VK_S22_FILTER override it. No pipeline to
// rebuild — it is a push-constant bit — so the toggle takes effect on the next drawn frame.
void set_option_filter(bool on)
{
	s_option_filter = on;
}

// s22_depth_buffer, parked by retro_entry at load. Read once at build_pipeline() (the depth-stencil
// state is baked into the pipeline), so it is reload-gated; M2VK_S22_DEPTH overrides it.
void set_option_depth(bool on)
{
	s_option_depth = on;
}

// system22_fog / system22_no_textures / No Lighting, parked by retro_entry (at load and on every live
// change). Read at the top of each draw via the *_enabled() helpers, which let the M2VK_* switch
// override; push-constant flag bits, so the toggle takes effect on the next drawn frame.
void set_option_fog(bool on)
{
	s_option_fog = on;
}

void set_option_no_textures(bool on)
{
	s_option_notex = on;
}

void set_option_no_lighting(bool on)
{
	s_option_nolight = on;
}

// The primitive count of the most recently recorded scene — quads plus sprite tiles — for the polygon
// counter HUD. Reset at record_begin and accumulated as the seam submits, so after a frame it is that
// frame's total; a dupe frame that records nothing new leaves the last scene's count in place.
uint32_t geom_primitive_count()
{
	return s_quad_count + s_sprite_count;
}

} // namespace s22
