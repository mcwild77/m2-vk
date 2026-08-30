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
#include "renderer_vk/vk_geom.h"
#include "renderer_vk/s22_geom.h"
#include "s22_seam.h"
#include "renderer_vk/s21_geom.h"
#include "renderer_vk/m1_geom.h"
#include "renderer_vk/s23_geom.h"
#include "s21_seam.h"
#include "m1_seam.h"
#include "s23_seam.h"

#include "m2vk_frame.h"
#include "m2vk_reticle.h"
#include "m2vk_steerbar.h"
#include "m2vk_sink.h"

#include "renderer_vk/shaders/downsample_frag_spv.h"
#include "renderer_vk/shaders/fullscreen_vert_spv.h"
#include "renderer_vk/shaders/overlay_frag_spv.h"
#include "renderer_vk/shaders/passthrough_frag_spv.h"
#include "renderer_vk/shaders/reticle_frag_spv.h"
#include "renderer_vk/shaders/steerbar_frag_spv.h"
#include "renderer_vk/shaders/counter_frag_spv.h"

#include <algorithm>
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

// M2VK_SS's ceiling. At 4x a slot's oversized colour and depth attachments are 12 MB each, so a ring
// of three costs 73 MB on top of everything else; this is a diagnostic and the bound says so.
constexpr uint32_t MAX_SUPERSAMPLE = 8;

// A backstop on the internal resolution for a device whose reported limits are implausible, and
// nothing more — the real bound is maxImageDimension2D and maxFramebufferWidth/Height, asked of the
// device in clamp_resolution(). 16384 is what RetroArch's MoltenVK reports here.
constexpr uint32_t MAX_OUTPUT_DIMENSION = 16384;


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

	// The polygon pass's depth buffer, which holds draw order rather than z (vk_geom.h). Per slot
	// like everything else here: it is written by this frame's draws and would be a hazard between
	// frames still in flight if it were shared. Never read outside its own render pass, so it is
	// cleared on entry and stored nowhere.
	VkImage         depth = VK_NULL_HANDLE;
	VkDeviceMemory  depth_memory = VK_NULL_HANDLE;
	VkImageView     depth_view = VK_NULL_HANDLE;

	// M2VK_SS only, and null otherwise: the oversized colour and depth the whole frame is drawn into
	// before being resolved back down into `image`. Same render pass, same everything — the only
	// difference is the extent, which is the point of the exercise.
	VkImage         ss_image = VK_NULL_HANDLE;
	VkDeviceMemory  ss_memory = VK_NULL_HANDLE;
	VkImageView     ss_view = VK_NULL_HANDLE;
	VkImage         ss_depth = VK_NULL_HANDLE;
	VkDeviceMemory  ss_depth_memory = VK_NULL_HANDLE;
	VkImageView     ss_depth_view = VK_NULL_HANDLE;
	VkFramebuffer   ss_framebuffer = VK_NULL_HANDLE;
	VkDescriptorSet ss_descriptor = VK_NULL_HANDLE;

	layer_tex       layers[m2vk::LAYER_COUNT];

	// model2_smooth_2d: a second descriptor naming layers[LAYER_UNDER].texture_view through the LINEAR
	// sampler. Written once alongside the NEAREST one; the composite binds whichever the live option
	// selects, so the toggle needs no ring rebuild.
	VkDescriptorSet under_linear_descriptor = VK_NULL_HANDLE;

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
// model2_smooth_2d: a LINEAR twin of s_sampler, used by the opaque 2D under-layer's second descriptor
// only. Never bound anywhere else — the color-keyed OVER layer, the reticle/bar/counter overlays and
// the supersample resolve all keep the NEAREST sampler.
VkSampler             s_sampler_linear = VK_NULL_HANDLE;
VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkDescriptorPool      s_descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout      s_pipeline_layout = VK_NULL_HANDLE;
VkPipeline            s_pipeline = VK_NULL_HANDLE;        // opaque: the under layer
VkPipeline            s_pipeline_over = VK_NULL_HANDLE;   // discards pixel 0: the over layer
VkPipeline            s_pipeline_resolve = VK_NULL_HANDLE; // M2VK_SS only: the supersample resolve
VkPipeline            s_pipeline_reticle = VK_NULL_HANDLE; // the lightgun crosshair, over everything
VkPipeline            s_pipeline_steerbar = VK_NULL_HANDLE; // the steering read-out, over that
VkPipeline            s_pipeline_counter = VK_NULL_HANDLE;  // the polygon counter, top-right, over all

// Captured when the ring is built, so that teardown does not depend on the context's state having
// survived — and so that the funcs table is one pointer chase away in the per-frame path.
const retro_hw_render_interface_vulkan *s_iface = nullptr;
vk_funcs s_fns;
VkDevice s_device = VK_NULL_HANDLE;

// The PICTURE: MAME's visible area, and the units everything upstream of the render pass is in — the
// 2D layer textures and their staging, the polygon stream's coordinates, the reticle's centre.
unsigned s_width = 0;
unsigned s_height = 0;

// The OUTPUT: the extent of the image the frontend is handed, which is the internal-resolution option
// and equals the picture only at native. The ring image, its depth attachment, the framebuffer, the
// render pass and every viewport are in these units.
//
// Everything in between is the two scale factors, and they are NOT equal to each other: the option's
// resolutions are 4:3 and the hardware's 496x384 is 1.2917, so a 640x480 frame is 1.290x in x and
// 1.250x in y. Nothing in the polygon pass cares — poly.vert maps picture pixels to NDC and NDC fills
// whatever viewport it is given — but geom_draw's scissor does, which is why they are floats.
unsigned s_out_width = 0;
unsigned s_out_height = 0;
float    s_scale_x = 1.0f;
float    s_scale_y = 1.0f;

uint32_t s_mask = 0;

//============================================================
//  the supersample diagnostic
//
//  🚨 This is NOT the internal-resolution option, and conflating the two is the mistake this file
//  spent a day being wrong about. They are two features that happen to share one render path:
//
//    M2VK_SS      draw big, then RESOLVE BACK to the picture. The frontend still receives 496x384.
//                 An antialiasing setting, and the instrument P4 step 2's resolution-invariance
//                 claim rests on — res.sh and res-baselines.md are built on the output staying
//                 native, which is what lets ppmdiff.py compare a supersampled run against MAME's
//                 rasteriser at all.
//
//    the option   draw at the chosen size and HAND THAT OVER. No resolve, no downsample.frag. The
//                 picture the player sees really is 1440x1080.
//
//  Mutually exclusive, resolved in read_resolution(), and M2VK_SS wins — so no remembered option
//  value can disturb a harness run.
//============================================================

// M2VK_SS=<n> draws the whole frame — both 2D layers and the polygon pass — into an attachment n
// times the visible extent in each axis, then resolves it back down into the image the frontend is
// handed. Everything downstream still sees 496x384, so ppmdiff.py and the whole A/B harness measure
// it without knowing.
//
// M2VK_SS_POINT=1 takes the centre subpixel instead of the mean, which is only meaningful for an ODD
// n (downsample.frag explains why) and is refused otherwise. It is the stronger test: at 3x point the
// fragment shader runs at the same screen positions as the 1x render, so the two pictures are
// comparable pixel for pixel rather than only in aggregate.
//
// Cost when unset: one getenv per ring build. s_ss == 1 is the ordinary path and every branch below
// is written so that it is the one with nothing extra in it.
uint32_t s_ss = 1;
bool     s_ss_point = false;

// downsample.frag's push block.
struct resolve_push
{
	uint32_t scale;
	uint32_t point_sample;
};


//============================================================
//  the lightgun reticle
//============================================================

// reticle.frag's push block, and the whole of what the shader knows: MAME draws no crosshair that
// this OSD can see (m2vk_reticle.h), so the cross is generated from these numbers.
//
// The geometry is not restated here — it is copied out of m2vk::RETICLE_SHAPE at fill time, so the
// software blitter and this share one definition. What travels per draw is the centre and the scale;
// the centre is in PICTURE pixels and the scale is M2VK_SS, so the shader can put gl_FragCoord back
// into picture pixels and one set of constants serves every internal resolution.
struct reticle_push
{
	float    cx, cy;
	float    scale;
	float    half_thick;
	float    gap;
	float    arm;
	float    outline;
	uint32_t colour;
	uint32_t outline_colour;
};

// std430 in the shader: a vec2 at offset 0 aligned to 8, then seven scalars. No padding anywhere, so
// the struct above is the block verbatim — but the two are edited in different files, and a silent
// disagreement here reads as a reticle drawn at the wrong place or the wrong size rather than as an
// error.
static_assert(sizeof(reticle_push) == 36, "reticle.frag's push block is 9 words");


//============================================================
//  the steering read-out bar
//============================================================

// steerbar.frag's push block. Rectangle is in ATTACHMENT pixels (unlike the reticle) because the
// bar is a screen-space panel that stretches with the attachment.
struct steerbar_push
{
	float    ox, oy;
	float    sx, sy;
	float    value;
	float    raw;
	float    border_u;
	float    border_v;
	float    tick_half;
	float    centre_half;
	uint32_t c_border;
	uint32_t c_empty;
	uint32_t c_fill;
	uint32_t c_centre;
	uint32_t c_tick;
};

static_assert(sizeof(steerbar_push) == 60, "steerbar.frag's push block is 15 words");

