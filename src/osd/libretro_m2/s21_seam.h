// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 21 renderer seam — the landing pad for the tapped primitive stream.

    The System 21 analogue of s22_seam.h, and a much smaller one: the S21 "polygonizer"
    (namcos21_3d_device, src/mame/namco/namcos21_3d.cpp) is UNTEXTURED and PRE-PROJECTED. Every
    primitive is one flat-shaded quad that already arrives in screen space with an integer depth code;
    there is no texture/palette/fog tail, no perspective divide, and no per-vertex colour. So what
    crosses the seam is tiny: four screen-space corners, one per-quad depth, and one resolved pen.

    Two #ifdef S21VK sites in namcos21_3d.cpp call into this header and nothing else:

      * blit_single_quad()               -> submit_quad(), at the end of the method, AFTER backface
                                            culling (so only visible quads are recorded), the palette /
                                            depth-cue colour resolve, and the screen-space vertex setup,
                                            immediately before the two rendertri() calls. Both quad
                                            sources (draw_quads / draw_direct_quad) funnel through here,
                                            so this one tap covers the whole family.
      * swap_and_clear_poly_framebuffer() -> frame_end()/frame_begin(), the double-buffer swap the DSP
                                            drives once per 3D list (namcos21_kickstart). The completed
                                            work page becomes visible at the swap, so frame_end() closes
                                            the just-drawn list and frame_begin() opens the new one. The
                                            two priming swaps at device_start run before any consumer
                                            attaches and are inert (active() is false).

    Depth note, for whoever writes the GPU path (T2): renderscanline_flat tests and writes the PER-QUAD
    zsort (the clamped mean zcode), NOT the edge-interpolated z rendertri computes — that interpolation
    is dead. So the hardware model is a real z-buffer at per-quad depth granularity; one flat depth per
    quad, carried here as `zsort`. The `color` field is the FINAL resolved pen (palette base + the
    per-quad depth cue already folded in), so the consumer reproduces the exact framebuffer pen without
    re-deriving the cue.

    Everything crossing the seam is a plain snapshot carrying no MAME types: consumers compile without
    the driver's headers and cannot come to depend on its internals. The seam is observation-only in T1
    — it never touches what the software rasteriser draws, which is what keeps the output byte-identical
    to the T0 software baseline.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_S21_SEAM_H
#define MAME_OSD_LIBRETRO_M2_S21_SEAM_H

#pragma once

#include <cstdint>

