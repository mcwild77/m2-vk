// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the polygon pass. See vk_geom.h for the three decisions this rests on.

    The shape of one frame here is: walk the record's polygon stream in draw order, fan each polygon
    to n-2 triangles, write vertices, indices and one parameter block per polygon straight into
    mapped device memory, and record one indexed draw per run of polygons that share a scissor
    rectangle. Nothing else varies per polygon — no pipeline change, no descriptor rebind — so a game
    with one viewport for the whole frame is still a single draw, which is nearly all of them.

    There are THREE pipelines. The first two differ in one thing: whether the fragment shader can
    discard. A polygon that is neither translucent nor checkered cannot, so it takes the variant that
    declares EarlyFragmentTests and has its depth resolved before the shader runs; everything else
    takes the late-test variant, because a discarded fragment must leave the draw-order key unclaimed.
    The predicate is the two flag bits and nothing else — see the note above the split in geom_upload.
    The third exists only under the model2_transparency option and is described at the deferred list.

    Four traps that are already paid for and must stay paid:

      * The scissor comes from the polygon's own clipped viewport and it is not decoration. MAME
        passes `vp` — the game's viewport registers intersected with the visible rectangle — to
        render_triangle for every polygon, so a polygon whose geometry strays outside its viewport is
        cut off there. Games that use an inset window (vcop2's demo panel) depend on it, and the
        geometry merely happening to stay inside the panel is not the same thing. Scissored fragments
        never reach the depth test, so a cut-off pixel leaves the draw-order key unclaimed for a
        later polygon — exactly as an unwritten m_fillmap entry does.

      * Indices are 32-bit. Primitive restart cannot be disabled on this implementation — MoltenVK
        says so once per pipeline creation, because Metal has no way to turn it off — so an index of
        0xffff with 16-bit indices would restart the primitive whether or not the pipeline asked.
        32-bit indices put the restart value at 0xffffffff, which the vertex count cannot reach:
        1450 polygons at 8 vertices is 11600, and the capacity check below refuses anything that
        would come near it.

      * Batches follow SUBMISSION ORDER and are never regrouped to save a draw. The depth key makes
        the final picture order-independent — the winner of a pixel is the lowest record index that
        covers it and does not discard, whatever sequence the draws arrive in — so all the early-Z
        polygons could legally be swept into one draw and all the rest into another. That is deliberately
        not done: it would rest the picture on that argument being airtight, to buy draw calls back in a
        phase where performance.md §2a says the whole optimisation list is bidding for 4.5 % of a frame.
        If the draw count ever does become the problem, this paragraph is the escape hatch.

      * The parameter buffer is indexed by the polygon's position in the *record*, not by its
        position among the polygons that were actually drawn. Skipped polygons leave a dead 16-byte
        entry, which is the right trade: the draw-order depth key comes from the same index, so when
        the textured paths arrive they slot in at the depth they always had rather than shifting
        everything that follows.

    Window ordering costs nothing here and that is worth stating, because it looks like something that
    ought to need handling. render_polygons walks `for (window = cur_window; window >= 0; window--)`,
    so higher-numbered windows reach the seam FIRST and win pixels against everything behind them.
    The record is in seam order and the draw-order depth key is the record index, so the priority the
    window loop expresses is already in the key. There is nothing to sort and nothing to group by
    window; the log line below asserts the record really is window-descending rather than assuming it.

*********************************************************************************************************************************/

#include "renderer_vk/vk_geom.h"

#include "renderer_vk/shaders/poly_early_frag_spv.h"
#include "renderer_vk/shaders/poly_frag_spv.h"
#include "renderer_vk/shaders/poly_vert_spv.h"

#include "m2vk_frame.h"
#include "m2vk_sink.h"

#include <array>
#include <cmath>
#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <vector>


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

// 1 - n/65536 for polygon n in draw order. The width is not a guess against an observed frame: MAME
// fatalerrors above MAX_POLYGONS = 32768 polygons in a frame (model2.h), so 16 bits has exactly 2x
// headroom over a limit the emulation enforces before we ever see the stream, and the clamp below
// can never fire. Keys land in (0.5, 1.0], where a float32 ULP is ~6e-8 against a key step of
// 1.5e-5 — every polygon is ~256 ULP from its neighbours, and D32_SFLOAT represents every one of
// these values exactly.
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
	float    smooth_luma;   // model2_smooth_shading: welded per-vertex luma; = poly luma when the option is off
};

static_assert(sizeof(gpu_vertex) == 32, "the vertex attribute offsets below are written out by hand");

// std430, sixteen scalar words, so the array stride is a plain 64 bytes. Mirrors the poly_params
// struct at the top of poly.frag, field for field and in order.
struct gpu_poly
{
	uint32_t palcolor;
	uint32_t luma;
	uint32_t flags;
	int32_t  texlod;

	uint32_t texwidth;
	uint32_t texheight;
	uint32_t texx;
	uint32_t texy;

	uint32_t lumabase;
	uint32_t sheet;
	uint32_t utexx;
	uint32_t utexy;

	uint32_t utexminlod;
	int32_t  max_level;
	uint32_t reserved0;
	uint32_t reserved1;
};

static_assert(sizeof(gpu_poly) == 64, "poly.frag's poly_params must match this word for word");

enum : uint32_t
{
	FLAG_TRANSLUCENT = 1,
	FLAG_TEXTURED    = 2,
	FLAG_CHECKER     = 4,
	FLAG_WRAPX       = 8,
	FLAG_WRAPY       = 16,
	FLAG_MIRRORX     = 32,
	FLAG_MIRRORY     = 64,
	FLAG_UTEX        = 128
};

// Both sheets in one buffer, sheet 0 first. 2 MB, which is the entirety of Model 2's texture memory
// and the reason there is no atlas, no cache and no page decode anywhere in this renderer.
constexpr uint32_t TEXRAM_TOTAL_WORDS = m2vk::TEXRAM_SHEET_WORDS * 2;

// The polygon pass's push constant block, shared by both stages. The vertex shader takes the visible
// picture's half-extent in pixels; the fragment shader takes the stipple divisor, and takes it for
// exactly one reason — the `checker` screen door is the one thing in the shader that is measured in
// pixels rather than in varyings, so it has to be told how many ATTACHMENT pixels a square spans.
// Everything else there is already resolution-invariant because it works off the varyings.
//
// A push constant rather than a specialisation constant even though the divisor is fixed for the life
// of a pipeline: the value is already in geom_draw()'s hand, and a spec constant would mean rebuilding
// both pipelines to change it, which is the pipeline cache's problem rather than the caller's.
struct gpu_push
{
	float half_width, half_height;
	uint32_t stipple_div;
	uint32_t blend;             // model2_transparency: 0 stipples the screen door, 1 defers and blends
};

static_assert(sizeof(gpu_push) == 16, "poly.vert/poly.frag's push block is four words");


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

// One indexed draw: a run of consecutive polygons that share a clipped viewport AND a pipeline. The
// rectangle is carried in m_destmap pixels rather than as a VkRect2D, because geom_draw is where the
// framebuffer extent is known and therefore where the clamp — and, at P5, the internal-resolution
// scale — belongs.
struct draw_batch
{
	int32_t  left, top, right, bottom;  // inclusive, exactly as MAME's rectangle spells it
	uint32_t first_index;
	uint32_t index_count;
	uint8_t  pipe;                      // an index into s_pipelines; see PIPE_* below
};