// counter.frag's push block. The three vec2 sit at offsets 0/8/16 (vec2 is 8-byte aligned) and the four
// uints follow at 24/28/32/36, which is exactly the natural layout of this struct — no padding.
struct counter_push
{
	float    ox, oy;    // box top-left, attachment pixels
	float    cx, cy;    // one digit cell (w,h)
	float    ix, iy;    // glyph inset within the cell
	uint32_t count;     // number of digits
	uint32_t digits;    // packed BCD, nibble 0 = leftmost digit
	uint32_t fg;        // 0x00RRGGBB glyph
	uint32_t bg;        // 0x00RRGGBB cell background
};

static_assert(sizeof(counter_push) == 40, "counter.frag's push block is 10 words");

// poly_counter — a HUD read-out of the primitive count the active family submitted this frame. Off by
// default (an overlay is pixels no fixture reference would have). M2VK_POLYCOUNT overrides the option in
// the standing presence-or-value direction. The value is refreshed each frame in record_and_submit from
// whichever family owns the 3D; a frame that drew no new 3D keeps the last count rather than flashing 0.
bool     s_option_counter = false;
int      s_env_counter = -2;   // -2 = not read; -1 = no switch; 0/1 = pinned
uint32_t s_counter_value = 0;

bool counter_on()
{
	if (s_env_counter == -2)
	{
		char const *const env = std::getenv("M2VK_POLYCOUNT");
		s_env_counter = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
	}
	return (s_env_counter < 0) ? s_option_counter : (s_env_counter != 0);
}

// model2_smooth_2d — bilinear-filter the opaque 2D under-layer (background tilemaps) when the picture
// is magnified above native by model2_internal_res. Off by default; a no-op at native, where the layer
// maps one texel per pixel. Only ever changes which of the under-layer's two descriptors the composite
// binds, so it applies live. M2VK_SMOOTH_2D overrides it in the same presence-or-value direction as
// the counter switch above.
bool     s_option_smooth_2d = false;
int      s_env_smooth_2d = -2;   // -2 = not read; -1 = no switch; 0/1 = pinned

bool smooth_2d_on()
{
	if (s_env_smooth_2d == -2)
	{
		char const *const env = std::getenv("M2VK_SMOOTH_2D");
		s_env_smooth_2d = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
	}
	return (s_env_smooth_2d < 0) ? s_option_smooth_2d : (s_env_smooth_2d != 0);
}

// model2_internal_res's value, parked here by retro_load_game before the ring exists; 0x0 is "the
// hardware's own". The environment switches override it — see set_option_resolution()'s comment in
// vk_present.h for why.
uint32_t s_option_width = 0;
uint32_t s_option_height = 0;

// What the device will actually accept as an attachment, asked of it once and cached. Zero means "not
// asked yet", which is the state the very first rebuild test runs in — ensure_limits() closes that
// window before the test is used, so the test and the build can never disagree about a clamp.
uint32_t s_limit_width = 0;
uint32_t s_limit_height = 0;

// M2VK_SS parsed once: 0 means "absent, or present and unusable". The environment cannot change
// under a running process, and the rebuild test runs once per presented frame, so these are parsed
// once rather than 57 times a second.
uint32_t s_env_ss = 0;
bool s_env_ss_read = false;
bool s_env_ss_bad = false;

uint32_t env_supersample()
{
	if (!s_env_ss_read)
	{
		s_env_ss_read = true;
		if (char const *const env = std::getenv("M2VK_SS"))
		{
			const unsigned long n = std::strtoul(env, nullptr, 10);
			if ((n < 1) || (n > MAX_SUPERSAMPLE))
				s_env_ss_bad = true;
			else
				s_env_ss = uint32_t(n);
		}
	}
	return s_env_ss;
}

// M2VK_RES=<w>x<h>, the internal resolution's overriding switch, parsed once for the same reason.
// It exists because no entry in the option's list is an integer multiple of 496x384, and the
// equivalence check against the M2VK_SS path needs one — see devnotes/p5-internal-resolution.md.
uint32_t s_env_width = 0;
uint32_t s_env_height = 0;
bool s_env_res_read = false;
bool s_env_res_bad = false;

void env_resolution(uint32_t &width, uint32_t &height)
{
	if (!s_env_res_read)
	{
		s_env_res_read = true;
		if (char const *const env = std::getenv("M2VK_RES"))
		{
			char *end = nullptr;
			const unsigned long w = std::strtoul(env, &end, 10);
			unsigned long h = 0;
			if ((end != nullptr) && (*end == 'x'))
				h = std::strtoul(end + 1, &end, 10);

			if ((w == 0) || (h == 0) || (*end != '\0')
					|| (w > MAX_OUTPUT_DIMENSION) || (h > MAX_OUTPUT_DIMENSION))
			{
				s_env_res_bad = true;
			}
			else
			{
				s_env_width = uint32_t(w);
				s_env_height = uint32_t(h);
			}
		}
	}
	width = s_env_width;
	height = s_env_height;
}

// The frame's shape, from whichever source wins — silent, and with no side effects, because the
// ring-rebuild test calls it every frame to notice a core option changing mid-run. read_resolution()
// below is the half that is allowed to log.
//
// The two features are decided here and nowhere else. M2VK_SS is a supersample-and-resolve, so it
// pins the output at the picture; otherwise the output is the requested size, clamped to what the
// device will take.
struct wanted_frame
{
	uint32_t out_width;
	uint32_t out_height;
	uint32_t ss;
};

wanted_frame wanted_resolution(unsigned picture_width, unsigned picture_height)
{
	wanted_frame want{ uint32_t(picture_width), uint32_t(picture_height), 1 };

	if (const uint32_t ss = env_supersample())
	{
		want.ss = ss;
		return want;
	}

	uint32_t w = 0, h = 0;
	env_resolution(w, h);
	if ((w == 0) || (h == 0))
	{
		w = s_option_width;
		h = s_option_height;
	}
	if ((w == 0) || (h == 0))
		return want;

	// Clamped rather than refused: a player who has picked 2848x2136 on a device that tops out lower
	// is better served by the largest frame it will draw than by silently falling back to native.
	if ((s_limit_width != 0) && (w > s_limit_width))
		w = s_limit_width;
	if ((s_limit_height != 0) && (h > s_limit_height))
		h = s_limit_height;

	want.out_width = w;
	want.out_height = h;
	return want;
}

void read_resolution(unsigned picture_width, unsigned picture_height)
{
	// Resolved in wanted_resolution() so that the rebuild test and the build itself cannot disagree
	// about it — if they ever did, the ring would be rebuilt on every frame for the life of the run.
	const wanted_frame want = wanted_resolution(picture_width, picture_height);

	s_ss = want.ss;
	s_ss_point = false;
	s_out_width = want.out_width;
	s_out_height = want.out_height;

	// The picture's pixels per output pixel, in each axis separately. Not one number: the option's
	// resolutions are 4:3 and the hardware's picture is 1.2917.
	s_scale_x = float(s_out_width) / float(picture_width);
	s_scale_y = float(s_out_height) / float(picture_height);

	if (s_env_ss_bad)
	{
		vk_log(RETRO_LOG_ERROR, "M2VK_SS is out of range (1..%u); using the core option's resolution\n",
				unsigned(MAX_SUPERSAMPLE));
	}
	if (s_env_res_bad)
		vk_log(RETRO_LOG_ERROR, "M2VK_RES is not a usable <width>x<height>; using the core option's resolution\n");

	// The s_ss > 1 test is not decoration: there is no resolve pass at 1x, so a bare M2VK_SS_POINT
	// would otherwise leave the flag set on a run that never reads it, and 1 is odd.
	if ((s_ss > 1) && (std::getenv("M2VK_SS_POINT") != nullptr))
	{
		// Refused rather than quietly boxed: an even scale has no subpixel whose centre coincides with
		// the 1x pixel's, so "point" would silently become "a half-pixel shift", which is exactly the
		// kind of result that gets believed.
		if ((s_ss % 2) == 0)
			vk_log(RETRO_LOG_ERROR, "M2VK_SS_POINT needs an odd scale (it is %u); resolving with the box filter\n", unsigned(s_ss));
		else
			s_ss_point = true;
	}
}

// What this device will accept as a colour attachment, asked once per context and cached. Two limits
// bound it and the smaller wins: an image cannot exceed maxImageDimension2D, and a framebuffer cannot
// exceed maxFramebufferWidth/Height, which are allowed to differ.
//
// 🚨 It must be cached BEFORE the rebuild test first runs, not merely before the first build: the test
// and the build both go through wanted_resolution(), so a clamp that appears between them would make
// them disagree and the ring would be rebuilt on every frame for the rest of the run.
void ensure_limits(const retro_hw_render_interface_vulkan &iface)
{
	if ((s_limit_width != 0) && (s_limit_height != 0))
		return;

	VkPhysicalDeviceProperties props{};
	vk_funcs const fns = context_funcs();
	if (fns.get_physical_device_properties == nullptr)
		return;

	fns.get_physical_device_properties(iface.gpu, &props);

	uint32_t w = props.limits.maxImageDimension2D;
	uint32_t h = w;
	if (props.limits.maxFramebufferWidth < w)
		w = props.limits.maxFramebufferWidth;
	if (props.limits.maxFramebufferHeight < h)
		h = props.limits.maxFramebufferHeight;

	// A device reporting nothing usable would otherwise pin the internal resolution at zero and take
	// the picture with it; the backstop keeps it at something that can at least be drawn.
	s_limit_width = (w == 0) ? MAX_OUTPUT_DIMENSION : w;
	s_limit_height = (h == 0) ? MAX_OUTPUT_DIMENSION : h;
}

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
//  the geometry diagnostic
//============================================================

