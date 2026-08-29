// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 1 renderer seam — the landing pad for the tapped primitive stream.

    The Model 1 analogue of s21_seam.h, and simpler still. Model 1's polygon path
    (model1_v.cpp) is UNTEXTURED: push_object walks the polygon ROM and resolves every face to one flat
    32-bit colour, already lit and palette-translated at geometry time (quad_t::col). There is no tile
    fetch, no palette bank, no fog/gamma tail, and — unlike S21 — no CLUT to carry across: the pen is
    already RGB. Depth is draw order, the Model 2 model: sort_quads() qsorts every quad by view-space z
    and draw_quads() paints back-to-front, ties broken by submission order. So what crosses the seam is
    tiny: four screen-space corners, a sort z, and one resolved colour.

    Two #ifdef M1VK sites in model1_v.cpp call into this header and nothing else:

      * draw_quads()             -> submit_quad(), inside the per-quad loop, in the exact order the sorted
                                    m_quadind[] is walked, so the record preserves the painter's order the
                                    software rasteriser draws in. Both quad sources (draw_objects' sorted
                                    quads and draw_direct's unsorted "direct" quads) funnel through
                                    draw_quads(), so this one tap covers the whole family. Followed by the
                                    !sw_owns_3d() early-out that lets the GPU pass (M1-2) suppress the
                                    software fill_quad; at M1-1 sw_owns_3d() is always true, so the guard
                                    never trips and the output stays byte-identical.
      * screen_update_model1()   -> frame_begin()/frame_end() around the 2D-under / 3D / 2D-over sequence.
                                    Model 1 renders the whole frame in one screen_update (there is no
                                    device-driven swap as in S21), and draw_quads() is called more than
                                    once per frame (sorted objects, then direct quads), so the bracket
                                    lives in screen_update — NOT in draw_quads — to fire exactly once.

    The `col` field is the resolved value fill_quad writes straight into the bitmap_rgb32: bits 0..23 are
    0x00RRGGBB, bit 24 (MOIRE, 0x01000000) marks the quad translucent — the software path stipples it on a
    (x^y)&1 checker, so the GPU consumer (M1-2) reproduces it as a stipple like Model 2's `checker`.

    COLOUR / LIGHTING NOTE (M1-5, done): `col` is the FINAL lit+translated colour; the seam ALSO carries
    `albedo`, the pre-luma palette pen captured in push_object before the color_xlat luma LUT. "No
    Lighting" (the model2_flat_luma reuse, via set_option_no_lighting/no_lighting() below) is a GPU-side
    pick: m1.frag emits `col` (lit, the default) or `albedo` (raw) per the flat_luma push constant, so the
    toggle is live with no re-capture. NB there is exactly ONE live shading branch in our mame0288 tree —
    the color_xlat LUT; scale_color() is dead (#if 0'd at both sites), and (flags>>10)&3 is the z-key mode,
    not a colour-path selector — so carrying the driver's own lit `col` is bit-exact and covers both the
    push_object and draw_direct paths.

    HI-RES NOTE (M1-3, done): the seam now carries the FLOAT projected pixel, not the rounded s.x/s.y, so
    the GPU rasterises sub-pixel and a raised internal resolution supersamples cleanly (and the bottom-edge
    fill-rule row the integer capture dropped comes back). The two projection paths compute the pixel
    differently — project_point (with zoomx/zoomy + viewx/viewy) vs project_point_direct (no zoom) — and
    draw_quads does not know which produced a given point. But each draw_quads() call is HOMOGENEOUS
    (draw_objects flushes only project_point quads, draw_direct only project_point_direct quads), and the
    hook recomputes both candidate float pixels from the view_t and picks the one whose truncation matches
    the stored integer s — self-contained in the guarded #ifdef M1VK block, no struct edit to point_t.

    Everything crossing the seam is a plain snapshot carrying no MAME types: the consumer compiles without
    the driver's headers. The seam is observation-only at M1-1 — it never changes what the software
    rasteriser draws, which is what keeps the output byte-identical to the M1-0 software baseline.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M1_SEAM_H
#define MAME_OSD_LIBRETRO_M2_M1_SEAM_H

#pragma once

#include <cstdint>

namespace m1 {

// One flat-shaded, pre-projected quad, as it reaches fill_quad in the sorted draw_quads() loop.
// Everything is a copy — nothing here points back into the driver.
struct quad
{
	float    x[4], y[4];   // sub-pixel screen-space (the float projected pixel, before the s.x/s.y round)
	float    z;            // the per-quad view-space sort key (quad_t::z), for reference; depth is order
	uint32_t col;          // resolved: bits 0..23 = 0x00RRGGBB, bit 24 (0x01000000) = MOIRE translucency
	uint32_t albedo;       // pre-luma palette albedo (0x00RRGGBB), the "No Lighting" colour; no MOIRE bit
};

// The MOIRE translucency flag folded into `col` by push_object (model1_v.cpp `enum { MOIRE = 0x01000000 }`).
constexpr uint32_t COL_MOIRE = 0x01000000u;

namespace detail {

// True only while a consumer is attached. Read at the seam for every primitive, so it is a plain bool
// rather than a call; written once, when the tap decides whether to attach (first frame_begin).
extern bool g_active;

// True while MAME's own scanline rasteriser should still draw the 3D. Default true, so a build that never
// turns the GPU path on (M1-1, or the diagnostic tap alone) keeps drawing in software. Read at the
// fill_quad site in draw_quads.
extern bool g_sw_owns_3d;

} // namespace detail

// Cheap enough to sit in front of the per-primitive copy below. A build that attaches nothing leaves this
// false forever and the seam costs one predicate per primitive.
inline bool active() { return detail::g_active; }

// True while the software rasteriser owns the 3D. When false (M1-2's set_gpu), the GPU pass owns it:
// draw_quads records but skips fill_quad, and screen_update skips nothing else (the 2D bands still draw).
inline bool sw_owns_3d() { return detail::g_sw_owns_3d; }

// True only when the GPU genuinely owns the 3D (set_gpu(true)) — unlike sw_owns_3d(), which is ALSO false
// under set_no_3d()'s "neither draws" background reference. Mirrors the S21 gate; M1-4's 2D under/over
// capture will key on it. Wired to the record consumer in M1-2.
bool gpu_owns_3d();

// GPU capture control (wired to m1_geom.cpp's record_* in M1-2). set_gpu(true) attaches the record
// consumer (active() goes true) and hands sw_owns_3d() back false, so the software rasteriser stops
// drawing the 3D and the GPU owns it; set_gpu(false) reverses it. Called by the OSD once the renderer
// decision is made, for a Model 1 driver only. At M1-1 nothing calls these; they are the merge-firewall
// surface, inert until M1-2.
void set_gpu(bool on);

// M2VK_NO_3D: neither the GPU nor the software rasteriser draws the 3D — the background reference (the 2D
// tile layers alone). Hands sw_owns_3d() back false too; capture stays off, so the seam is inert.
void set_no_3d();

// "No Lighting" (the shared model2_flat_luma option). When on, the GPU pass (m1.frag) emits each quad's
// pre-luma albedo instead of the lit `col`. Set by retro_entry when the option/switch is resolved; read on
// the frontend thread in m1_geom's geom_draw, so the toggle is live (a uniform flip, no re-capture). Model
// 1's lighting is baked at geometry time, so unlike Model 2's per-seam luma this is purely a display pick.
void set_option_no_lighting(bool on);
bool no_lighting();

// The 2D-over overlay (m1_seam.cpp). Model 1's tile layers are RGB (segas24_tile_device draws resolved
// 0xffRRGGBB pens straight into the bitmap_rgb32), NOT the pen-index composite S21 needs — so unlike
// S21's option-B machinery this is the plain S22-style RGB sandwich: the driver draws the OVER band
// (layers 7/5/3/1) into a sentinel-filled scratch, this snapshots the pixels it touched as an opaque
// overlay (transparent elsewhere), and vk_present redraws it over the GPU 3D. The 2D-UNDER band (layers
// 6/4/2/0) needs no capture — it is already in the passthrough background the 3D draws over. over_begin
// returns a width*height buffer to fill (0 = transparent), over_end marks it readable, over_pixels
// returns it to the frontend or nullptr if none this frame, over_forget drops it. Gated on gpu_owns_3d().
uint32_t       *over_begin(int width, int height);
void            over_end();
uint32_t const *over_pixels(int &width, int &height);
void            over_forget();

// Snapshots the OVER-band scratch bitmap into the overlay. The scratch is sentinel-filled with high byte
// 0 before the driver draws the OVER tile layers into it; a real tile pen is 0xffRRGGBB, so a set high
// byte marks a pixel the OVER band actually drew. Those become opaque (alpha forced 0xff so a pure-black
// HUD pixel is not read as transparent by the overlay shader, which discards an all-zero texel); the
// untouched sentinel becomes transparent, letting the 3D show through. Templated so this header needs
// none of MAME's types — Bitmap is a bitmap_rgb32, Rect a rectangle.
template <typename Bitmap, typename Rect>
inline void capture_over(Bitmap const &bm, Rect const &clip)
{
	const int w = clip.width();
	const int h = clip.height();
	uint32_t *dst = over_begin(w, h);
	if (dst == nullptr)
		return;

	for (int y = 0; y < h; y++, dst += w)
	{
		auto const *const src = &bm.pix(clip.top() + y, clip.left());
		for (int x = 0; x < w; x++)
		{
			uint32_t const p = uint32_t(src[x]);
			dst[x] = (p & 0xff000000u) ? (0xff000000u | (p & 0x00ffffffu)) : 0u;
		}
	}

	over_end();
}

// Frame brackets. frame_begin() is also where the tap performs its one-time attach decision, so it must
// run before any submit(). Both fire from screen_update_model1: frame_begin() before the 2D-under band,
// frame_end() after the 2D-over band.
void frame_begin();
void frame_end();

// Plumbing (m1_seam.cpp). Drives the diagnostic tap and, when set_gpu(true) has turned capture on, the
// GPU record consumer below.
void submit(quad const &q);

// The GPU record consumer (m1_geom.cpp). Called on the emulation thread from the seam plumbing when
// set_gpu(true) has turned capture on; type-free so the seam and the driver need no renderer headers.
void record_begin();
void record_quad(quad const &q);
void record_end();

// The seam helper for the quad site: the driver hook recomputes the float projected pixel (M1-3) and
// hands it here, so the GPU rasterises at sub-pixel precision and scales cleanly to any internal size.
// A plain forwarder, inert unless a consumer is attached.
inline void submit_quad(float const x[4], float const y[4], float z, uint32_t col, uint32_t albedo)
{
	if (!active())
		return;

	quad q;
	for (int i = 0; i < 4; i++)
	{
		q.x[i] = x[i];
		q.y[i] = y[i];
	}
	q.z = z;
	q.col = col;
	q.albedo = albedo;
	submit(q);
}

} // namespace m1

#endif // MAME_OSD_LIBRETRO_M2_M1_SEAM_H