// Which of the three pipelines a batch wants. GENERAL and EARLY interleave through the stream in
// submission order; BLEND batches are all appended after every one of them, which is what makes the
// blended-transparency pass a second pass rather than a third kind of polygon in the first.
enum : uint8_t
{
	PIPE_GENERAL = 0,
	PIPE_EARLY   = 1,
	PIPE_BLEND   = 2,
	PIPE_COUNT   = 3
};

// One polygon held back for the blended pass: everything the index emission needs, since its vertices
// and its parameter block were already written in stream order and only its indices are deferred.
struct deferred_poly
{
	uint32_t base;                      // first vertex of the fan
	uint32_t nverts;
	int32_t  left, top, right, bottom;  // its own clipped viewport, as draw_batch spells it
};

struct geom_slot
{
	mapped_buffer   vertices;
	mapped_buffer   indices;
	mapped_buffer   params;
	mapped_buffer   colorxlat;
	mapped_buffer   lumaram;
	mapped_buffer   texram;

	VkDescriptorSet descriptor = VK_NULL_HANDLE;

	uint32_t        capacity = 0;       // polygons the three per-frame buffers are sized for
	uint32_t        index_count = 0;    // what this slot's recorded draws will submit in total
	uint64_t        tables_serial = 0;  // what is in colorxlat/lumaram right now
	bool            tables_valid = false;

	// Host-side only, so it grows on its own rather than with size_slot. One entry in the common case
	// and it reaches the run's high-water mark within a few frames, same as everything else here.
	std::vector<draw_batch> batches;

	// The polygons the blended-transparency pass owes, in stream order. Empty on the accurate path,
	// which is the default and the whole of what the A/B harness runs; a member rather than a local so
	// that the option costs no allocation per frame once a run is going.
	std::vector<deferred_poly> deferred;
};

std::array<geom_slot, MAX_SLOTS> s_slots{};
uint32_t s_slot_count = 0;

VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout      s_pipeline_layout = VK_NULL_HANDLE;

// Indexed by draw_batch::pipe. [0] is the general variant, which can discard and therefore tests
// depth late; [1] declares EarlyFragmentTests and is only ever selected for polygons whose fragment
// shader has no discard in it at all; [2] is the blended-transparency pass. Same layout, same
// descriptor set, same shaders — [2] differs from [0] only in its depth-write and blend state.
VkPipeline            s_pipelines[PIPE_COUNT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };

const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

// M2VK_NO_3D=1 draws neither the software 3D nor the hardware 3D, leaving the two tilemap layers
// with a hole between them. That is the reference picture the coverage comparison differences
// against: it is bit-identical under both renderers, because neither of them touches those pixels.
bool s_no_3d = false;
bool s_no_3d_known = false;

// M2VK_NO_SCISSOR=1 goes back to one unscissored draw for the whole frame — the pre-step-6 behaviour.
// It is an attribution tool, not a symmetric harness switch: MAME's rasteriser always clips to `vp`
// and there is no way to ask it not to, so this makes the two paths differ on purpose. Its use is
// answering "did the scissor move these pixels", which it does in one run.
bool s_no_scissor = false;
bool s_no_scissor_known = false;

// M2VK_NO_EARLY_Z=1 sends every polygon to the general pipeline, which is the pre-split behaviour and
// one draw again. Unlike M2VK_NO_SCISSOR this is a pure no-op switch: it must not move a single pixel
// on either renderer, so "digests equal with it on and off" is the whole verification of the split.
bool s_no_early_z = false;
bool s_no_early_z_known = false;

// model2_transparency, parked here by retro_load_game and re-parked whenever the option changes.
// M2VK_BLEND overrides it — the standing rule, and it takes a value rather than a presence so that a
// harness run can pin the accurate path ON as well as off. -1 is "no switch set".
bool s_option_blend = false;
int  s_env_blend = -1;
bool s_env_blend_known = false;

// Latched at the top of each upload so that the whole frame agrees with itself: geom_upload decides
// which polygons to defer, and geom_draw pushes the flag the shader reads, and a change landing between
// the two would stipple polygons that were deferred — i.e. draw them twice, once with a screen door.
bool s_blend_frame = false;

// Said once rather than 57 times a second.
bool s_reported_skips = false;

// The window/scissor report, also once. Two things worth asserting rather than assuming: that the
// record really arrives window-descending (the draw-order depth key is the record index, so if it did
// not, window priority would be silently inverted) and how many separate scissor runs a frame costs.
bool s_reported_windows = false;

// Maxima over the whole run, reported at geom_end_run. This is the measurement that answers "does
// this game scissor at all", which the once-only line above cannot: it latches on the first frame
// with either more than one window or more than one scissor run, and those are not the same frame.
uint32_t s_max_windows = 0;
uint32_t s_max_batches = 0;
uint32_t s_offscreen_total = 0;
bool     s_windows_ascending_seen = false;

// The pipeline split's own numbers, because its cost and its benefit are the same quantity seen twice:
// the share of polygons that get early-Z is what it buys, and the batches those polygons break the
// stream into is what it costs. A game that is nearly all translucent (sgt24h, overrev) can only lose
// here, and the report is how that is seen rather than assumed.
uint64_t s_early_polys = 0;
uint64_t s_drawn_polys = 0;

// The current frame's submitted polygon count, for the polygon-counter HUD. Latched in geom_upload once
// the frame commits to drawing (a dupe re-draws last frame's list, so its count is right too); read by
// geom_frame_polys(). An empty or dropped frame returns early without touching this, so the counter
// holds the last real number rather than flashing 0.
uint32_t s_frame_polys = 0;

// Polygons the blended pass took, over the run. Reported for the same reason the early-Z share is: it
// is the one number that says the option reached the geometry rather than merely being read, and a run
// with the option on and zero here is a stream with no checkered polygons in it — which the feature
// survey says does not exist, since all 29 games use the stipple.
uint64_t s_blend_polys = 0;

// Polygons the per-polygon scissor actually cuts *and the previous build did not*, which is a much
// narrower thing than it first looks. Every game has tens of thousands of polygons a run reaching
// outside their viewport, but in almost all of them the viewport is the whole visible screen — and
// that was already the scissor, and before that the NDC clip. So the count here is deliberately
// restricted to polygons cut by a viewport TIGHTER than the visible rectangle: the ones whose picture
// changed at step 6. Always on; it is a few float compares against vertices already in registers.
uint32_t s_clipped_polys = 0;
uint32_t s_clipped_frames = 0;

// How far outside, in pixels. This is the number that decides whether the scissor is load-bearing or
// merely correct: the geometry engine has ALREADY clipped every polygon against four frustum planes
// built from the same viewport registers clip[] comes from (model2_v.cpp's clip_plane[]), so a cut of
// a fraction of a pixel is projection rounding and a cut of many pixels would be a second, real cut.
float s_clipped_worst = 0.0f;

// The three ways a frame can produce no new draw, counted rather than assumed, because two of them are
// correct and one is a bug and they are not distinguishable by looking at the picture.
//
//   dupes   — render_polygons took its m_render_done early return: the geometrizer has not presented a
//             new list, the record still holds last frame's stream, and re-uploading and redrawing it
//             is the "keep last frame's 3D" case with no code of its own. MAME re-copies its own
//             destmap on that path, so redrawing is what agrees with it.
//   empty   — the display list was new and had nothing in it. MAME draws no 3D at all for such a frame,
//             so matching it means drawing nothing. The record can only say this because the core
//             brackets that early return too (m2vk::frame_begin(0, ...)); before it did, an empty list
//             was indistinguishable from a dupe here and the GPU redrew a stale list for the rest of
//             the run — vstriker composited a football pitch under the copyright card. So this count
//             is the evidence that the notification is arriving, and it is the only thing on this side
//             that can show it: EVERY game boots through empty frames, so a run reporting zero of them
//             means the core has stopped telling the record about empty lists and the stale-3D bug is
//             back. The second figure — empty frames after the 3D had been drawn once — is the part
//             the bug was actually about, and is where vstriker's football pitch used to live.
//   dropped — the 3D went missing from a frame that should have had it, AFTER it had once been drawn.
//             That is the 3D layer flickering and it must stay zero. Deliberately NOT fired for the
//             `empty` case, which is a correct blank rather than a lost one.
uint64_t s_seen_serial = 0;
uint32_t s_dupe_frames = 0;
uint32_t s_empty_frames = 0;
uint32_t s_empty_after_draw = 0;
uint32_t s_dropped_frames = 0;
bool     s_drew_once = false;


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