// M2VK_GEOM_LOG=1 reports the frame record's geometry as the *renderer* sees it — on the frontend's
// thread, after the baton, from data the emulation thread wrote. One line per frame with new
// polygons rather than one per presented frame, so a duped frame is silent and the sequence lines up
// one for one with the polytap's own per-frame line.
//
// That correspondence is the test. The two counts are produced by different code on different
// threads from different copies of the stream, and if they agree frame for frame over a run then the
// record is carrying the whole stream and carrying it intact. Nothing is drawn from it yet.
bool     s_geom_log = false;
bool     s_geom_log_known = false;
uint64_t s_geom_last_serial = 0;
uint64_t s_geom_last_tables = 0;

// FNV-1a, and only over tables that have just changed. A serial says something crossed; a digest
// says the bytes did, which is the part that would otherwise go unnoticed until the shading came out
// wrong with nothing to point at. Texture RAM is not in here: it is not snapshotted into the record,
// so there is nothing whose crossing could be in doubt.
uint64_t table_digest(m2vk::frame_record const &record)
{
	uint64_t h = 1469598103934665603ull;
	for (uint8_t const b : record.colorxlat)
	{
		h ^= b;
		h *= 1099511628211ull;
	}
	for (uint8_t const b : record.lumaram)
	{
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

void report_geometry(m2vk::frame_record const *record)
{
	if (!s_geom_log_known)
	{
		s_geom_log = (std::getenv("M2VK_GEOM_LOG") != nullptr);
		s_geom_log_known = true;
	}

	if (!s_geom_log || (record == nullptr) || !record->geometry_valid)
		return;
	if (record->geometry_serial == s_geom_last_serial)
		return;     // the emulator produced no new list this frame; the renderer would dupe the 3D

	s_geom_last_serial = record->geometry_serial;

	if (record->tables_serial != s_geom_last_tables)
	{
		s_geom_last_tables = record->tables_serial;
		vk_log(RETRO_LOG_INFO, "geom %llu: polys %u/%u  tables %llu now %016llx (%u + %u bytes)\n",
				(unsigned long long)record->geometry_serial,
				unsigned(record->poly_count), unsigned(record->submitted),
				(unsigned long long)record->tables_serial, (unsigned long long)table_digest(*record),
				unsigned(record->colorxlat.size()), unsigned(record->lumaram.size()));
	}
	else
	{
		vk_log(RETRO_LOG_INFO, "geom %llu: polys %u/%u  tables %llu  capacity %u\n",
				(unsigned long long)record->geometry_serial,
				unsigned(record->poly_count), unsigned(record->submitted),
				(unsigned long long)record->tables_serial, unsigned(record->polys.size()));
	}
}


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

// This file's shorthand for the shared helper in vk_funcs, which every call here wants against the
// same device and the same funcs table.
bool find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want, uint32_t &out)
{
	return m2vk::find_memory_type(s_fns, s_iface->gpu, type_bits, want, out);
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
//
// The depth attachment exists for the polygon pass in the middle of the frame and for nothing else.
// It is cleared to 0 on entry and DONT_CARE on the way out: with a GREATER test that clear means
// "no polygon has claimed this pixel", and nothing outside the pass ever reads it.
bool build_render_pass()
{
	VkAttachmentDescription attachments[2]{};

	VkAttachmentDescription &colour = attachments[0];
	colour.format = RING_FORMAT;
	colour.samples = VK_SAMPLE_COUNT_1_BIT;
	colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colour.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkAttachmentDescription &depth = attachments[1];
	depth.format = m2vk::GEOM_DEPTH_FORMAT;
	depth.samples = VK_SAMPLE_COUNT_1_BIT;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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

	// Both directions stated rather than left to the implicit defaults, which are TOP_OF_PIPE on the
	// way in and BOTTOM_OF_PIPE with no access mask on the way out — the latter being no dependency
	// at all as far as the frontend's fragment shader read is concerned. The incoming one covers the
	// depth clear as well, which happens at EARLY_FRAGMENT_TESTS; the outgoing one does not, because
	// the depth attachment is DONT_CARE and nothing outside the pass ever looks at it.
	VkSubpassDependency deps[2]{};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
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

	if (!check(s_fns.create_sampler(s_device, &info, nullptr, &s_sampler), "vkCreateSampler"))
		return false;

	// The LINEAR twin for model2_smooth_2d. Identical but for the two filter fields: same
	// CLAMP_TO_EDGE, so magnifying the picture-sized under-layer up to the internal-resolution target
	// interpolates the interior and clamps the border with no wrap bleed. mipmapMode stays NEAREST —
	// the layer texture has no mip chain (maxLod 0), so it never samples one. UNORM formats are
	// required by Vulkan to support SAMPLED_IMAGE_FILTER_LINEAR, so this cannot fail where NEAREST did.
	info.magFilter = VK_FILTER_LINEAR;
	info.minFilter = VK_FILTER_LINEAR;

	return check(s_fns.create_sampler(s_device, &info, nullptr, &s_sampler_linear), "vkCreateSampler (linear)");
}

// One combined image sampler, fragment stage, one set per slot. The sampler is NOT immutable: the
// under-layer owns a second descriptor naming the LINEAR twin (model2_smooth_2d), and immutable would
// force both descriptors to the layout's one sampler. Each write names its sampler instead — every
// site already did, when the field was ignored — so this is one word of layout, not a per-frame cost.
bool build_descriptors(uint32_t slot_count)
{
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	binding.pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	if (!check(s_fns.create_descriptor_set_layout(s_device, &layout_info, nullptr, &s_set_layout), "vkCreateDescriptorSetLayout"))
		return false;

	// One set per layer per slot: the two layers are sampled by two draws in the same command buffer,
	// so they cannot share. One more per slot for the under-layer's LINEAR twin (model2_smooth_2d),
	// naming the same view through s_sampler_linear. Under M2VK_SS each slot needs one more again,
	// naming its oversized attachment for the resolve draw.
	const uint32_t sets = slot_count * (uint32_t(m2vk::LAYER_COUNT) + 1u + ((s_ss > 1) ? 1u : 0u));

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
	// The push constant belongs to the resolve, the reticle and the steering bar — the two layer
	// pipelines declare no push block and never write one — but a pipeline layout is shared by all of them, and a range no
	// shader reads costs nothing. One range covering the larger of the two blocks rather than a second
	// layout: the ordinary pipelines then stay identical whether either extra is in play, which is
	// what makes "M2VK_SS unset changes nothing" and "no gun changes nothing" cheap to believe.
	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = uint32_t(std::max({ sizeof(resolve_push), sizeof(reticle_push), sizeof(steerbar_push),
			sizeof(counter_push) }));

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &s_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (!check(s_fns.create_pipeline_layout(s_device, &layout_info, nullptr, &s_pipeline_layout), "vkCreatePipelineLayout"))
		return false;

	// The modules exist only for the duration of vkCreateGraphicsPipelines; the pipeline does not
	// reference them afterwards.
	VkShaderModule vert = VK_NULL_HANDLE;
	VkShaderModule frag = VK_NULL_HANDLE;
	VkShaderModule frag_over = VK_NULL_HANDLE;
	VkShaderModule frag_resolve = VK_NULL_HANDLE;
	VkShaderModule frag_reticle = VK_NULL_HANDLE;
	VkShaderModule frag_steerbar = VK_NULL_HANDLE;
	VkShaderModule frag_counter = VK_NULL_HANDLE;
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
			&& make_module(OVERLAY_FRAG_SPV, sizeof(OVERLAY_FRAG_SPV), frag_over)
			&& ((s_ss == 1) || make_module(DOWNSAMPLE_FRAG_SPV, sizeof(DOWNSAMPLE_FRAG_SPV), frag_resolve))
			&& make_module(RETICLE_FRAG_SPV, sizeof(RETICLE_FRAG_SPV), frag_reticle)
			&& make_module(STEERBAR_FRAG_SPV, sizeof(STEERBAR_FRAG_SPV), frag_steerbar)
			&& make_module(COUNTER_FRAG_SPV, sizeof(COUNTER_FRAG_SPV), frag_counter);

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

		// The two 2D layers are not depth-tested and write no depth: they are a background and a
		// foreground, and their order is the order they are recorded in. The state has to be stated
		// all the same, because the subpass now has a depth attachment and pDepthStencilState may
		// only be null when it does not.
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depth.depthTestEnable = VK_FALSE;
		depth.depthWriteEnable = VK_FALSE;
		depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
		depth.depthBoundsTestEnable = VK_FALSE;
		depth.stencilTestEnable = VK_FALSE;

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

		// And the resolve, for the same reason from the same structs. It differs from the opaque layer
		// pipeline in nothing but the fragment shader: same fullscreen triangle, same descriptor, and it
		// covers the attachment so it neither tests nor writes depth.
		if (ok && (s_ss > 1))
		{
			stages[1].module = frag_resolve;
			ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline_resolve),
					"vkCreateGraphicsPipelines (resolve)");
		}

		// And the reticle, again from the same structs: the same fullscreen triangle, scissored down to
		// the cross's bounding box at draw time, discarding everything outside it. Built even on a run
		// with no gun in it — the alternative is a pipeline whose existence depends on a per-frame
		// decision, and it would have to be created on the frame a port first becomes a gun, which is
		// the worst moment to compile a shader. One compile per ring build costs a millisecond once.
		if (ok)
		{
			stages[1].module = frag_reticle;
			ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline_reticle),
					"vkCreateGraphicsPipelines (reticle)");
		}

		// Steering bar, same terms. Built unconditionally so toggling the option mid-run is free.
		if (ok)
		{
			stages[1].module = frag_steerbar;
			ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline_steerbar),
					"vkCreateGraphicsPipelines (steerbar)");
		}

		// Polygon counter, same terms. Built unconditionally like the steering bar so the option is free
		// to toggle mid-run.
		if (ok)
		{
			stages[1].module = frag_counter;
			ok = check(s_fns.create_graphics_pipelines(s_device, VK_NULL_HANDLE, 1, &info, nullptr, &s_pipeline_counter),
					"vkCreateGraphicsPipelines (counter)");
		}
	}

	if (frag_counter != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag_counter, nullptr);
	if (frag_steerbar != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag_steerbar, nullptr);

	if (frag_reticle != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag_reticle, nullptr);
	if (frag_resolve != VK_NULL_HANDLE)
		s_fns.destroy_shader_module(s_device, frag_resolve, nullptr);
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
	if (s_pipeline_counter != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline_counter, nullptr);
	if (s_pipeline_steerbar != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline_steerbar, nullptr);
	if (s_pipeline_reticle != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline_reticle, nullptr);
	if (s_pipeline_resolve != VK_NULL_HANDLE)
		s_fns.destroy_pipeline(s_device, s_pipeline_resolve, nullptr);
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
	if (s_sampler_linear != VK_NULL_HANDLE)
		s_fns.destroy_sampler(s_device, s_sampler_linear, nullptr);
	if (s_render_pass != VK_NULL_HANDLE)
		s_fns.destroy_render_pass(s_device, s_render_pass, nullptr);

	s_pipeline_counter = VK_NULL_HANDLE;
	s_pipeline_steerbar = VK_NULL_HANDLE;
	s_pipeline_reticle = VK_NULL_HANDLE;
	s_pipeline_resolve = VK_NULL_HANDLE;
	s_pipeline_over = VK_NULL_HANDLE;
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_sampler = VK_NULL_HANDLE;
	s_sampler_linear = VK_NULL_HANDLE;
	s_render_pass = VK_NULL_HANDLE;
}