namespace s21 {

// One flat-shaded, pre-projected quad, as it reaches the two rendertri() calls. Everything is a copy —
// nothing here points back into the driver.
struct quad
{
	int16_t  x[4], y[4];   // screen-space pixels (frame-centre-offset, as written to the framebuffer)
	int32_t  zsort;        // the per-quad depth actually tested/written by renderscanline_flat
	uint16_t color;        // the final resolved pen: palette base + per-quad depth cue folded in
};

// The palette the CLUT reads, as a raw pointer into the driver's palette device. The device only knows
// pen indices; the RGB lives in the driver (m_palette), so — unlike S22, which resolves basecolor to
// RGB at the seam — S21 hands the palette over and the GPU does the CLUT lookup. The pointer is stable;
// its contents change as the game writes palette RAM, so the consumer re-reads it each frame. Set from
// the driver's frame bracket; inert unless capturing.
struct palette_ram
{
	uint32_t const *pens = nullptr;   // m_palette->pens(), 0x00RRGGBB
	uint32_t        count = 0;        // number of entries
};

namespace detail {

// True only while a consumer is attached. Read at the seam for every primitive, so it is a plain
// bool rather than a call; written once, when the tap decides whether to attach (first frame_begin).
extern bool g_active;

// True while MAME's own scanline rasteriser should still draw the 3D. Default true, so a build that
// never turns the GPU path on (T1, or the diagnostic tap alone) keeps drawing in software. Read at the
// rendertri site in blit_single_quad and at the copy_visible / mix sites in the driver screen update.
extern bool g_sw_owns_3d;

} // namespace detail

// Cheap enough to sit in front of the per-primitive conversion below. A build that attaches nothing
// leaves this false forever and the seam costs one predicate per primitive.
inline bool active() { return detail::g_active; }

// True while the software rasteriser owns the 3D. When false, the GPU pass owns it: blit_single_quad
// records but skips rendertri, and the driver skips copy_visible_poly_framebuffer / the C355 z-mix.
inline bool sw_owns_3d() { return detail::g_sw_owns_3d; }

// True only when the GPU genuinely owns the 3D (set_gpu(true)) — unlike sw_owns_3d(), which is ALSO
// false under set_no_3d()'s "neither draws" background reference. The driver's screen_update takes the
// same !sw_owns_3d() branch either way (there is no third "capture nothing" branch to give it), so this
// is what over_begin()/mix_begin() gate on: a capture_over_sprites/capture_mix_sprites call that fires
// under M2VK_NO_3D must not populate an overlay the renderer would then draw, or the background
// reference every coverage comparison differences against stops being neither-renderer-drew-anything.
bool gpu_owns_3d();

// T2 GPU capture. set_gpu(true) attaches the record consumer (active() goes true) and hands sw_owns_3d()
// back false, so the software rasteriser stops drawing the 3D and the GPU owns it. set_gpu(false) is the
// reverse. Called by the OSD once the renderer decision is made, for the namcos21 subtarget only — in
// the model2 / namcos22 builds these are compiled but never called (no S21 seam site fires).
void set_gpu(bool on);

// M2VK_NO_3D: neither the GPU nor the software rasteriser draws the 3D — the background reference. Hands
// sw_owns_3d() back false too, so the driver skips the 3D entirely and the picture is just the 2D
// layers. Capture stays off, so the seam is inert.
void set_no_3d();

// The palette pointer the CLUT reads. Called by the driver from the frame bracket; cheap (stores a
// pointer and a count). Inert unless capturing; the consumer re-reads the contents each frame.
void set_palette(uint32_t const *pens, uint32_t count);
palette_ram const &get_palette();

// The 2D-over overlay (s21_seam.cpp). When the GPU owns the 3D, the driver draws the C355 sprite bands
// that sit OVER the polygons (high-priority, and — for pri1 in {0,2} — the flat layer-0) into a
// transparent overlay instead of the UNDER bitmap, so the renderer draws them again after the GPU 3D.
// Same UNDER/OVER sandwich Model 2 and System 22 use. over_begin returns a width*height buffer to fill
// (0 = transparent), over_end marks it readable, over_pixels returns it to the frontend or nullptr if
// none this frame, over_forget drops it. A no-op while the software rasteriser owns the 3D.
uint32_t       *over_begin(int width, int height);
void            over_end();
uint32_t const *over_pixels(int &width, int &height);
void            over_forget();

// Snapshots a palettized scratch bitmap (the C355 over-bands the driver has just drawn) into the
// overlay: every non-zero pen becomes an opaque 0xffRRGGBB texel, pen 0 stays transparent. Templated so
// this header needs none of MAME's types — Bitmap is a bitmap_ind16, Rect a rectangle. Keeps the pixel
// loop out of the upstream driver, which is left with the sprite draws it alone can do (the golden rule).
// The high byte is forced to 0xff so a text pixel that resolves to pure black is not read as transparent
// by the overlay shader (which discards an all-zero texel).
template <typename Bitmap, typename Rect>
inline void capture_over(Bitmap const &bm, uint32_t const *pens, Rect const &clip)
{
	const int w = clip.width();
	const int h = clip.height();
	uint32_t *dst = over_begin(w, h);
	if ((dst == nullptr) || (pens == nullptr))
		return;

	for (int y = 0; y < h; y++, dst += w)
	{
		auto const *const src = &bm.pix(clip.top() + y, clip.left());
		for (int x = 0; x < w; x++)
		{
			auto const pen = src[x];
			dst[x] = pen ? (0xff000000u | (uint32_t(pens[pen]) & 0x00ffffffu)) : 0u;
		}
	}

	over_end();
}

// The T2b layer-0 z-mix overlay (s21_seam.cpp). pri1==4 gameplay draws its layer-0 C355 sprites gated
// against the polygon z-buffer (namcos21_c67_state::mix_layer0_sprites); the CPU no longer has that
// buffer once the GPU owns the 3D (T2a), so the driver captures the sprite pixels and a per-pixel
// SHOW/gated/never tag instead, and the GPU pass (s21_geom.cpp geom_draw_mix) does the depth comparison
// against its own real depth attachment. Same shape as over_begin/end/pixels/forget; a separate buffer
// because the tag byte means something different from the over overlay's plain opaque/transparent alpha.
uint32_t       *mix_begin(int width, int height);
void            mix_end();
uint32_t const *mix_pixels(int &width, int &height);
void            mix_forget();

// Snapshots the post-draw scratch bitmap from namcos21_c67_state::capture_mix_sprites into the mix
// overlay. The sentinel fill is 0 (proven safe for this exact c355 draw(...,0) call by capture_over's
// pri1∈{0,2} path, which relies on the same convention), so a pixel value of 0 unambiguously means "no
// layer-0 sprite here" — tag 0, discarded on the GPU. Otherwise the tag replicates
// mix_layer0_sprites' own branch: `pen & 0x5000` is the priority-bank-gated case (tag = 1 + bank, so the
// GPU can recover the bank as tag-1), `pen < 0x1000` is the unconditional-show case (tag 255), and
// anything else is never shown (tag 0) — exactly the third, implicit "leave dest alone" case the
// software loop falls through to. The colour is the same pens[] lookup capture_over uses, packed with
// the tag in the top byte in place of the fixed 0xff alpha.
template <typename Bitmap, typename Rect>
inline void capture_mix(Bitmap const &bm, uint32_t const *pens, Rect const &clip)
{
	const int w = clip.width();
	const int h = clip.height();
	uint32_t *dst = mix_begin(w, h);
	if ((dst == nullptr) || (pens == nullptr))
		return;

	for (int y = 0; y < h; y++, dst += w)
	{
		auto const *const src = &bm.pix(clip.top() + y, clip.left());
		for (int x = 0; x < w; x++)
		{
			auto const pen = src[x];
			uint32_t tag;
			if (pen == 0)
				tag = 0u;
			else if (pen & 0x5000)
				tag = 1u + ((pen >> 8) & 0xfu);
			else if (pen < 0x1000)
				tag = 255u;
			else
				tag = 0u;

			dst[x] = tag ? ((tag << 24) | (uint32_t(pens[pen]) & 0x00ffffffu)) : 0u;
		}
	}

	mix_end();
}

// Frame brackets. frame_begin() is also where the tap performs its one-time attach decision, so it
// must run before any submit(). Both fire from swap_and_clear_poly_framebuffer(): frame_end() closes
// the list that just became visible, frame_begin() opens the freshly cleared work page.
void frame_begin();
void frame_end();

// Plumbing (s21_seam.cpp).
void submit(quad const &q);

// The GPU record consumer (s21_geom.cpp). Called on the emulation thread from the seam plumbing when
// set_gpu(true) has turned capture on; type-free so the seam and the driver need no renderer headers.
void record_begin();
void record_quad(quad const &q);
void record_end();

// The seam helper for the quad site: all scalars (the driver already has them as plain ints), so a
// plain forwarder rather than a template. Inert unless a consumer is attached.
inline void submit_quad(int const x[4], int const y[4], int zsort, uint16_t color)
{
	if (!active())
		return;

	quad q;
	for (int i = 0; i < 4; i++)
	{
		q.x[i] = int16_t(x[i]);
		q.y[i] = int16_t(y[i]);
	}
	q.zsort = zsort;
	q.color = color;
	submit(q);
}

} // namespace s21

#endif // MAME_OSD_LIBRETRO_M2_S21_SEAM_H