bool no_scissor()
{
	if (!s_no_scissor_known)
	{
		s_no_scissor = (std::getenv("M2VK_NO_SCISSOR") != nullptr);
		s_no_scissor_known = true;
	}
	return s_no_scissor;
}

bool no_early_z()
{
	if (!s_no_early_z_known)
	{
		s_no_early_z = (std::getenv("M2VK_NO_EARLY_Z") != nullptr);
		s_no_early_z_known = true;
	}
	return s_no_early_z;
}

// The option, unless the switch is set. Anything M2VK_BLEND cannot be read as a number is taken as 1,
// because the reason to type it at all is to turn the thing on.
bool blend_stipple()
{
	if (!s_env_blend_known)
	{
		char const *const env = std::getenv("M2VK_BLEND");
		s_env_blend = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
		s_env_blend_known = true;
	}
	return (s_env_blend < 0) ? s_option_blend : (s_env_blend != 0);
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

	// HOST_VISIBLE | HOST_COHERENT is required; DEVICE_LOCAL on top of it is preferred (BAR memory —
	// fast to sample, and on unified memory the only type there is). But that preferred type can live
	// in a tiny heap: on a discrete GPU without resizable BAR the device-local+host-visible heap is the
	// ~256 MB PCIe aperture, and a frontend that has already spent most of it (RetroArch's own swapchain
	// and menu textures share the device) leaves too little for our buffers — the allocation then fails
	// with OUT_OF_DEVICE_MEMORY even though the large plain-host-visible heap (system RAM) is wide open.
	// So the preferred type is *tried*, and a failed allocation falls back to plain host-visible rather
	// than being fatal. find_memory_type succeeding is not a promise the heap has room.
	const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t preferred_type = 0, fallback_type = 0;
	const bool have_preferred = find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits,
			need | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, preferred_type);
	const bool have_fallback = find_memory_type(s_fns, s_iface->gpu, reqs.memoryTypeBits, need, fallback_type);
	if (!have_preferred && !have_fallback)
	{
		vk_log(RETRO_LOG_ERROR, "no host-visible coherent memory type accepts a %llu byte buffer (%s)\n",
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
			vk_log(RETRO_LOG_WARN, "device-local host-visible heap could not hold a %llu byte buffer (%s); "
					"falling back to plain host-visible memory\n", (unsigned long long)reqs.size, what);
		alloc.memoryTypeIndex = fallback_type;
		ar = s_fns.allocate_memory(s_device, &alloc, nullptr, &b.memory);
	}
	if (ar != VK_SUCCESS)
	{
		vk_log(RETRO_LOG_ERROR, "vkAllocateMemory failed for a %llu byte buffer (%s): %d\n",
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

// Points the slot's descriptor set at whatever its buffers currently are. Called at build and again
// whenever growth has replaced one of them.
void write_descriptor(geom_slot &slot)
{
	const VkBuffer buffers[4] = { slot.params.buffer, slot.colorxlat.buffer, slot.lumaram.buffer, slot.texram.buffer };

	VkDescriptorBufferInfo info[4]{};
	VkWriteDescriptorSet write[4]{};
	for (uint32_t i = 0; i < 4; i++)
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

	s_fns.update_descriptor_sets(s_device, 4, write, 0, nullptr);
}

// max_level in draw_scanline_tex: 30 - count_leading_zeros_32(min(texwidth, texheight)), which is
// floor(log2(min)) - 1 — the chain stops at 2x2. Resolved here rather than in the shader because it
// is per polygon and constant across its fragments.
int32_t max_mip_level(uint32_t width, uint32_t height)
{
	uint32_t smaller = (width < height) ? width : height;
	int32_t level = -1;
	while (smaller != 0)
	{
		smaller >>= 1;
		level++;
	}

	// Only reachable if a textured polygon arrived with a zero dimension, which the header decode
	// cannot produce (32 << n). MAME would clamp against a negative bound here.
	return (level > 0) ? (level - 1) : 0;
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
	// Four storage buffers: the per-polygon parameters, the baked colour ramps, the luma translator
	// and texture RAM. All four are bound for every draw — the sheets in particular, because the mip
	// chain alternates between them and a polygon reads both.
	VkDescriptorSetLayoutBinding bindings[4]{};
	for (uint32_t i = 0; i < 4; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 4;
	layout_info.pBindings = bindings;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &layout_info, nullptr, &s_set_layout),
			"vkCreateDescriptorSetLayout (geometry)"))
	{
		return false;
	}

	VkDescriptorPoolSize size{};
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = slot_count * 4;

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
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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
	VkShaderModule frag[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	auto const make_module = [](uint32_t const *code, size_t bytes, VkShaderModule &out)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = bytes;
		info.pCode = code;
		return check(s_fns.create_shader_module(s_device, &info, nullptr, &out), "vkCreateShaderModule");
	};

	bool ok = make_module(POLY_VERT_SPV, sizeof(POLY_VERT_SPV), vert)
			&& make_module(POLY_FRAG_SPV, sizeof(POLY_FRAG_SPV), frag[0])
			&& make_module(POLY_EARLY_FRAG_SPV, sizeof(POLY_EARLY_FRAG_SPV), frag[1]);

	if (ok)
	{
		// The three pipelines share every piece of state below except the two that are spelled out per
		// pipeline further down, which is the point: anything that has to be true of one is true of the
		// others by construction. PIPE_BLEND takes the GENERAL fragment module — a deferred polygon is
		// checkered and may also carry the translucent cutout, so it can discard and must test late.
		VkShaderModule const frag_for[PIPE_COUNT] = { frag[0], frag[1], frag[0] };

		VkPipelineShaderStageCreateInfo stages[PIPE_COUNT][2]{};
		for (uint32_t i = 0; i < PIPE_COUNT; i++)
		{
			stages[i][0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[i][0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			stages[i][0].module = vert;
			stages[i][0].pName = "main";
			stages[i][1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[i][1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[i][1].module = frag_for[i];
			stages[i][1].pName = "main";
		}

		VkVertexInputBindingDescription binding{};
		binding.binding = 0;
		binding.stride = sizeof(gpu_vertex);
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		VkVertexInputAttributeDescription attrs[4]{};
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
		attrs[3].location = 3;
		attrs[3].binding = 0;
		attrs[3].format = VK_FORMAT_R32_SFLOAT;         // model2_smooth_shading: welded per-vertex luma
		attrs[3].offset = 28;

		VkPipelineVertexInputStateCreateInfo vertex_input{};
		vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = 4;
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
		//
		// Identical in both pipelines. WHEN the test happens differs — the early-Z module declares
		// EarlyFragmentTests, so its depth is resolved before the shader — but what it computes does not,
		// and that is the only reason the split is safe: a shader with no discard cannot tell the two
		// apart, and one with a discard never reaches the early pipeline.
		VkPipelineDepthStencilStateCreateInfo depth[PIPE_COUNT]{};
		for (uint32_t i = 0; i < PIPE_COUNT; i++)
		{
			depth[i].sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depth[i].depthTestEnable = VK_TRUE;
			depth[i].depthWriteEnable = VK_TRUE;
			depth[i].depthCompareOp = VK_COMPARE_OP_GREATER;
			depth[i].depthBoundsTestEnable = VK_FALSE;
			depth[i].stencilTestEnable = VK_FALSE;
			depth[i].minDepthBounds = 0.0f;
			depth[i].maxDepthBounds = 1.0f;
		}

		// PIPE_BLEND TESTS DEPTH AND DOES NOT WRITE IT, and both halves are load-bearing.
		//
		// It tests, because the depth buffer the first pass leaves behind already answers the only
		// question a deferred polygon has: the key at a pixel belongs to the LOWEST record index that
		// claimed it, and GREATER passes exactly where the deferred polygon's own index is lower still —
		// i.e. where nothing opaque is in front of it. That is the same occlusion the screen door gets
		// from first-writer-wins, obtained from the same buffer, with no sorting of the opaque stream.
		//
		// It does not write, because a transparent surface occludes nothing: leaving the key unclaimed is
		// what lets two overlapping deferred polygons both draw, and it is why the pass can be walked
		// back to front without any of them hiding each other.
		depth[PIPE_BLEND].depthWriteEnable = VK_FALSE;

		VkPipelineColorBlendAttachmentState blend_attachment[PIPE_COUNT]{};
		for (uint32_t i = 0; i < PIPE_COUNT; i++)
		{
			blend_attachment[i].blendEnable = VK_FALSE;
			blend_attachment[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
					| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		}

		// The ordinary over operator on colour, and the destination's alpha left exactly as it was:
		// poly.frag hands this 0.5, which is the coverage the screen door it replaces has, but the
		// attachment's alpha channel is what the 2D composite and the presented frame read as opaque and
		// a half-transparent polygon must not punch a hole in it.
		blend_attachment[PIPE_BLEND].blendEnable = VK_TRUE;
		blend_attachment[PIPE_BLEND].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend_attachment[PIPE_BLEND].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment[PIPE_BLEND].colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment[PIPE_BLEND].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend_attachment[PIPE_BLEND].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment[PIPE_BLEND].alphaBlendOp = VK_BLEND_OP_ADD;

		VkPipelineColorBlendStateCreateInfo blend[PIPE_COUNT]{};
		for (uint32_t i = 0; i < PIPE_COUNT; i++)
		{
			blend[i].sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			blend[i].attachmentCount = 1;
			blend[i].pAttachments = &blend_attachment[i];
		}

		const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{};
		dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamic_states;

		VkGraphicsPipelineCreateInfo info[PIPE_COUNT]{};
		for (uint32_t i = 0; i < PIPE_COUNT; i++)
		{
			info[i].sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			info[i].stageCount = 2;
			info[i].pStages = stages[i];
			info[i].pVertexInputState = &vertex_input;
			info[i].pInputAssemblyState = &assembly;
			info[i].pViewportState = &viewport_state;
			info[i].pRasterizationState = &raster;
			info[i].pMultisampleState = &multisample;
			info[i].pDepthStencilState = &depth[i];
			info[i].pColorBlendState = &blend[i];
			info[i].pDynamicState = &dynamic;
			info[i].layout = s_pipeline_layout;
			info[i].renderPass = render_pass;
			info[i].subpass = 0;
		}

		// One call for all three, so a driver that can share compilation work between them gets the
		// chance. PIPE_BLEND is built whether or not the option is on: it costs one compile at
		// context_reset, and building it lazily would put a pipeline creation in the middle of the first
		// frame after somebody changes the option from the menu.
		ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, PIPE_COUNT, info, nullptr, s_pipelines),
				"vkCreateGraphicsPipelines (geometry)");
	}

	for (VkShaderModule m : frag)
	{
		if (m != VK_NULL_HANDLE)
			s_fns.destroy_shader_module(s_device, m, nullptr);
	}
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

		// The tables and the texture sheets are fixed size and are allocated here rather than on the
		// first frame that carries them, so that nothing in the per-frame path allocates in the steady
		// state. Texture RAM is much the largest thing here — 2 MB a slot, 6 MB over the ring — and it
		// is still small enough that holding all of it beats working out which part is wanted.
		if (!size_slot(slot, INITIAL_POLY_CAPACITY)
				|| !create_buffer(slot.colorxlat, m2vk::COLORXLAT_ENTRIES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
						"vkCreateBuffer (colorxlat)")
				|| !create_buffer(slot.lumaram, m2vk::LUMARAM_ENTRIES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
						"vkCreateBuffer (lumaram)")
				|| !create_buffer(slot.texram, VkDeviceSize(TEXRAM_TOTAL_WORDS) * sizeof(uint32_t),
						VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (texture RAM)"))
		{
			geom_destroy();
			return false;
		}

		// Zeroed once, so that a polygon whose texture parameters point at a sheet the driver variant
		// does not fill reads black rather than whatever the allocator left behind.
		std::memset(slot.texram.mapped, 0, size_t(TEXRAM_TOTAL_WORDS) * sizeof(uint32_t));

		write_descriptor(slot);
	}

	const VkDeviceSize per_slot = s_slots[0].vertices.size + s_slots[0].indices.size + s_slots[0].params.size
			+ s_slots[0].colorxlat.size + s_slots[0].lumaram.size + s_slots[0].texram.size;
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
		destroy_buffer(slot.texram);
	}

	for (VkPipeline p : s_pipelines)
	{
		if (p != VK_NULL_HANDLE)
			s_fns.destroy_pipeline(s_device, p, nullptr);
	}
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

	for (VkPipeline &p : s_pipelines)
		p = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_device = VK_NULL_HANDLE;
	s_iface = nullptr;
}

// model2_smooth_shading weld. Model 2 bakes one flat luma per face; to smooth it, average each vertex's
// luma over the faces meeting there, keyed on the bit-exact screen position + depth. Two faces that share
// a 3D vertex project to the identical (x, y, rz) bits (the geometry engine reuses the transformed
// vertex), so equal keys are the same point; the raw record rz is used, not the renderer-normalised one,
// so textured faces weld with each other (a solid/textured boundary just stays flat, which is fine).
// The map is reused across frames (clear keeps its capacity), and the whole pass runs only when the
// option is on.
namespace {

struct luma_key
{
	uint32_t a, b, c;
	bool operator==(luma_key const &o) const { return a == o.a && b == o.b && c == o.c; }
};

struct luma_hash
{
	size_t operator()(luma_key const &k) const
	{
		uint64_t h = 1469598103934665603ull;
		for (uint32_t w : { k.a, k.b, k.c })
			h = (h ^ w) * 1099511628211ull;
		return size_t(h);
	}
};

struct luma_acc { float sum = 0.0f; uint32_t count = 0; };

luma_key make_luma_key(float x, float y, float rz)
{
	luma_key k;
	std::memcpy(&k.a, &x, 4);
	std::memcpy(&k.b, &y, 4);
	std::memcpy(&k.c, &rz, 4);
	return k;
}

std::unordered_map<luma_key, luma_acc, luma_hash> s_luma_weld;

} // anonymous namespace

bool geom_upload(uint32_t slot_index, frame_record const &record)
{
	// All three pipelines or none: they come out of one vkCreateGraphicsPipelines call.
	if ((slot_index >= s_slot_count) || (s_pipelines[PIPE_GENERAL] == VK_NULL_HANDLE)
			|| (s_pipelines[PIPE_EARLY] == VK_NULL_HANDLE) || (s_pipelines[PIPE_BLEND] == VK_NULL_HANDLE))
		return false;

	// The software rasteriser still owning the 3D is not an error: it is what M2VK_SW_3D=1 asks for,
	// and drawing the same polygons twice would be the bug. Its output is already inside the under
	// layer, exactly as it was in steps 1 and 2.
	if (sw_owns_3d() || no_3d())
		return false;
	// A new frame that is genuinely empty, and the record says so rather than merely failing to say
	// anything. Drawing nothing is what agrees with the software renderer here — see the counter block
	// at the top of this file for why this must not be reported as a drop.
	if (record.geometry_valid && (record.poly_count == 0))
	{
		s_empty_frames++;
		if (s_drew_once)
			s_empty_after_draw++;
		s_seen_serial = record.geometry_serial;
		return false;
	}

	// Nothing usable in the record at all. Before the first drawn frame that is simply a run that has
	// not started rendering; after one, the capture has broken and the 3D layer is flickering.
	if (!record.geometry_valid || !record.tables_valid)
	{
		if (s_drew_once)
			s_dropped_frames++;
		return false;
	}

	// Same serial as last frame means the geometrizer produced nothing and render_polygons took its
	// m_render_done early return. The record still holds last frame's list, so everything below runs
	// again on the same data and the 3D layer simply persists.
	if (record.geometry_serial == s_seen_serial)
		s_dupe_frames++;
	else
		s_seen_serial = record.geometry_serial;

	// This frame commits to drawing from here; latch its polygon count for the HUD.
	s_frame_polys = record.poly_count;

	geom_slot &slot = s_slots[slot_index];
	slot.index_count = 0;
	slot.batches.clear();
	slot.deferred.clear();

	// Latched once for the whole frame. geom_draw pushes this same value, so the shader's stipple test
	// and this function's decision to defer can never disagree about one polygon.
	s_blend_frame = blend_stipple();

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

	// Texture RAM is only reachable if the frame actually carried it; a record from before the first
	// captured geometry frame has null pointers. Without it a textured polygon has nothing to sample,
	// so it is skipped rather than drawn black.
	const bool have_texram = (record.texram[0] != nullptr) && (record.texram[1] != nullptr);

	uint32_t vcount = 0;
	uint32_t icount = 0;
	uint32_t drawn_textured = 0;
	uint32_t skipped_textured = 0;
	uint32_t skipped_translucent = 0;
	uint32_t skipped_offscreen = 0;
	uint32_t frame_clipped = 0;
	uint32_t frame_early = 0;
	uint32_t frame_drawn = 0;
	float    frame_worst = 0.0f;

	// Window ordering, asserted rather than assumed — see the note at the top of the file.
	uint32_t windows_seen = 1;
	bool     windows_descending = true;
	uint8_t  last_window = (record.poly_count != 0) ? record.polys[0].window : 0;

	const bool scissoring = !no_scissor();

	// The visible rectangle, which is what MAME already intersected every clip[] with and what the
	// scissor was before step 6. Only the counter uses it; the batches carry clip[] unaltered.
	const int32_t visible_right = record.layer[LAYER_UNDER].width - 1;
	const int32_t visible_bottom = record.layer[LAYER_UNDER].height - 1;

	// model2_smooth_shading pre-pass: weld each vertex's luma from the OPAQUE faces meeting there, so the
	// vertex loop below can read a smooth per-vertex value. Only opaque, non-stippled polygons take part —
	// translucent-class polygons (renderer bit0) and checker-stipple ones (trees, glass, alpha-mask decals
	// like eyes/lips) keep their flat per-face luma. Two reasons: their luma feeds a different path in
	// poly.frag (a transparent texel borrows its neighbour's luma), where a smoothly-varying value produces
	// the white blow-outs and streaks a flat one does not; and a billboard/decal welding its luma into the
	// solid surface behind it — or borrowing that surface's — is wrong either direction. So they neither
	// contribute here nor read below. Runs only when the option is on, so a default frame pays nothing.
	const bool smoothing = m2vk::smooth();
	auto const poly_smoothable = [](m2vk::poly const &p)
	{
		return ((p.renderer & 1) == 0) && !p.checker;
	};
	if (smoothing)
	{
		s_luma_weld.clear();
		if (s_luma_weld.bucket_count() < record.poly_count * 4)
			s_luma_weld.reserve(record.poly_count * 4);
		for (uint32_t n = 0; n < record.poly_count; n++)
		{
			m2vk::poly const &p = record.polys[n];
			if (!poly_smoothable(p))
				continue;
			const uint32_t nv = (p.num_verts > m2vk::MAX_VERTICES) ? uint32_t(m2vk::MAX_VERTICES) : p.num_verts;
			if (nv < 3)
				continue;
			for (uint32_t i = 0; i < nv; i++)
			{
				luma_acc &a = s_luma_weld[make_luma_key(p.v[i].x, p.v[i].y, p.v[i].rz)];
				a.sum += float(p.luma);
				a.count++;
			}
		}
	}

	for (uint32_t n = 0; n < record.poly_count; n++)
	{
		m2vk::poly const &p = record.polys[n];
		const uint8_t cls = p.renderer & 3;

		if (p.window != last_window)
		{
			windows_seen++;
			if (p.window > last_window)
				windows_descending = false;
			last_window = p.window;
		}

		// cls is bit0 = translucent, bit1 = textured — the same two bits, in the same order, that
		// m_render_callbacks is indexed by. Spelled out rather than passed through, so that the flag
		// words stay independent of that coincidence.
		gpu_poly &gp = params[n];
		gp.palcolor = p.palcolor;
		gp.luma = p.luma;
		gp.flags = ((cls & 1) ? FLAG_TRANSLUCENT : 0u)
				| ((cls & 2) ? FLAG_TEXTURED : 0u)
				| (p.checker ? FLAG_CHECKER : 0u)
				| (p.texwrapx ? FLAG_WRAPX : 0u)
				| (p.texwrapy ? FLAG_WRAPY : 0u)
				| (p.texmirrorx ? FLAG_MIRRORX : 0u)
				| (p.texmirrory ? FLAG_MIRRORY : 0u)
				| (p.utex ? FLAG_UTEX : 0u);
		gp.texlod = p.texlod;
		gp.texwidth = p.texwidth;
		gp.texheight = p.texheight;
		gp.texx = p.texx;
		gp.texy = p.texy;
		gp.lumabase = p.lumabase;
		// The shader reads texsheet[i] as sheet ^ i, so what it wants is which sheet texheader[2]'s
		// bit 12 named — not either of the record's two already-swapped pointers.
		gp.sheet = p.sheet;
		gp.utexx = p.utexx;
		gp.utexy = p.utexy;
		gp.utexminlod = p.utexminlod;
		gp.max_level = ((cls & 2) != 0) ? max_mip_level(p.texwidth, p.texheight) : 0;
		gp.reserved0 = 0;
		gp.reserved1 = 0;

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
			if (!have_texram)
			{
				skipped_textured++;
				continue;
			}
			drawn_textured++;
		}
		// Clamped rather than trusted. The buffers above are sized at MAX_VERTICES per polygon, which
		// is what model2_state::polygon declares, and the one place this could exceed it is a
		// corrupted display list — where the cost of believing it is a write past the end of mapped
		// device memory.
		const uint32_t nverts = (p.num_verts > m2vk::MAX_VERTICES) ? uint32_t(m2vk::MAX_VERTICES) : p.num_verts;
		if (nverts < 3)
			continue;

		// The clipped viewport. MAME already intersected the game's viewport registers with the visible
		// rectangle at the seam, so an empty rectangle here means the game aimed its viewport entirely
		// off screen — and render_triangle against an empty cliprect emits no scanlines at all. Dropping
		// the polygon is that same decision, and it must be a drop rather than an empty-scissor draw or
		// the batch runs fragment for nothing.
		if (scissoring && ((p.clip[2] < p.clip[0]) || (p.clip[3] < p.clip[1])))
		{
			skipped_offscreen++;
			continue;
		}

		// WHICH PIPELINE, and this predicate is the load-bearing line of the split. The early-Z variant
		// resolves depth before the fragment shader, so it is legal only where the shader cannot discard
		// — and the shader's two discard sites are gated on exactly these two flags, read here out of the
		// same word that was just written for it. Any other formulation (p.renderer, p.checker, cls) is a
		// second copy of the predicate that can fall out of step with the shader; this one cannot.
		//
		// Getting it wrong is invisible on most frames: an early-Z polygon that does discard claims the
		// pixel in depth and never writes a colour to it, so the background shows through a hole that
		// nothing later can fill. A stipple over a decal is where that would first be seen.
		const bool early = !no_early_z() && ((gp.flags & (FLAG_TRANSLUCENT | FLAG_CHECKER)) == 0u);
		if (early)
			frame_early++;

		// WHICH PASS. Under model2_transparency a checkered polygon is held back and drawn blended after
		// every opaque one, and the reason is the stream's order rather than anything about the polygon:
		// Model 2 submits FRONT TO BACK, so right here, at the polygon's own place in the list, the
		// geometry behind it does not exist in the colour attachment yet and there is nothing to blend it
		// with. Blending in place would composite it against the 2D under-layer and then be overwritten by
		// whatever it was meant to be seen through.
		//
		// Everything else about it is unchanged — same vertices, same parameter block, same draw-order
		// depth key — because the key is what the second pass tests against, and it has to be the one the
		// polygon always had. Only its INDICES are deferred, which is why the loop below still runs.
		//
		// Note this is never true on the accurate path, so the deferred vector stays empty, no PIPE_BLEND
		// batch is ever appended, and this is exactly the pre-option code.
		const bool defer = s_blend_frame && ((gp.flags & FLAG_CHECKER) != 0u);

		// Extend the open batch, or open a new one. A batch is a run of consecutive polygons sharing both
		// a viewport and a pipeline — one batch for the whole frame is the common case for the viewport,
		// but the pipeline alternates with the polygon stream, so this is where the draw count comes from
		// now. Order is submission order regardless; see the note at the top of the file about why the
		// two classes are not swept into one draw each.
		//
		// With M2VK_NO_SCISSOR the one batch is opened at a rectangle nothing can be outside, which
		// geom_draw's clamp turns back into the framebuffer extent — so the diagnostic path is the same
		// code with one draw, not a second code path.
		const int32_t left   = scissoring ? p.clip[0] : 0;
		const int32_t top    = scissoring ? p.clip[1] : 0;
		const int32_t right  = scissoring ? p.clip[2] : std::numeric_limits<int32_t>::max();
		const int32_t bottom = scissoring ? p.clip[3] : std::numeric_limits<int32_t>::max();

		const uint8_t pipe = early ? PIPE_EARLY : PIPE_GENERAL;

		if (!defer && (slot.batches.empty()
				|| (slot.batches.back().left != left) || (slot.batches.back().top != top)
				|| (slot.batches.back().right != right) || (slot.batches.back().bottom != bottom)
				|| (slot.batches.back().pipe != pipe)))
		{
			slot.batches.push_back(draw_batch{ left, top, right, bottom, icount, 0, pipe });
		}

		frame_drawn++;

		const uint32_t base = vcount;
		const float z = 1.0f - (float((n > DEPTH_MAX_INDEX) ? DEPTH_MAX_INDEX : n) * DEPTH_STEP);

		float bx0 = p.v[0].x, bx1 = p.v[0].x;
		float by0 = p.v[0].y, by1 = p.v[0].y;

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
			// model2_smooth_shading: the welded per-vertex luma, or the flat poly luma when off, on a
			// transparent/alpha material (kept flat, see the pre-pass), or at a vertex that welded with
			// nothing (a clip-edge point). The shader always reads this, so the flat value reproduces the
			// pre-option path exactly.
			float sl = float(p.luma);
			if (smoothing && poly_smoothable(p))
			{
				auto const it = s_luma_weld.find(make_luma_key(p.v[i].x, p.v[i].y, p.v[i].rz));
				if ((it != s_luma_weld.end()) && (it->second.count != 0))
					sl = it->second.sum / float(it->second.count);
			}
			v.smooth_luma = sl;

			bx0 = (v.x < bx0) ? v.x : bx0;
			bx1 = (v.x > bx1) ? v.x : bx1;
			by0 = (v.y < by0) ? v.y : by0;
			by1 = (v.y > by1) ? v.y : by1;
		}

		// Cut by a viewport tighter than the visible rectangle — see the counter's note. Conservative on
		// purpose: a bbox poking out by less than a pixel counts, so an answer of zero really does mean
		// the viewport never bites anywhere the previous build did not already cut.
		float excess = 0.0f;
		if (p.clip[0] > 0)
			excess = std::max(excess, float(p.clip[0]) - bx0);
		if (p.clip[2] < visible_right)
			excess = std::max(excess, bx1 - float(p.clip[2] + 1));
		if (p.clip[1] > 0)
			excess = std::max(excess, float(p.clip[1]) - by0);
		if (p.clip[3] < visible_bottom)
			excess = std::max(excess, by1 - float(p.clip[3] + 1));

		if (excess > 0.0f)
		{
			frame_clipped++;
			frame_worst = std::max(frame_worst, excess);
		}

		// Held back for the second pass; its fan is emitted below, after every other polygon's.
		if (defer)
		{
			slot.deferred.push_back(deferred_poly{ base, nverts, left, top, right, bottom });
			continue;
		}

		// A fan from vertex 0. Model 2 polygons are convex — the geometry engine clips them against
		// four planes — so a fan is exactly what MAME's render_polygon<N,3> covers.
		for (uint32_t i = 1; i + 1 < nverts; i++)
		{
			indices[icount++] = base;
			indices[icount++] = base + i;
			indices[icount++] = base + i + 1;
		}

		slot.batches.back().index_count = icount - slot.batches.back().first_index;
	}

	// THE BLENDED-TRANSPARENCY PASS, walked BACK TO FRONT — which is the reverse of the record, because
	// the record is front to back. Two translucent surfaces over each other only composite correctly if
	// the far one is laid down first, and this is the only place in the renderer where draw order decides
	// a colour: everywhere else the depth key makes the picture order-independent, and it still does
	// between these polygons and the opaque ones. It is only each other they have to be ordered against.
	//
	// The indices continue in the same buffer, so a polygon's fan is emitted exactly once whichever pass
	// owns it and the capacity check above covers both. The batches are appended after every first-pass
	// batch, and geom_draw walks them in order, which is the whole of what makes this a second pass.
	for (std::size_t d = slot.deferred.size(); d-- != 0; )
	{
		deferred_poly const &dp = slot.deferred[d];

		if (slot.batches.empty() || (slot.batches.back().pipe != PIPE_BLEND)
				|| (slot.batches.back().left != dp.left) || (slot.batches.back().top != dp.top)
				|| (slot.batches.back().right != dp.right) || (slot.batches.back().bottom != dp.bottom))
		{
			slot.batches.push_back(draw_batch{ dp.left, dp.top, dp.right, dp.bottom, icount, 0, PIPE_BLEND });
		}

		for (uint32_t i = 1; i + 1 < dp.nverts; i++)
		{
			indices[icount++] = dp.base;
			indices[icount++] = dp.base + i;
			indices[icount++] = dp.base + i + 1;
		}

		slot.batches.back().index_count = icount - slot.batches.back().first_index;
	}

	s_blend_polys += uint32_t(slot.deferred.size());

	if (!s_reported_skips && ((skipped_textured != 0) || (skipped_translucent != 0) || (skipped_offscreen != 0)))
	{
		s_reported_skips = true;
		vk_log(RETRO_LOG_INFO, "geometry: of %u polygons, %u drawn (%u textured), %u textured with no texture RAM, %u untextured translucent (drawn by neither renderer), %u viewport entirely off screen\n",
				unsigned(record.poly_count),
				unsigned(record.poly_count - skipped_textured - skipped_translucent - skipped_offscreen),
				unsigned(drawn_textured),
				unsigned(skipped_textured), unsigned(skipped_translucent), unsigned(skipped_offscreen));
	}

	if (windows_seen > s_max_windows)
		s_max_windows = windows_seen;
	if (slot.batches.size() > s_max_batches)
		s_max_batches = uint32_t(slot.batches.size());
	s_early_polys += frame_early;
	s_drawn_polys += frame_drawn;
	s_offscreen_total += skipped_offscreen;
	s_clipped_polys += frame_clipped;
	s_clipped_worst = std::max(s_clipped_worst, frame_worst);
	if (frame_clipped != 0)
		s_clipped_frames++;
	if (!windows_descending)
		s_windows_ascending_seen = true;

	// The first frame with geometry, reported whatever it looks like: the scissor rectangle is the thing
	// a survey wants and it is interesting even when there is only one of it, because a constant
	// viewport smaller than the screen still clips where the pre-step-6 build did not. The window
	// clause is the assertion — if a run prints `ASCENDING` the record is not in the order
	// render_polygons walks and the draw-order depth key has window priority backwards, which is
	// invisible in any single-window game.
	if (!s_reported_windows && !slot.batches.empty())
	{
		s_reported_windows = true;
		draw_batch const &b = slot.batches.front();
		vk_log(RETRO_LOG_INFO, "geometry: %u polygons, %u window runs (windows %s), %u scissor draws, first %d,%d..%d,%d\n",
				unsigned(record.poly_count), unsigned(windows_seen),
				windows_descending ? "descending, as render_polygons walks them" : "ASCENDING — draw order is wrong",
				unsigned(slot.batches.size()), int(b.left), int(b.top), int(b.right), int(b.bottom));
	}

	// Texture RAM, straight from the machine's memory shares — no snapshot on the emulation thread,
	// because that thread is parked on the baton for the whole of this call and the shares outlive the
	// machine's every frame. 2 MB a frame, and only on frames that actually sample it: the A/B
	// harness's M2VK_FORCE_SOLID runs never touch it.
	//
	// It is re-uploaded unconditionally rather than serial-checked. A dirty check on 2 MB costs a
	// compare against a shadow copy, which is the memcpy again plus the shadow; the write handlers are
	// where that check belongs if it is ever needed, and it is not needed until it is measured.
	if ((drawn_textured != 0) && have_texram)
	{
		auto *const dst = static_cast<uint32_t *>(slot.texram.mapped);
		for (uint32_t sheet = 0; sheet < 2; sheet++)
		{
			const uint32_t words = (record.texram_words[sheet] < m2vk::TEXRAM_SHEET_WORDS)
					? record.texram_words[sheet] : uint32_t(m2vk::TEXRAM_SHEET_WORDS);
			std::memcpy(dst + (sheet * m2vk::TEXRAM_SHEET_WORDS), record.texram[sheet],
					size_t(words) * sizeof(uint32_t));
		}
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

	// The record had polygons and this produced no draw from them. Whether that is a drop depends on
	// why: an untextured translucent polygon and one whose viewport is entirely off screen are drawn by
	// neither renderer, so a frame made only of those is a correct blank and MAME's 3D layer is blank
	// too. A polygon skipped for want of texture RAM is not — that one the software renderer would have
	// drawn, and its absence is the flicker this counter exists to catch.
	if (icount != 0)
		s_drew_once = true;
	else if (s_drew_once && (record.poly_count > (skipped_translucent + skipped_offscreen)))
		s_dropped_frames++;

	return icount != 0;
}

void geom_draw(uint32_t slot_index, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height, unsigned stipple_div)
{
	if ((slot_index >= s_slot_count) || (s_pipelines[PIPE_GENERAL] == VK_NULL_HANDLE)
			|| (s_pipelines[PIPE_EARLY] == VK_NULL_HANDLE) || (s_pipelines[PIPE_BLEND] == VK_NULL_HANDLE))
		return;

	geom_slot &slot = s_slots[slot_index];
	if (slot.index_count == 0)
		return;

	// The VISIBLE half-extent, at every scale. The vertex shader turns m_destmap pixels into NDC and
	// NDC is what does not depend on the resolution; handing it the scaled extent instead puts the
	// whole frame in a 1/scale corner of the attachment, which is what happened the first time.
	gpu_push push{};
	push.half_width = float(width) * 0.5f;
	push.half_height = float(height) * 0.5f;

	// The fragment shader's half of the block. 1 for an ordinary frame, so the stipple's divide is a
	// division by one and this is bit-exact the pre-supersampling behaviour.
	push.stipple_div = stipple_div;

	// The frame's own value, not the option's, and geom_upload latched it — see the note there. Under it
	// the stipple discard is switched off and checkered polygons come out at alpha 0.5, which only the
	// PIPE_BLEND batches ever draw.
	push.blend = s_blend_frame ? 1u : 0u;

	// Attachment pixels per m_destmap pixel, per axis. Floats, and two of them, because the
	// internal-resolution option names absolute 4:3 sizes for a 1.2917 picture — see the header.
	const float scale_x = float(draw_width) / float(width);
	const float scale_y = float(draw_height) / float(height);

	const VkDeviceSize offset = 0;

	// The pipeline is bound inside the loop, per batch. The descriptor set, the push constants and the
	// two buffers are not: both pipelines use the same layout, so binding them once outside covers both.
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
			0, 1, &slot.descriptor, 0, nullptr);
	s_fns.cmd_push_constants(cmd, s_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s_fns.cmd_bind_vertex_buffers(cmd, 0, 1, &slot.vertices.buffer, &offset);
	s_fns.cmd_bind_index_buffer(cmd, slot.indices.buffer, 0, VK_INDEX_TYPE_UINT32);

	// One draw per scissor run per pipeline. The rectangle is inclusive in m_destmap pixels and clamped
	// to the framebuffer here — MAME had already intersected it with the visible rectangle, so the clamp
	// is belt and braces against an emulated viewport register rather than something the frame needs.
	//
	// The internal-resolution scaling is exactly here, and nowhere else in this file: an m_destmap pixel
	// spans scale_x by scale_y attachment pixels, so an inclusive [left, right] becomes
	// [floor(left*sx), ceil((right+1)*sx) - 1]. That rounds OUTWARD on both edges, which can only ever
	// keep a boundary pixel and never drop one — the safe direction, and safe here for a measured
	// reason: P3 step 6 found this scissor exactly redundant with the geometry engine's own frustum
	// clip in 23 of 25 games, the worst excess anywhere being one float ULP. Rounding inward would let
	// a fractional scale shave a column off a polygon that MAME draws whole.
	//
	// The vertex shader's half-extent — the caller's width/height — is deliberately NOT scaled. It is
	// the whole of what a resolution change costs the polygon pass, which is P4 step 2's point: the
	// depth key carries no screen-space term, so nothing else here can depend on the resolution.
	VkPipeline bound = VK_NULL_HANDLE;
	VkRect2D   set_to{};
	bool       scissor_set = false;

	for (draw_batch const &b : slot.batches)
	{
		if (b.index_count == 0)
			continue;

		const int32_t x0 = (b.left < 0) ? 0 : int32_t(std::floor(float(b.left) * scale_x));
		const int32_t y0 = (b.top < 0) ? 0 : int32_t(std::floor(float(b.top) * scale_y));

		// b.right/b.bottom are INT32_MAX for an unscissored batch (M2VK_NO_SCISSOR, and the ordinary
		// full-screen case), so the multiply is done in double and clamped rather than in float: the
		// clamp below is what turns it back into the framebuffer extent, and it has to survive the trip.
		const double right_f = (double(b.right) + 1.0) * double(scale_x);
		const double bottom_f = (double(b.bottom) + 1.0) * double(scale_y);
		const int32_t right = (right_f >= double(draw_width)) ? int32_t(draw_width) - 1
				: int32_t(std::ceil(right_f)) - 1;
		const int32_t bottom = (bottom_f >= double(draw_height)) ? int32_t(draw_height) - 1
				: int32_t(std::ceil(bottom_f)) - 1;
		const int32_t x1 = (right >= int32_t(draw_width)) ? int32_t(draw_width) - 1 : right;
		const int32_t y1 = (bottom >= int32_t(draw_height)) ? int32_t(draw_height) - 1 : bottom;
		if ((x1 < x0) || (y1 < y0))
			continue;

		VkPipeline const want = s_pipelines[(b.pipe < PIPE_COUNT) ? b.pipe : PIPE_GENERAL];
		if (want != bound)
		{
			s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
			bound = want;
		}

		VkRect2D rect{};
		rect.offset = { x0, y0 };
		rect.extent = { uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1) };

		// The pipeline alternates far more often than the viewport does, so most batch boundaries need a
		// new pipeline and the same rectangle. Skipping the redundant vkCmdSetScissor is what keeps the
		// split from costing two commands per batch instead of one.
		if (!scissor_set || (rect.offset.x != set_to.offset.x) || (rect.offset.y != set_to.offset.y)
				|| (rect.extent.width != set_to.extent.width) || (rect.extent.height != set_to.extent.height))
		{
			s_fns.cmd_set_scissor(cmd, 0, 1, &rect);
			set_to = rect;
			scissor_set = true;
		}

		s_fns.cmd_draw_indexed(cmd, b.index_count, 1, b.first_index, 0, 0);
	}

	// Put it back. The caller set the full extent before the under-layer draw and draws the over layer
	// after this returns; leaving a polygon's window in the scissor would clip the foreground tilemaps
	// to it, which is a whole-frame corruption from one line of omission.
	VkRect2D full{};
	full.offset = { 0, 0 };
	full.extent = { draw_width, draw_height };
	s_fns.cmd_set_scissor(cmd, 0, 1, &full);
}

// The current frame's submitted polygon count, for the polygon-counter HUD. See s_frame_polys.
uint32_t geom_frame_polys()
{
	return s_frame_polys;
}

void set_option_blend(unsigned mode)
{
	// Parked, not applied. geom_upload latches it at the top of the next frame, which is what keeps one
	// frame's deferral decision and its push constant in agreement — and is also why this is safe to
	// call from retro_run while a machine is running.
	s_option_blend = (mode != 0);
}

void geom_end_run()
{
	// The run's maxima, which is what a survey reads. A game printing `scissor draws 1` never varies
	// its viewport within a frame and cannot be told apart from the pre-step-6 build by any picture.
	if (s_max_windows != 0)
	{
		vk_log(RETRO_LOG_INFO, "geometry: over the run, at most %u window runs and %u scissor draws in a frame; %u polygons in %u frames cut by a viewport tighter than the screen, worst by %g px%s%s\n",
				unsigned(s_max_windows), unsigned(s_max_batches),
				unsigned(s_clipped_polys), unsigned(s_clipped_frames), double(s_clipped_worst),
				(s_offscreen_total != 0) ? ", some dropped with an off-screen viewport" : "",
				s_windows_ascending_seen ? " — WINDOWS ASCENDING SOMEWHERE, draw order is wrong" : "");

		// The pipeline split. `early` is the share of drawn polygons whose fragment shader cannot discard
		// and which therefore resolve depth before it runs; the batch figure above is what that costs in
		// draws, since a batch now breaks on the pipeline as well as on the viewport. Zero early polygons
		// in a game that is not all-translucent means the predicate has stopped matching the flags — and
		// M2VK_NO_EARLY_Z=1 is the other half of that check, since it must not move a pixel.
		if (s_drawn_polys != 0)
		{
			vk_log(RETRO_LOG_INFO, "geometry: %llu of %llu drawn polygons took the early-Z pipeline (%.1f %%)%s\n",
					(unsigned long long)s_early_polys, (unsigned long long)s_drawn_polys,
					100.0 * double(s_early_polys) / double(s_drawn_polys),
					no_early_z() ? " — M2VK_NO_EARLY_Z is set, so the split is off" : "");
		}

		// The blended-transparency pass, reported only when it did something. Silence on the accurate
		// path is deliberate: that path is what every harness run measures, and a line about a feature
		// that is off is a line somebody has to learn to ignore.
		if (s_blend_polys != 0)
		{
			vk_log(RETRO_LOG_INFO, "geometry: %llu of %llu drawn polygons were checkered and drawn blended in a deferred pass (%.1f %%)\n",
					(unsigned long long)s_blend_polys, (unsigned long long)s_drawn_polys,
					(s_drawn_polys != 0) ? (100.0 * double(s_blend_polys) / double(s_drawn_polys)) : 0.0);
		}
		else if (blend_stipple())
		{
			vk_log(RETRO_LOG_WARN, "geometry: blended transparency was on and NO polygon was checkered — the option reached no geometry\n");
		}

		// The three no-draw paths. `3D dropped` must be 0: anything else means a frame that had geometry
		// lost it again, which is the 3D layer flickering. `empty` is not a fault — it is a game that
		// stops submitting geometry, and a game known to do that reporting 0 of them means the core is
		// no longer telling the record about empty display lists.
		vk_log(RETRO_LOG_INFO, "geometry: %u frames redrew last frame's list (geometrizer behind), %u frames drew nothing on an empty display list (%u of them after the 3D had been drawn), 3D dropped from %u frames after first being drawn\n",
				unsigned(s_dupe_frames), unsigned(s_empty_frames), unsigned(s_empty_after_draw),
				unsigned(s_dropped_frames));
	}

	s_reported_skips = false;
	s_reported_windows = false;
	s_max_windows = 0;
	s_max_batches = 0;
	s_offscreen_total = 0;
	s_clipped_polys = 0;
	s_clipped_frames = 0;
	s_clipped_worst = 0.0f;
	s_seen_serial = 0;
	s_dupe_frames = 0;
	s_empty_frames = 0;
	s_empty_after_draw = 0;
	s_dropped_frames = 0;
	s_drew_once = false;
	s_windows_ascending_seen = false;
	s_early_polys = 0;
	s_drawn_polys = 0;
	s_blend_polys = 0;
	s_no_3d_known = false;
	s_no_scissor_known = false;
	s_no_early_z_known = false;
	s_env_blend_known = false;
}

} // namespace m2vk