// Drops every handle without calling Vulkan. Either the objects have just been destroyed, or the
// device they belonged to is already gone and touching one would be worse than leaking it.
void forget_ring()
{
	// The polygon pass's own handles belonged to the same device and go the same way.
	m2vk::geom_forget();
	s22::geom_forget();
	s21::geom_forget();
	m1::geom_forget();
	s23::geom_forget();

	// Zeroed rather than left stale: the frontend may still hold a pointer to a slot's handover.
	for (frame_slot &slot : s_slots)
		slot = frame_slot{};
	s_slot_count = 0;

	s_dump_mapped = nullptr;
	s_dump_buffer = VK_NULL_HANDLE;
	s_dump_memory = VK_NULL_HANDLE;
	s_pipeline_counter = VK_NULL_HANDLE;
	s_pipeline_steerbar = VK_NULL_HANDLE;
	s_pipeline_reticle = VK_NULL_HANDLE;
	s_pipeline_resolve = VK_NULL_HANDLE;
	s_pipeline_over = VK_NULL_HANDLE;
	s_pipeline = VK_NULL_HANDLE;
	s_pipeline_layout = VK_NULL_HANDLE;
	s_descriptor_pool = VK_NULL_HANDLE;
	s_set_layout = VK_NULL_HANDLE;
	s_sampler = VK_NULL_HANDLE;
	s_sampler_linear = VK_NULL_HANDLE;
	s_render_pass = VK_NULL_HANDLE;
	s_device = VK_NULL_HANDLE;
	s_iface = nullptr;
	s_width = 0;
	s_height = 0;
	s_out_width = 0;
	s_out_height = 0;
	s_scale_x = 1.0f;
	s_scale_y = 1.0f;
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
		// The supersample pair. Null unless M2VK_SS asked for them; the set is freed with the pool.
		if (slot.ss_framebuffer != VK_NULL_HANDLE)
			s_fns.destroy_framebuffer(s_device, slot.ss_framebuffer, nullptr);
		if (slot.ss_depth_view != VK_NULL_HANDLE)
			s_fns.destroy_image_view(s_device, slot.ss_depth_view, nullptr);
		if (slot.ss_depth != VK_NULL_HANDLE)
			s_fns.destroy_image(s_device, slot.ss_depth, nullptr);
		if (slot.ss_depth_memory != VK_NULL_HANDLE)
			s_fns.free_memory(s_device, slot.ss_depth_memory, nullptr);
		if (slot.ss_view != VK_NULL_HANDLE)
			s_fns.destroy_image_view(s_device, slot.ss_view, nullptr);
		if (slot.ss_image != VK_NULL_HANDLE)
			s_fns.destroy_image(s_device, slot.ss_image, nullptr);
		if (slot.ss_memory != VK_NULL_HANDLE)
			s_fns.free_memory(s_device, slot.ss_memory, nullptr);
		if (slot.depth_view != VK_NULL_HANDLE)
			s_fns.destroy_image_view(s_device, slot.depth_view, nullptr);
		if (slot.depth != VK_NULL_HANDLE)
			s_fns.destroy_image(s_device, slot.depth, nullptr);
		if (slot.depth_memory != VK_NULL_HANDLE)
			s_fns.free_memory(s_device, slot.depth_memory, nullptr);
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
	m2vk::geom_destroy();
	s22::geom_destroy();
	s21::geom_destroy();
	m1::geom_destroy();
	s23::geom_destroy();
	destroy_shared();

	// The frame counts are the point of this line as much as the ring is: they are what says whether
	// a context loss cost the run anything, and they are the only place the two counters are seen
	// together.
	vk_log(RETRO_LOG_INFO, "ring of %u destroyed after %llu frames in this context (%llu since load)\n",
			count, (unsigned long long)s_context_frames, (unsigned long long)s_frames);

	forget_ring();
}

