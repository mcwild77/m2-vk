// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 renderer sink — the landing pad for the tapped polygon stream.

    The seam is model2_renderer::model2_3d_render() in src/mame/sega/model2_v.cpp: the point where
    the geometry engine has finished projecting to screen space and every texture/lighting parameter
    has been resolved, immediately before the software scanline rasterizer is dispatched. Four
    #ifdef M2VK blocks there call frame_begin() / submit() / frame_end() and nothing else; this
    header is the whole of what they see.

    What arrives here is a m2vk::poly — a plain snapshot of the polygon, carrying no MAME types.
    Its shape, and the rest of what crosses the seam, is m2vk_frame.h; this header is the seam
    itself. The separation is deliberate:

      * consumers (the diagnostic tap below, the hardware renderer later) compile without the
        driver's headers, so they cannot come to depend on driver internals, and an upstream change
        to model2.h breaks the conversion in one place instead of everywhere;
      * the snapshot is close to what a vertex buffer upload wants anyway.

    The conversion itself is the templated submit() at the bottom. It is a template purely so that
    this header can also be compiled in translation units that have no idea what a
    model2_state::polygon is — the parameter types are deduced at the one call site.

    Lifetime: the sink is a single object with static storage duration (m2vk_sink.cpp), so it always
    exists, whether or not the game ever renders a polygon. sink_open() / sink_close() bracket one
    machine's run; the OSD calls them from init() and osd_exit(). A build whose OSD does not call
    them (the plain SUBTARGET=model2 binary) still gets one run's worth of output, flushed when the
    process exits.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_SINK_H
#define MAME_OSD_LIBRETRO_M2_M2VK_SINK_H

#pragma once

#include "m2vk_frame.h"

#include <cstdint>
#include <cstring>

namespace m2vk {

// Implemented by anything that wants the stream. run_begin/run_end bracket one machine; the frame
// pair brackets one rendered frame. Every method is optional.
class consumer
{
public:
	virtual ~consumer() = default;

	virtual void run_begin() { }
	virtual void run_end() { }
	virtual void frame_begin(uint32_t submitted, frame_tables const &tables) { }
	virtual void submit(poly const &p) { }
	virtual void frame_end() { }
};

namespace detail {

// True while at least one consumer is attached. Read at the seam for every polygon, so it is a
// plain bool rather than a call into the sink; written only when consumers are attached or dropped.
extern bool g_active;

// True while MAME's own scanline rasteriser should still draw the 3D layer. False once the hardware
// renderer owns it, which is also where nearly all of the emulator's CPU time goes.
extern bool g_rasterize;

// The debug filter, all four of which are set once from the environment in sink_open(). See
// force_solid() and only_poly() below for what they are for; they live here rather than behind
// accessors because submit() reads them for every polygon on both renderers' paths.
extern uint8_t  g_force_solid;      // 0 off, 1 clear the textured bit, 2 force renderer 0
extern bool     g_opaque_only;      // every translucent polygon rewritten to the class neither draws
extern int32_t  g_only_poly;        // -1 draws every polygon, otherwise only this one
extern int32_t  g_only_frame;       // -1 means every frame; only meaningful with g_only_poly
extern uint32_t g_frame_index;      // frames seen at the seam, from 0
extern uint32_t g_poly_index;       // polygons seen in this frame, from 0
extern bool     g_poly_dropped;     // set by submit() for the polygon it was just given

} // namespace detail

// Cheap enough to sit in front of the per-polygon conversion below. Two predicates because there
// are two independent reasons to want the stream: a diagnostic consumer is attached, or the frame
// record is capturing for the hardware renderer.
inline bool active() { return detail::g_active || capturing(); }

// Read at the seam once per polygon, immediately after submit() — and it answers for *that*
// polygon, not for the frame: the single-polygon debug mode below drops polygons by making this
// false for them. Default true, so a build that never sets any of it behaves exactly as MAME does.
inline bool rasterize() { return detail::g_rasterize && !detail::g_poly_dropped; }

// Whether MAME's scanline rasteriser owns the 3D layer at all, which is the question the hardware
// renderer asks — it must not draw the same polygons a second time. Distinct from rasterize(),
// which is per-polygon and is the seam's question.
//
// It is also what render_polygons() tests before recycling poly_manager's object-data arena by
// hand, and that is not an optimisation. model2_3d_render() takes a slot from it for every polygon
// before the seam is reached, and poly_manager recycles the arena in wait() — which early-outs when
// there are no work units outstanding. With the scanline dispatch skipped there never are any, so
// nothing is ever recycled and the arena grows for the length of the run: measured at ~200 MiB over
// 2500 frames of VF2 before the reset was added.
inline bool sw_owns_3d() { return detail::g_rasterize; }
void set_rasterize(bool on);

//============================================================
//  the debug modes
//============================================================
//
// Both are read once from the environment and both act on *both* renderers, which is the entire
// point of them: a difference between the two paths is only attributable if the two paths were
// given the same polygons to draw.
//
//   M2VK_FORCE_SOLID=1   clears the textured bit, so every polygon takes the untextured path and
//                        translucency still means "draw nothing" (draw_scanline_solid<true>
//                        returns immediately). This is the faithful one.
//   M2VK_FORCE_SOLID=2   forces renderer 0, so every polygon draws opaque. Nothing is skipped, so
//                        the coverage comparison sees the whole stream — which is what it is for.
//
//   M2VK_OPAQUE_ONLY=1   rewrites every translucent polygon to renderer class 1, which is the one
//                        class NEITHER renderer draws — draw_scanline_solid<true> returns before it
//                        writes a pixel and vk_geom drops it at upload. So it removes exactly the
//                        same polygons from both paths, which is what makes it a measuring tool
//                        rather than a picture. It exists because P3 lands the translucent cutout a
//                        step after the opaque textured path, and without it the coverage diff for
//                        that step reads the missing translucency as a rasterizer disagreement.
//                        Applied after M2VK_FORCE_SOLID, which subsumes it in both its modes.
//
//   M2VK_ONLY_POLY=<n>   draws polygon n of a frame and no other. With M2VK_ONLY_FRAME=<m> it is
//                        polygon n of frame m; without, it is polygon n of every frame. n counts
//                        polygons arriving at the seam, so it is the same n in both renderers even
//                        when the record has dropped some.
//
// None of them costs anything when unset: three predicates on an already-hot path.
inline bool force_solid() { return detail::g_force_solid != 0; }
inline bool opaque_only() { return detail::g_opaque_only; }
inline bool only_poly() { return detail::g_only_poly >= 0; }

// One machine's run. Called by the OSD from init() and osd_exit().
void sink_open();
void sink_close();

// The seam, driver-type-free half. The tables are pointers into model2_state and are read — and
// copied — before frame_begin returns; nothing keeps them.
void frame_begin(uint32_t submitted, frame_tables const &tables);
void submit(poly const &p);
void frame_end();

// The seam, driver-facing half: converts one polygon and hands it over. Templated so that this
// header carries no dependency on the driver's headers; Polygon is model2_state::polygon, Extra is
// m2_poly_extra_data and Rect is the clipped viewport rectangle.
//
// It *returns the renderer class the software rasteriser should use*, and the caller assigns it
// back. That is deliberate and it is what keeps the upstream diff at 26 lines: M2VK_FORCE_SOLID has
// to rewrite the class for both renderers or it proves nothing, and the seam already had this value
// in its hands. In every other case the value comes back unchanged.
template <typename Polygon, typename Extra, typename Rect>
inline uint8_t submit(Polygon const &src, Extra const &extra, uint8_t renderer, Rect const &vp)
{
	// Counted for every polygon the seam sees, whether or not anything is watching, so that
	// M2VK_ONLY_POLY names the same polygon under renderer=software as under renderer=vulkan.
	const uint32_t index = detail::g_poly_index++;

	detail::g_poly_dropped = only_poly()
			&& ((uint32_t(detail::g_only_poly) != index)
				|| ((detail::g_only_frame >= 0) && (uint32_t(detail::g_only_frame) != detail::g_frame_index)));

	// Before the active() test: the software rasteriser obeys this even when nothing is recording.
	if (detail::g_force_solid == 1)
		renderer &= 1;      // textured -> untextured, translucent still means "draw nothing"
	else if (detail::g_force_solid != 0)
		renderer = 0;       // everything opaque and untextured, so nothing is skipped

	// Class 1 is the one neither renderer draws, so this subtracts the same set from both.
	if (detail::g_opaque_only && ((renderer & 1) != 0))
		renderer = 1;

	if (!active() || detail::g_poly_dropped)
		return renderer;

	poly p;

	p.num_verts = uint8_t(src.num_vertices);
	for (int i = 0; i < src.num_vertices; i++)
	{
		p.v[i].x  = src.v[i].x;
		p.v[i].y  = src.v[i].y;
		p.v[i].rz = src.v[i].p[0];
		p.v[i].uz = src.v[i].p[1];
		p.v[i].vz = src.v[i].p[2];
	}
	for (int i = src.num_vertices; i < MAX_VERTICES; i++)
		p.v[i] = vertex{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

	p.renderer = renderer;
	p.bucket = src.z;
	p.window = src.window;
	for (int i = 0; i < 4; i++)
		p.texheader[i] = src.texheader[i];
	p.luma = src.luma;
	p.texlod = src.texlod;
	for (int i = 0; i < 4; i++)
		p.viewport[i] = src.viewport[i];
	p.center[0] = src.center[0];
	p.center[1] = src.center[1];
	p.clip[0] = vp.left();
	p.clip[1] = vp.top();
	p.clip[2] = vp.right();
	p.clip[3] = vp.bottom();

	p.lumabase = extra.lumabase;
	p.colorbase = extra.colorbase;

	// The one table lookup that happens here rather than crossing whole. Both scanline renderers do
	// exactly this — state->m_palram[colorbase + 0x1000] — and colorbase is 10 bits, so the index
	// cannot leave m_palram's 0x2000 entries. See m2vk::poly for why the raw word is kept.
	p.palcolor = extra.state->m_palram[extra.colorbase + 0x1000];

	p.checker = extra.checker;

	if (renderer & 2)
	{
		p.texwidth = extra.texwidth;
		p.texheight = extra.texheight;
		p.texx = extra.texx;
		p.texy = extra.texy;
		p.texwrapx = extra.texwrapx;
		p.texwrapy = extra.texwrapy;
		p.texmirrorx = extra.texmirrorx;
		p.texmirrory = extra.texmirrory;
		p.sheet = (src.texheader[2] & 0x1000) ? 1 : 0;
		p.utex = extra.utex;
		p.utexminlod = extra.utexminlod;
		p.utexx = extra.utexx;
		p.utexy = extra.utexy;
		p.texsheet[0] = extra.texsheet[0];
		p.texsheet[1] = extra.texsheet[1];
	}
	else
	{
		p.texwidth = p.texheight = 0;
		p.texx = p.texy = 0;
		p.texwrapx = p.texwrapy = 0;
		p.texmirrorx = p.texmirrory = 0;
		p.sheet = 0;
		p.utex = p.utexminlod = 0;
		p.utexx = p.utexy = 0;
		p.texsheet[0] = p.texsheet[1] = nullptr;
	}

	submit(p);
	return renderer;
}

// The 2D half of the seam: one of the two tilemap layers that sandwich the 3D, cropped to the
// visible rectangle and copied into the frame record. Templated for the same reason submit() is —
// Bitmap is a bitmap_rgb32 and Rect a rectangle, and this header must not know that.
//
// `clip` is screen_update()'s cliprect, which for Model 2 is the visible area (x 0..495, y 0..383 of
// a 656x424 raster — model2.cpp's set_raw). Cropping here rather than later means the layers come out
// the same size as the picture the OSD hands the frontend, so the composite is 1:1 with no offset
// arithmetic anywhere downstream.
template <typename Bitmap, typename Rect>
inline void capture_layer(int which, Bitmap const &bm, Rect const &clip)
{
	if (!capturing())
		return;

	const int w = clip.width();
	const int h = clip.height();
	if ((w <= 0) || (h <= 0) || (clip.right() >= bm.width()) || (clip.bottom() >= bm.height()))
		return;

	uint32_t *dst = layer_begin(which, w, h);
	if (dst == nullptr)
		return;

	for (int y = 0; y < h; y++, dst += w)
		std::memcpy(dst, &bm.pix(clip.top() + y, clip.left()), size_t(w) * sizeof(uint32_t));

	layer_end(which);
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_SINK_H