// `width` and `height` are the PICTURE — MAME's visible area — and are what the 2D layer textures and
// their staging are sized from. The ring image, its depth attachment and the framebuffer are sized
// from s_out_width/s_out_height instead, which read_resolution() has already set. The two are equal at
// native and under M2VK_SS; they differ exactly when the internal-resolution option is above native.
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
	image_info.extent = { s_out_width, s_out_height, 1 };
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

	// The polygon pass's depth buffer. Same extent as the picture, so gl_FragCoord and the depth
	// sample agree by construction, and no usage beyond the attachment itself: it is cleared on
	// entry to the pass and discarded at the end of it.
	VkImageCreateInfo depth_info = image_info;
	depth_info.flags = 0;
	depth_info.format = m2vk::GEOM_DEPTH_FORMAT;
	depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (!check(s_fns.create_image(s_device, &depth_info, nullptr, &slot.depth), "vkCreateImage (depth)"))
		return false;
	if (!allocate_and_bind_image(slot.depth, slot.depth_memory))
		return false;

	VkImageViewCreateInfo depth_view_info = view_info;
	depth_view_info.image = slot.depth;
	depth_view_info.format = m2vk::GEOM_DEPTH_FORMAT;
	depth_view_info.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
	if (!check(s_fns.create_image_view(s_device, &depth_view_info, nullptr, &slot.depth_view), "vkCreateImageView (depth)"))
		return false;

	const VkImageView attachments[2] = { slot.view, slot.depth_view };

	VkFramebufferCreateInfo fb_info{};
	fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb_info.renderPass = s_render_pass;
	fb_info.attachmentCount = 2;
	fb_info.pAttachments = attachments;
	fb_info.width = s_out_width;
	fb_info.height = s_out_height;
	fb_info.layers = 1;
	if (!check(s_fns.create_framebuffer(s_device, &fb_info, nullptr, &slot.framebuffer), "vkCreateFramebuffer"))
		return false;

	// The supersample diagnostic's oversized pair. Same formats, same render pass — a render pass says
	// nothing about extent, so the frame is recorded into this one exactly as it would be into the
	// image above, and the resolve draw below is the only thing that knows the difference. The colour
	// image is SAMPLED as well, because the resolve reads it.
	if (s_ss > 1)
	{
		const unsigned ss_width = width * s_ss;
		const unsigned ss_height = height * s_ss;

		VkImageCreateInfo ss_info = image_info;
		ss_info.flags = 0;
		ss_info.extent = { ss_width, ss_height, 1 };
		ss_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (!check(s_fns.create_image(s_device, &ss_info, nullptr, &slot.ss_image), "vkCreateImage (supersample)"))
			return false;
		if (!allocate_and_bind_image(slot.ss_image, slot.ss_memory))
			return false;

		VkImageViewCreateInfo ss_view_info = view_info;
		ss_view_info.image = slot.ss_image;
		if (!check(s_fns.create_image_view(s_device, &ss_view_info, nullptr, &slot.ss_view), "vkCreateImageView (supersample)"))
			return false;

		VkImageCreateInfo ss_depth_info = depth_info;
		ss_depth_info.extent = { ss_width, ss_height, 1 };
		if (!check(s_fns.create_image(s_device, &ss_depth_info, nullptr, &slot.ss_depth), "vkCreateImage (supersample depth)"))
			return false;
		if (!allocate_and_bind_image(slot.ss_depth, slot.ss_depth_memory))
			return false;

		VkImageViewCreateInfo ss_depth_view_info = depth_view_info;
		ss_depth_view_info.image = slot.ss_depth;
		if (!check(s_fns.create_image_view(s_device, &ss_depth_view_info, nullptr, &slot.ss_depth_view), "vkCreateImageView (supersample depth)"))
			return false;

		const VkImageView ss_attachments[2] = { slot.ss_view, slot.ss_depth_view };

		VkFramebufferCreateInfo ss_fb_info = fb_info;
		ss_fb_info.pAttachments = ss_attachments;
		ss_fb_info.width = ss_width;
		ss_fb_info.height = ss_height;
		if (!check(s_fns.create_framebuffer(s_device, &ss_fb_info, nullptr, &slot.ss_framebuffer), "vkCreateFramebuffer (supersample)"))
			return false;

		VkDescriptorSetAllocateInfo ss_set_alloc{};
		ss_set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		ss_set_alloc.descriptorPool = s_descriptor_pool;
		ss_set_alloc.descriptorSetCount = 1;
		ss_set_alloc.pSetLayouts = &s_set_layout;
		if (!check(s_fns.allocate_descriptor_sets(s_device, &ss_set_alloc, &slot.ss_descriptor), "vkAllocateDescriptorSets (supersample)"))
			return false;

		VkDescriptorImageInfo ss_binding{};
		ss_binding.sampler = s_sampler;
		ss_binding.imageView = slot.ss_view;
		ss_binding.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet ss_write{};
		ss_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		ss_write.dstSet = slot.ss_descriptor;
		ss_write.dstBinding = 0;
		ss_write.descriptorCount = 1;
		ss_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		ss_write.pImageInfo = &ss_binding;
		s_fns.update_descriptor_sets(s_device, 1, &ss_write, 0, nullptr);
	}

	// One of these per 2D layer. Identical in every respect but which draw samples them; the loop is
	// the only thing that stops the two from drifting apart.
	for (layer_tex &l : slot.layers)
	{
		// The texture MAME's layer lands in. Optimal tiling and a copy rather than a linear-tiled image
		// sampled in place: MoltenVK's linear-tiling feature set is narrow, and this is the portable
		// shape regardless.
		// The PICTURE's extent, not the ring's: this is MAME's own 2D layer, uploaded at the size the
		// emulator drew it and magnified by the NEAREST sampler at draw time. Inheriting image_info's
		// extent here would make it an oversized texture holding a small picture in one corner.
		VkImageCreateInfo tex_info = image_info;
		tex_info.flags = 0;
		tex_info.extent = { width, height, 1 };
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
		image_binding.sampler = s_sampler;   // NEAREST; the layout carries no immutable sampler now
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

	// model2_smooth_2d's LINEAR twin for the opaque under-layer: the same view the NEAREST descriptor
	// names, sampled through s_sampler_linear. Only the background gets one — the color-keyed OVER
	// layer must stay NEAREST (its exact pixel-0 transparency test breaks under interpolation, and the
	// key colour would bleed into glyph edges), so it has no linear descriptor.
	{
		VkDescriptorSetAllocateInfo set_alloc{};
		set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		set_alloc.descriptorPool = s_descriptor_pool;
		set_alloc.descriptorSetCount = 1;
		set_alloc.pSetLayouts = &s_set_layout;
		if (!check(s_fns.allocate_descriptor_sets(s_device, &set_alloc, &slot.under_linear_descriptor),
				"vkAllocateDescriptorSets (under linear)"))
			return false;

		VkDescriptorImageInfo image_binding{};
		image_binding.sampler = s_sampler_linear;
		image_binding.imageView = slot.layers[m2vk::LAYER_UNDER].texture_view;
		image_binding.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = slot.under_linear_descriptor;
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

	// Before anything is built: it decides the descriptor pool's size and each slot's attachments.
	ensure_limits(iface);
	read_resolution(width, height);

	// Asked of the device rather than assumed. D24_UNORM_S8_UINT does not exist on this GPU at all —
	// its optimalTilingFeatures is literally zero — and a depth format that is merely *usually*
	// present is exactly the sort of thing to find out about at build rather than at the first draw.
	VkFormatProperties depth_props{};
	s_fns.get_physical_device_format_properties(iface.gpu, m2vk::GEOM_DEPTH_FORMAT, &depth_props);
	if ((depth_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
	{
		vk_log(RETRO_LOG_ERROR, "this device has no depth-stencil attachment support for the polygon pass's depth format\n");
		destroy_ring();
		return false;
	}

	// The shared objects first: the slots' framebuffers need the render pass and their descriptor
	// sets need the layout and the pool.
	if (!build_render_pass() || !build_sampler() || !build_descriptors(count) || !build_pipeline())
	{
		destroy_ring();
		return false;
	}

	// The polygon pass shares the render pass and is indexed by the same sync index, so it is built
	// and torn down with the ring rather than with the context.
	if (!m2vk::geom_build(iface, s_fns, s_render_pass, count))
	{
		destroy_ring();
		return false;
	}

	// The System 22 pass, built alongside it and sharing the same render pass and sync index. Stashes
	// the handles only; its pipeline is built lazily on the first captured frame, so the Model 2 build
	// pays nothing here.
	if (!s22::geom_build(iface, s_fns, s_render_pass, count))
	{
		destroy_ring();
		return false;
	}

	// The System 21 pass, likewise — built alongside the ring, pipeline built lazily on first capture.
	if (!s21::geom_build(iface, s_fns, s_render_pass, count))
	{
		destroy_ring();
		return false;
	}

	// The Sega Model 1 pass, likewise — built alongside the ring, pipeline built lazily on first capture.
	if (!m1::geom_build(iface, s_fns, s_render_pass, count))
	{
		destroy_ring();
		return false;
	}

	// The System 23 pass (23-2), likewise — built alongside the ring, pipeline built lazily on first capture.
	if (!s23::geom_build(iface, s_fns, s_render_pass, count))
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
			unsigned(count), s_out_width, s_out_height, unsigned(mask), iface.queue_index,
			(unsigned long long)((VkDeviceSize(width) * VkDeviceSize(height) * 4 * count * m2vk::LAYER_COUNT) / 1024));

	// The internal-resolution option, said out loud for the same reason the supersample line below is:
	// the picture's own size is the only evidence it is in effect, and a log that does not name it
	// leaves "did the option apply?" answerable only by measuring a PPM.
	if ((s_out_width != width) || (s_out_height != height))
	{
		vk_log(RETRO_LOG_INFO,
				"internal resolution: drawing and presenting at %ux%u for a %ux%u picture (%.3fx by %.3fx); %llu KiB of attachments\n",
				s_out_width, s_out_height, width, height, double(s_scale_x), double(s_scale_y),
				(unsigned long long)((VkDeviceSize(s_out_width) * VkDeviceSize(s_out_height) * 8 * count) / 1024));
	}

	// Said out loud, every ring build, because a supersampled run's output is the ordinary 496x384 and
	// there is otherwise nothing in the log or the picture to say which resolution produced it.
	if (s_ss > 1)
	{
		vk_log(RETRO_LOG_INFO, "supersample: drawing at %ux%u (%ux) and resolving with the %s; %llu KiB of oversized attachments\n",
				width * s_ss, height * s_ss, unsigned(s_ss), s_ss_point ? "centre subpixel" : "box filter",
				(unsigned long long)((VkDeviceSize(width) * VkDeviceSize(height) * VkDeviceSize(s_ss) * VkDeviceSize(s_ss)
					* 8 * count) / 1024));
	}

	// The read-back buffer only exists when someone asked for it. A ring rebuild re-reads the
	// environment but does not re-arm a dump that has already been taken.
	if (char const *const prefix = std::getenv("M2VK_VK_DUMP"))
	{
		s_dump_prefix = prefix;
		char const *const at = std::getenv("M2VK_VK_DUMP_FRAME");
		s_dump_frame = (at != nullptr) ? uint64_t(std::strtoull(at, nullptr, 10)) : 600;
		// The ring image's extent, not the picture's: -vk.ppm is a read-back of what the frontend was
		// handed, so above native it is a bigger PPM than -src.ppm. They stopped being comparable with
		// cmp at P3 step 3 anyway.
		if (!s_dump_done && !dump_build_buffer(s_out_width, s_out_height))
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

// The lightgun reticle, last of the pass and over everything in it — it is a pointer, not part of the
// picture. One scissored fullscreen triangle per active gun; nothing is drawn, and no state is
// touched, when no port is set to a gun, which is what keeps every accuracy fixture unmoved
// (m2vk_reticle.h).
//
// `fallback_set` is bound before the draw even though reticle.frag samples nothing. The pipeline
// layout declares the combined image sampler, and the polygon pass ahead of this one binds
// descriptors with a DIFFERENT layout, which leaves set 0 undefined for ours. One bind is cheaper
// than reasoning about whether a validation layer will mind.
void draw_reticles(VkCommandBuffer cmd, uint32_t draw_width, uint32_t draw_height, VkDescriptorSet fallback_set)
{
	if ((s_pipeline_reticle == VK_NULL_HANDLE) || !m2vk::reticle_any())
		return;

	bool bound = false;

	for (unsigned port = 0; port < m2vk::RETICLE_MAX; port++)
	{
		m2vk::reticle_state const &r = m2vk::reticle_get(port);
		if (!r.on)
			continue;

		// The centre in ATTACHMENT pixels, and one scalar for the cross's size. Two factors would be
		// wrong here even though the frame has two: an internal resolution of 640x480 for a 496x384
		// picture is 1.290x by 1.250x, and scaling the shape by both would leave the crosshair's arms
		// visibly longer across than down. The aim is a position and takes both; the cross is a shape
		// and takes one.
		const float scale_y = float(draw_height) / float(s_height);

		reticle_push push{};
		push.cx = r.x * float(draw_width);
		push.cy = r.y * float(draw_height);
		push.scale = scale_y;
		push.half_thick = m2vk::RETICLE_SHAPE.half_thick;
		push.gap = m2vk::RETICLE_SHAPE.gap;
		push.arm = m2vk::RETICLE_SHAPE.arm;
		push.outline = m2vk::RETICLE_SHAPE.outline;
		push.colour = r.colour;
		push.outline_colour = r.outline;

		// The bounding box, in attachment pixels, clamped to the attachment. The shader would discard
		// everything outside it anyway; the scissor is what stops the other 190000 fragments from being
		// shaded to find that out.
		const float half = m2vk::RETICLE_RADIUS * scale_y;
		const float left = push.cx - half;
		const float top = push.cy - half;
		const int32_t x0 = std::max(0, int32_t(left));
		const int32_t y0 = std::max(0, int32_t(top));
		const int32_t x1 = std::min(int32_t(draw_width), int32_t(left + (2.0f * half)) + 1);
		const int32_t y1 = std::min(int32_t(draw_height), int32_t(top + (2.0f * half)) + 1);
		if ((x1 <= x0) || (y1 <= y0))
			continue;   // entirely off the picture

		if (!bound)
		{
			s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_reticle);
			s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
					0, 1, &fallback_set, 0, nullptr);
			bound = true;
		}

		VkRect2D box{};
		box.offset = { x0, y0 };
		box.extent = { uint32_t(x1 - x0), uint32_t(y1 - y0) };
		s_fns.cmd_set_scissor(cmd, 0, 1, &box);

		s_fns.cmd_push_constants(cmd, s_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		s_fns.cmd_draw(cmd, 3, 1, 0, 0);
	}

	// Put the scissor back. Nothing is drawn after this today — the pass ends on the next line — but
	// leaving dynamic state narrowed for whoever adds a draw here is the same trap vk_geom.cpp's
	// per-polygon scissor has to avoid, and it costs one command.
	if (bound)
	{
		VkRect2D full{};
		full.offset = { 0, 0 };
		full.extent = { draw_width, draw_height };
		s_fns.cmd_set_scissor(cmd, 0, 1, &full);
	}
}

// The steering read-out bar, over the reticle. One scissored fullscreen triangle; nothing drawn
// unless model2_steering_display asked. `fallback_set` bound to satisfy the layout's sampler slot.
void draw_steerbar(VkCommandBuffer cmd, uint32_t draw_width, uint32_t draw_height, VkDescriptorSet fallback_set)
{
	if (s_pipeline_steerbar == VK_NULL_HANDLE)
		return;

	m2vk::steerbar_state const &b = m2vk::steerbar_get();
	if (!b.on || !m2vk::steerbar_on())
		return;

	steerbar_push push{};
	push.sx = m2vk::STEERBAR.width * float(draw_width);
	push.sy = m2vk::STEERBAR.height * float(draw_height);
	push.ox = (float(draw_width) - push.sx) * 0.5f;
	push.oy = m2vk::STEERBAR.top * float(draw_height);
	push.value = b.value;
	push.raw = b.raw;
	push.border_u = m2vk::STEERBAR.border_u;
	push.border_v = m2vk::STEERBAR.border_v;
	push.tick_half = m2vk::STEERBAR.tick_half;
	push.centre_half = m2vk::STEERBAR.centre_half;
	push.c_border = m2vk::STEERBAR_COLOUR[m2vk::STEERBAR_BORDER];
	push.c_empty = m2vk::STEERBAR_COLOUR[m2vk::STEERBAR_EMPTY];
	push.c_fill = m2vk::STEERBAR_COLOUR[m2vk::STEERBAR_FILL];
	push.c_centre = m2vk::STEERBAR_COLOUR[m2vk::STEERBAR_CENTRE];
	push.c_tick = m2vk::STEERBAR_COLOUR[m2vk::STEERBAR_TICK];

	// Scissor to the bar's box so the other 180k fragments never get shaded.
	const int32_t x0 = std::max(0, int32_t(push.ox));
	const int32_t y0 = std::max(0, int32_t(push.oy));
	const int32_t x1 = std::min(int32_t(draw_width), int32_t(push.ox + push.sx) + 1);
	const int32_t y1 = std::min(int32_t(draw_height), int32_t(push.oy + push.sy) + 1);
	if ((x1 <= x0) || (y1 <= y0))
		return;

	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_steerbar);
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
			0, 1, &fallback_set, 0, nullptr);

	VkRect2D box{};
	box.offset = { x0, y0 };
	box.extent = { uint32_t(x1 - x0), uint32_t(y1 - y0) };
	s_fns.cmd_set_scissor(cmd, 0, 1, &box);

	s_fns.cmd_push_constants(cmd, s_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s_fns.cmd_draw(cmd, 3, 1, 0, 0);

	// Restore full scissor for whatever draws next.
	VkRect2D full{};
	full.offset = { 0, 0 };
	full.extent = { draw_width, draw_height };
	s_fns.cmd_set_scissor(cmd, 0, 1, &full);
}

// The polygon counter: draws s_counter_value (the active family's primitive count this frame) as decimal
// digits in a small box in the top-right, over everything. Same scissored-fullscreen-triangle trick as
// the steering bar. Sized as a fraction of the attachment so it stays the same on-screen size at every
// internal resolution. Binds `fallback_set` only to satisfy the shared layout — counter.frag reads no
// sampler, exactly as steerbar.frag does not.
void draw_counter(VkCommandBuffer cmd, uint32_t draw_width, uint32_t draw_height, VkDescriptorSet fallback_set)
{
	if (s_pipeline_counter == VK_NULL_HANDLE || !counter_on())
		return;

	// Format the count into decimal digits, most significant first, into an 8-digit field.
	uint32_t v = (s_counter_value > 99999999u) ? 99999999u : s_counter_value;
	uint8_t dec[8];
	int n = 0;
	if (v == 0)
		dec[n++] = 0;
	else
	{
		uint8_t tmp[8];
		int m = 0;
		while (v != 0) { tmp[m++] = uint8_t(v % 10u); v /= 10u; }
		for (int i = m - 1; i >= 0; i--)
			dec[n++] = tmp[i];
	}
	uint32_t packed = 0;                       // nibble 0 = leftmost digit
	for (int i = 0; i < n; i++)
		packed |= uint32_t(dec[i]) << (i * 4);

	counter_push push{};
	const float cell_h = float(draw_height) * 0.030f;
	const float cell_w = cell_h * 0.62f;
	const float margin = float(draw_height) * 0.018f;
	const float box_w = cell_w * float(n);
	push.cx = cell_w;
	push.cy = cell_h;
	push.ix = cell_w * 0.14f;
	push.iy = cell_h * 0.12f;
	push.count = uint32_t(n);
	push.digits = packed;
	push.fg = 0x35ff35u;                       // bright green digits
	push.bg = 0x000000u;                       // on a black cell (opaque; the pipeline does not blend)
	push.ox = float(draw_width) - box_w - margin;
	push.oy = margin;
	if (push.ox < 0.0f) push.ox = 0.0f;

	const int32_t x0 = std::max(0, int32_t(push.ox));
	const int32_t y0 = std::max(0, int32_t(push.oy));
	const int32_t x1 = std::min(int32_t(draw_width), int32_t(push.ox + box_w) + 1);
	const int32_t y1 = std::min(int32_t(draw_height), int32_t(push.oy + cell_h) + 1);
	if ((x1 <= x0) || (y1 <= y0))
		return;

	s_fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_counter);
	s_fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
			0, 1, &fallback_set, 0, nullptr);

	VkRect2D box{};
	box.offset = { x0, y0 };
	box.extent = { uint32_t(x1 - x0), uint32_t(y1 - y0) };
	s_fns.cmd_set_scissor(cmd, 0, 1, &box);

	s_fns.cmd_push_constants(cmd, s_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s_fns.cmd_draw(cmd, 3, 1, 0, 0);

	VkRect2D full{};
	full.offset = { 0, 0 };
	full.extent = { draw_width, draw_height };
	s_fns.cmd_set_scissor(cmd, 0, 1, &full);
}

// `draw_over` says whether the frame has a foreground layer to composite. Without one this is P2's
// passthrough exactly: a single opaque fullscreen draw of whatever landed in layer 0. `draw_3d` says
// whether the polygon pass has anything to put between the two.
bool record_and_submit(frame_slot &slot, uint32_t index, bool draw_over, bool draw_3d, bool draw_3d_s22, bool draw_3d_s21, bool draw_3d_m1, bool draw_3d_s23, bool dump)
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
	// Depth clears to 0, which with the polygon pass's GREATER test means "no polygon has claimed
	// this pixel yet". It is the m_fillmap.fill(0x00) the software renderer does at the top of
	// render_polygons.
	VkClearValue clear[2]{};
	clear[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	clear[1].depthStencil = { 0.0f, 0 };

	// Under M2VK_SS the sandwich is drawn into the oversized attachment and resolved into the ring
	// image by a second pass below; otherwise these are the ring image and its extent — which at an
	// internal resolution above native is already bigger than the picture, and is handed to the
	// frontend exactly as drawn.
	const bool ss = (s_ss > 1) && (slot.ss_framebuffer != VK_NULL_HANDLE) && (s_pipeline_resolve != VK_NULL_HANDLE);
	const uint32_t draw_width = ss ? (s_width * s_ss) : s_out_width;
	const uint32_t draw_height = ss ? (s_height * s_ss) : s_out_height;

	// The System 21 pen-space composite (option B) runs in its own private render pass, BEFORE the shared
	// present pass begins: it lays the 2D-under pens, the 3D quads and the layer-0 mix into a pen
	// attachment, which finish_draw (inside the present pass, below) then samples, applies the OVER band
	// to, and resolves to RGB. A no-op in the model2 / namcos22 builds and in S21 software / NO_3D mode.
	if (draw_3d_s21)
		s21::pen_pass(slot.cmd, index, draw_width, draw_height);

	VkRenderPassBeginInfo pass{};
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = s_render_pass;
	pass.framebuffer = ss ? slot.ss_framebuffer : slot.framebuffer;
	pass.renderArea.offset = { 0, 0 };
	pass.renderArea.extent = { draw_width, draw_height };
	pass.clearValueCount = 2;
	pass.pClearValues = clear;
	s_fns.cmd_begin_render_pass(slot.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = float(draw_width);
	viewport.height = float(draw_height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	s_fns.cmd_set_viewport(slot.cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = { draw_width, draw_height };
	s_fns.cmd_set_scissor(slot.cmd, 0, 1, &scissor);

	// The sandwich, bottom slice first. The two 2D draws are the same fullscreen triangle and differ
	// only in the fragment shader and in which layer's descriptor is bound; the polygon pass goes
	// between them. This is why the frame is three draws in one pass rather than one draw of a shader
	// that samples both layers — a depth-tested geometry pass has to sit in the middle, and it cannot
	// if the background and the foreground are resolved in a single fragment.
	// The System 21 finish pass is the whole S21 background: it resolves the pen composite pen_pass built
	// (2D-under + 3D + mix) with the OVER band applied in pen space. It stands in for the UNDER draw and
	// the 3D/mix/OVER draws below, all of which happened in the pen pass. Everything else takes the normal
	// UNDER background draw.
	if (draw_3d_s21)
	{
		s21::finish_draw(slot.cmd, index, draw_width, draw_height);
	}
	else
	{
		// model2_smooth_2d picks the LINEAR twin here and only here: this is the opaque background draw.
		// The OVER pass, the reticle/bar/counter overlays and the SS resolve keep the NEAREST descriptor.
		VkDescriptorSet const under_set = smooth_2d_on()
				? slot.under_linear_descriptor
				: slot.layers[m2vk::LAYER_UNDER].descriptor;
		s_fns.cmd_bind_pipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
		s_fns.cmd_bind_descriptor_sets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
				0, 1, &under_set, 0, nullptr);
		s_fns.cmd_draw(slot.cmd, 3, 1, 0, 0);
	}

	// The 3D. It is handed the VISIBLE extent and the attachment's separately: the polygon stream is in
	// m_destmap pixels and the vertex shader turns those into NDC, so the scaling belongs to the
	// viewport and the scissor and to nothing else.
	//
	// The last argument is the stipple divisor, and it is the one place the two features genuinely
	// differ rather than sharing a path. Under M2VK_SS the frame is about to be averaged back down to
	// the picture, so the checker has to stay one PICTURE pixel per square or the resolve turns the
	// screen door into a flat 50 % — measured, and the reason P4 step 2 handed the stipple to P5. At a
	// real internal resolution nothing is averaged, so the divisor is 1 and the checker is one OUTPUT
	// pixel per square: a fine dither that reads as smooth translucency, which is what it is for.
	if (draw_3d)
		m2vk::geom_draw(index, slot.cmd, s_width, s_height, draw_width, draw_height, ss ? s_ss : 1);

	// The System 22 untextured pass (S2). Over the background, in place of the Model 2 3D — the two are
	// never both live in one build. It draws in painter's order with the depth test off, so it neither
	// reads nor writes the depth attachment the Model 2 pass shares.
	if (draw_3d_s22)
		s22::geom_draw(index, slot.cmd, s_width, s_height, draw_width, draw_height);

	// The Sega Model 1 pass (M1-2). Over the background, painter's order with the depth test off — like
	// S22, it never touches the depth attachment the Model 2 pass shares. The three families are never
	// both live in one loaded game (family routing picks one). Its 2D-over HUD is the RGB overlay
	// redrawn by the shared OVER pass below (draw_over), the S22-style sandwich (M1-4).
	if (draw_3d_m1)
		m1::geom_draw(index, slot.cmd, s_width, s_height, draw_width, draw_height);

	// The System 23 pass (23-2). Over the background, painter's order with the depth test off — like S22
	// and M1, it never touches the depth attachment the Model 2 pass shares. The families are never both
	// live in one loaded game (family routing picks one). Untextured greyscale-lit geometry gate; textures
	// and the 2D-over sandwich come at 23-3 / 23-5.
	if (draw_3d_s23)
		s23::geom_draw(index, slot.cmd, s_width, s_height, draw_width, draw_height);

	// (System 21 is composited entirely in the pen pass + finish_draw above; nothing here.)

	if (draw_over)
	{
		s_fns.cmd_bind_pipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_over);
		s_fns.cmd_bind_descriptor_sets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
				0, 1, &slot.layers[m2vk::LAYER_OVER].descriptor, 0, nullptr);
		s_fns.cmd_draw(slot.cmd, 3, 1, 0, 0);
	}

	// The System 22 prioverchar over-pass: primitives flagged "priority over the text layer" (poly
	// cmode&7==1, sprite cz==0xfe) are redrawn over the OVER text overlay just drawn, so they sit above
	// the text — MAME's mixer priority 7. A no-op on plain S22 and on SS22 frames that have none.
	if (draw_3d_s22)
		s22::geom_draw_over(index, slot.cmd, s_width, s_height, draw_width, draw_height);

	// The reticle goes over the foreground too, and inside the supersampled pass rather than after the
	// resolve, so that at n>1 it is drawn at n times the size and comes back down antialiased along
	// with everything else.
	draw_reticles(slot.cmd, draw_width, draw_height, slot.layers[m2vk::LAYER_UNDER].descriptor);

	// Steering bar over the reticle, inside the SS pass so it antialiases with everything else.
	draw_steerbar(slot.cmd, draw_width, draw_height, slot.layers[m2vk::LAYER_UNDER].descriptor);

	// Polygon counter, topmost. Refresh the value from whichever family owns the 3D this frame; a frame
	// that drew no new 3D (draw flags all false, e.g. a dupe) keeps the previous count rather than showing
	// a spurious 0.
	if (draw_3d_s22)
		s_counter_value = s22::geom_primitive_count();
	else if (draw_3d_s21)
		s_counter_value = s21::geom_primitive_count();
	else if (draw_3d_m1)
		s_counter_value = m1::geom_primitive_count();
	else if (draw_3d_s23)
		s_counter_value = s23::geom_primitive_count();
	else if (draw_3d)
		s_counter_value = m2vk::geom_frame_polys();
	draw_counter(slot.cmd, draw_width, draw_height, slot.layers[m2vk::LAYER_UNDER].descriptor);

	s_fns.cmd_end_render_pass(slot.cmd);

	// The resolve. A second pass over the ring image, sampling what the first one drew — the render
	// pass's finalLayout has already put the oversized image into SHADER_READ_ONLY_OPTIMAL and its
	// outgoing subpass dependency already covers a fragment-shader read, so there is no barrier here
	// and none is missing. The triangle covers the attachment, so the colour clear and the depth
	// attachment along with it are along for the ride.
	if (ss)
	{
		// s_out_* rather than s_width/s_height, though under M2VK_SS they are equal by construction:
		// the resolve's target is the ring image, and the ring image's extent has exactly one name.
		pass.framebuffer = slot.framebuffer;
		pass.renderArea.extent = { s_out_width, s_out_height };
		s_fns.cmd_begin_render_pass(slot.cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

		viewport.width = float(s_out_width);
		viewport.height = float(s_out_height);
		s_fns.cmd_set_viewport(slot.cmd, 0, 1, &viewport);

		scissor.extent = { s_out_width, s_out_height };
		s_fns.cmd_set_scissor(slot.cmd, 0, 1, &scissor);

		resolve_push push{};
		push.scale = s_ss;
		push.point_sample = s_ss_point ? 1u : 0u;

		s_fns.cmd_bind_pipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_resolve);
		s_fns.cmd_bind_descriptor_sets(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout,
				0, 1, &slot.ss_descriptor, 0, nullptr);
		s_fns.cmd_push_constants(slot.cmd, s_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
				0, sizeof(push), &push);
		s_fns.cmd_draw(slot.cmd, 3, 1, 0, 0);

		s_fns.cmd_end_render_pass(slot.cmd);
	}

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
		back.imageExtent = { s_out_width, s_out_height, 1 };
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

	// The record the emulation thread finished writing before it parked on the baton. The polygon
	// stream and the colour tables in it are recorded but not yet drawn — step 2 of P3 hands them
	// across and no further, and report_geometry is how that is checked.
	m2vk::frame_record const *const record = m2vk::frame_current();
	report_geometry(record);

	// The composited path when the emulation thread has captured both 2D layers, the passthrough
	// otherwise. "Otherwise" is not only the software renderer: the layers do not exist until the
	// first screen_update has run, and a capture whose geometry disagrees with the frame the OSD
	// handed us is not one to composite from — the picture would be the right size and the wrong
	// content, which is worse than a frame of passthrough.
	m2vk::frame_record const *layers = record;
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

	// The resolution is the one rebuild trigger the frontend does not cause: model2_internal_res can
	// change in the options menu while content is running, and every slot's attachments are sized from
	// it. Treated exactly like a mask change — destroy_ring() already brackets a vkDeviceWaitIdle with
	// the frontend's queue lock, which is what makes a rebuild safe at an arbitrary frame.
	//
	// ensure_limits() before the test, not merely before the build: both go through
	// wanted_resolution(), so a clamp appearing between them would make them disagree for ever and the
	// ring would be rebuilt on every frame of the run.
	ensure_limits(*iface);
	const wanted_frame want = wanted_resolution(width, height);

	if ((s_slot_count == 0) || (iface != s_iface) || (width != s_width) || (height != s_height)
			|| (mask != s_mask) || (want.ss != s_ss)
			|| (want.out_width != s_out_width) || (want.out_height != s_out_height))
	{
		if (s_slot_count != 0)
		{
			vk_log(RETRO_LOG_INFO, "rebuilding the ring: %ux%u mask 0x%x %ux -> %ux%u mask 0x%x %ux\n",
					s_out_width, s_out_height, unsigned(s_mask), unsigned(s_ss),
					want.out_width, want.out_height, unsigned(mask), unsigned(want.ss));
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

	// The System 22 2D-over overlay: the HUD/text that must sit above the GPU 3D. Present only when the
	// S22 core owns the 3D and captured it this frame (nullptr in the Model 2 build and in S22 software
	// mode). When present it turns the passthrough into an UNDER/OVER sandwich around the S22 3D — the
	// same three-draws-in-one-pass Model 2 uses — with `pixels` (the finished 2D frame the seam has
	// stripped of software 3D) as the UNDER background and this overlay drawn again after the 3D.
	int s22_over_w = 0, s22_over_h = 0;
	uint32_t const *const s22_over = s22::over_pixels(s22_over_w, s22_over_h);
	const bool s22_sandwich = (layers == nullptr) && (s22_over != nullptr)
			&& (unsigned(s22_over_w) == width) && (unsigned(s22_over_h) == height);

	// System 21 composites its whole frame — 2D-under, 3D and the OVER band — in pen-index space on the
	// GPU (option B, s21_geom.cpp), so it hands back no RGB overlay to sandwich; the UNDER staging below
	// carries `pixels` only for the inert reticle/steering background, and finish_draw draws the picture.

	// The Sega Model 1 2D-over overlay (M1-4): the HUD (layers 7/5/3/1) that must sit above the GPU 3D.
	// Model 1's tile layers are RGB, so — like S22, and unlike S21's pen-space composite — it is the plain
	// UNDER/OVER sandwich: `pixels` (the finished 2D frame, seam-stripped of software 3D, already carrying
	// the 2D-UNDER band) as the background, this overlay drawn again after the 3D. nullptr in every build
	// that did not capture Model 1 and in M1 software / NO_3D mode.
	int m1_over_w = 0, m1_over_h = 0;
	uint32_t const *const m1_over = m1::over_pixels(m1_over_w, m1_over_h);
	const bool m1_sandwich = (layers == nullptr) && (m1_over != nullptr)
			&& (unsigned(m1_over_w) == width) && (unsigned(m1_over_h) == height);

	// The System 23 2D-over overlay (23-5): the text/HUD tilemap that must sit above the GPU 3D. Like S22
	// and Model 1 (and unlike S21's pen-space composite), System 23's text is RGB, so it is the plain
	// UNDER/OVER sandwich: `pixels` (the finished 2D frame, seam-stripped of software 3D, already carrying
	// the text band) as the background, this overlay drawn again after the 3D. nullptr in every build that
	// did not capture System 23 and in S23 software / NO_3D mode / with the overlay option off.
	int s23_over_w = 0, s23_over_h = 0;
	uint32_t const *const s23_over = s23::over_pixels(s23_over_w, s23_over_h);
	const bool s23_sandwich = (layers == nullptr) && (s23_over != nullptr)
			&& (unsigned(s23_over_w) == width) && (unsigned(s23_over_h) == height);

	if (layers != nullptr)
	{
		std::memcpy(slot.layers[m2vk::LAYER_UNDER].staging_mapped, layers->layer[m2vk::LAYER_UNDER].pixels.data(), bytes);
		std::memcpy(slot.layers[m2vk::LAYER_OVER].staging_mapped, layers->layer[m2vk::LAYER_OVER].pixels.data(), bytes);
	}
	else
	{
		std::memcpy(slot.layers[m2vk::LAYER_UNDER].staging_mapped, pixels, bytes);
		if (s22_sandwich)
			std::memcpy(slot.layers[m2vk::LAYER_OVER].staging_mapped, s22_over, bytes);
		else if (m1_sandwich)
			std::memcpy(slot.layers[m2vk::LAYER_OVER].staging_mapped, m1_over, bytes);
		else if (s23_sandwich)
			std::memcpy(slot.layers[m2vk::LAYER_OVER].staging_mapped, s23_over, bytes);
	}

	// The polygon stream, turned into this slot's vertex, index and parameter buffers. Only when the
	// 2D layers are ours to composite: without them there is no gap in the picture for the 3D to go
	// into, because the software renderer's own 3D is already in it.
	//
	// A frame whose geometry did not change re-uploads and redraws the same polygons, which is
	// exactly the "keep last frame's 3D" case — render_polygons takes its m_render_done early return
	// without touching the record, so what is still in there is last frame's list. Redrawing it costs
	// a frame's upload and is one behaviour rather than two.
	const bool draw_3d = (layers != nullptr) && m2vk::geom_upload(index, *record);

	// The System 22 untextured pass. Independent of the Model 2 layers path: the S22 core draws its 3D
	// over MAME's finished 2D frame (which the seam has stripped of software 3D), so it rides the
	// passthrough — layers is null and the background is `pixels`. geom_upload returns false in the
	// Model 2 build (nothing ever captures), so this costs one predicate there.
	const bool draw_3d_s22 = s22::geom_upload(index);

	// The System 21 pen-space composite. geom_upload returns true when the driver captured the 2D-under
	// this frame — i.e. S21 owns the picture — in which case pen_pass + finish_draw draw the whole frame.
	// Returns false in the Model 2 / namcos22 builds (nothing ever captures S21), so this costs one
	// predicate there.
	const bool draw_3d_s21 = s21::geom_upload(index);

	// The Sega Model 1 untextured pass (M1-2). Like S22, it draws its 3D over MAME's finished 2D frame
	// (the seam stripped of software 3D), riding the passthrough — layers is null, background is `pixels`.
	// geom_upload returns false in every build that did not capture Model 1, so this costs one predicate.
	const bool draw_3d_m1 = m1::geom_upload(index);

	// The System 23 untextured pass (23-2). Like S22/M1 it draws its 3D over MAME's finished 2D frame (the
	// seam stripped of software 3D), riding the passthrough — layers is null, background is `pixels`.
	// geom_upload returns false in every build that did not capture System 23, so this costs one predicate.
	const bool draw_3d_s23 = s23::geom_upload(index);

	// A foreground OVER pass runs when Model 2 captured both layers, or when the S22 or Model 1 core handed
	// back an overlay to sandwich its 3D between. (S21's OVER band is applied in the finish pass, not here.)
	const bool draw_over = (layers != nullptr) || s22_sandwich || m1_sandwich || s23_sandwich;

	const bool dump = !s_dump_done && !s_dump_prefix.empty() && (s_dump_mapped != nullptr)
			&& (s_frames == s_dump_frame);

	if (!record_and_submit(slot, index, draw_over, draw_3d, draw_3d_s22, draw_3d_s21, draw_3d_m1, draw_3d_s23, dump))
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
			vk_log(RETRO_LOG_INFO, "first frame presented: %ux%u through slot %u\n", s_out_width, s_out_height, unsigned(index));
		else
			vk_log(RETRO_LOG_INFO, "picture resumed at frame %llu: %ux%u through slot %u\n",
					(unsigned long long)s_frames, s_out_width, s_out_height, unsigned(index));
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
			write_ppm(s_dump_prefix + "-vk.ppm", s_dump_mapped, s_out_width, s_out_height);
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
	m2vk::geom_end_run();
	s22::geom_end_run();
	s21::geom_end_run();
	m1::geom_end_run();
	s23::geom_end_run();
	s_frames = 0;

	// The record's own serials restart with the run, so the watermarks that chase them have to as
	// well, or the first frame of a second game would look like a duplicate and go unreported.
	s_geom_last_serial = 0;
	s_geom_last_tables = 0;
}

void set_option_resolution(unsigned width, unsigned height)
{
	// 0x0 is the caller saying "the hardware's own", which is also where an unparseable option value
	// lands; anything else absurd is refused to the same place rather than clamped, because a caller
	// asking for 40000 has misunderstood something and quietly giving it 16384 hides that. The device's
	// real limits are applied later, in wanted_resolution(), where they are known.
	if ((width != 0) || (height != 0))
	{
		if ((width == 0) || (height == 0) || (width > MAX_OUTPUT_DIMENSION) || (height > MAX_OUTPUT_DIMENSION))
		{
			vk_log(RETRO_LOG_ERROR, "internal resolution %ux%u is not usable; drawing at the hardware's own\n",
					width, height);
			width = 0;
			height = 0;
		}
	}

	s_option_width = width;
	s_option_height = height;
}

void set_option_counter(bool on)
{
	// Parked; draw_counter reads counter_on() each frame, so it applies on the next presented frame with
	// nothing to rebuild. M2VK_POLYCOUNT overrides it there.
	s_option_counter = on;
}

void set_option_smooth_2d(bool on)
{
	// Parked; the composite reads smooth_2d_on() each frame to pick the under-layer descriptor, so it
	// applies on the next presented frame with nothing to rebuild. M2VK_SMOOTH_2D overrides it there.
	s_option_smooth_2d = on;
}

bool present_extent(unsigned &width, unsigned &height)
{
	if ((s_out_width == 0) || (s_out_height == 0))
		return false;

	width = s_out_width;
	height = s_out_height;
	return true;
}

} // namespace m2vk
