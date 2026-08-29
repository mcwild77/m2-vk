// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 23 / Super System 23 renderer seam — the landing pad for the tapped primitive stream.

    The System 23 analogue of s22_seam.h / s21_seam.h. Unlike System 22 (which has one producer,
    poly3d_drawquad) System 23 has four — render_model / render_direct_poly / render_immediate /
    render_sprite — but they all funnel into a single per-frame list: each builds a
    namcos23_poly_entry and appends it to render.polys[]. render_flush() (namcos23.cpp) then qsorts
    that list by the 24-bit zkey and walks it back-to-front through the 64-way shading dispatch. That
    sorted walk IS the depth result (painter's algorithm, the same model S22 uses), and the entry it
    walks already carries every producer's vertices and parameters. So the seam sits at that one
    unified recorder rather than at the four append sites — one #ifdef S23VK bracket in render_flush:

      * render_flush()  -> frame_begin() at the top of the sorted walk, submit(poly) per entry in
                           SORTED order, frame_end() before the poly_count reset.

    What crosses the seam is a plain snapshot carrying no MAME types (poly below): consumers compile
    without the driver's headers and cannot come to depend on its internals. The seam is
    observation-only at 23-1 — it never touches what the software rasteriser draws, which is what
    keeps the output byte-identical to the 23-0 baseline.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_S23_SEAM_H
#define MAME_OSD_LIBRETRO_M2_S23_SEAM_H

#pragma once

#include <cstdint>

namespace s23 {

// One primitive, snapshotted in the sorted (draw) order render_flush walks. x/y are screen space; p0
// is the first interpolated param (perspective depth cue) captured only for a diagnostic bbox at
// 23-1. num_verts is 1..16 (a near-plane zclip can grow a quad). Everything is a copy — nothing here
// points back into the driver.
struct poly
{
	uint8_t  num_verts;      // 1..16
	float    x[16], y[16];   // screen space (pre-viewport-offset, as the poly_manager rasterises them)
	float    p0[16];         // pv[i].p[0] — ooz (1/z), the screen-linear depth param the fetch divides by
	float    uoz[16];        // pv[i].p[1] — u_texel*ooz; the texel u is uoz/ooz (render_scanline: u*ooz)
	float    voz[16];        // pv[i].p[2] — v_texel*ooz; the texel v is voz/ooz, then + tbase
	float    ish[16];        // pv[i].p[3] — (shade+0.5?)*ooz: the per-vertex shade param; shade = ish/ooz
	int32_t  zkey;           // 24-bit sort key (zsort | absolute_priority<<21) — the depth result

	// The poly's viewport window (render_scanline's clip_left/right/top/bottom are derived from these).
	// The geometry pass bakes the (clip_left, clip_top) origin into the screen position and clips the draw
	// to [clip_left,clip_right) x [clip_top,clip_bottom) — the S23 analogue of S22's per-quad clip rect.
	int16_t  vp_size_x, vp_size_y;
	int16_t  vp_offset_x, vp_offset_y;

	// Which of the four producers built this entry (from namcos23_render_data).
	bool     model;          // render_model  — indexed textured 3D model (the main path)
	bool     direct;         // render_direct_poly — explicit polys streamed over PIO/DMA
	bool     immediate;      // render_immediate   — small immediate-mode polys
	bool     sprite;         // render_sprite      — row*col tiled 2D sprite

	// The six independent booleans render_flush ORs into its 6-bit render_hash (the 64-way shading
	// dispatch). Carried now so the 23-4 shading tail has them from the start; at 23-1 they only feed
	// the tap's per-scene histogram.
	bool     stencil_enabled;
	bool     shade_enabled;
	bool     pfade_enabled;      // poly fade
	bool     colorfade;          // fadefactor != 0xff (screen/color fade)
	bool     blend_enabled;
	bool     poly_alpha;         // alpha != 0xff

	// 23-4 poly-fade (render_hash bit3) and colour-fade (bit2) factors — both pure per-pixel colour
	// math, no destination read. The booleans above say whether each is on; these carry the multipliers,
	// all u8 in the driver (poly_fade_* / screen_fade_*). Poly-fade: c = (c * polycolor) >> 8. Colour-fade:
	// c = (c*fadefactor + fadecolor*(0x100 - fadefactor)) >> 8 (the inverse is recovered in the shader).
	uint8_t  polycolor_r, polycolor_g, polycolor_b;
	uint8_t  fadefactor;
	uint8_t  fadecolor_r, fadecolor_g, fadecolor_b;

	// 23-4 blend (render_hash bit1) and poly-alpha (bit0) — the two that READ the framebuffer. Both are
	// realised with fixed-function blend over the painter's pass (which is already back-to-front, so the
	// hardware over-blend matches). blend is a fixed 50%; poly-alpha is src*alpha + dst*(0x100-alpha), but
	// only where the per-texel gate (alpha_enabled || raw_pen == alpha_pen) passes — the shader computes
	// the per-fragment weight and hands it to the blend unit. poly-alpha wins over blend where its gate
	// passes, matching render_scanline's if/else-if. alpha is rd.alpha (0xff = opaque); alpha_inv is
	// recovered as 0x100 - alpha in the shader. See the "which one for low-spec" note in plan_system23.md.
	uint8_t  alpha;          // rd.alpha (= 0xff - poly_alpha); poly_alpha bool above is (alpha != 0xff)
	uint8_t  alpha_pen;      // rd.poly_alpha_pen — the raw texel byte that gates per-texel alpha
	bool     alpha_enabled;  // rd.alpha_enabled — when set, every pixel of the poly blends (no pen gate)

	uint8_t  cmode;          // colour/blend mode
	int32_t  tbase;          // texture base (model/direct/immediate); -1 when none
	uint32_t pens_base;      // rd.pens - m_palette->pens(): the poly's palette bank (0..0x7f00), the base
	                         // texture_lookup indexes with the cmode-adjusted pen
	uint16_t model_id;       // model index (render_model), else 0
};

// The texture ROM the fetch reads, as raw pointers into the driver's arrays (23-3). The two decoded
// arrays and texrom are ROM-derived and stable after the renderer's constructor decodes them, so they
// upload once; the palette pointer is stable too but its contents change per frame, so the consumer
// re-reads it every frame. This is the System 23 analogue of s22_seam.h's texture_ram; System 23's tile
// system is simpler — the tileid->address indirection is already resolved into tmrom_decoded, and there
// is no separate ayx table (texture_lookup addresses texrom directly).
struct texture_rom
{
	uint32_t const *tmrom_decoded = nullptr;   // tileid -> texrom base ((tmlrom|attr<<16 & tile_mask)<<8)
	uint8_t  const *texattr_decoded = nullptr; // tileid -> orientation (bit0 vflip, bit1 uflip, bit2 swap)
	uint32_t        decoded_count = 0;          // (tileid_mask|0xff)+1 — the length of both arrays above
	uint8_t  const *texrom = nullptr;           // the "textile" region: 8bpp texel bytes, 256 per tile
	uint32_t        texrom_bytes = 0;           // its real size (tile_mask+1)*256
	uint32_t const *palette = nullptr;          // m_palette->pens(), 0x00RRGGBB pens, re-read each frame
	uint32_t        palette_count = 0;          // m_palette->entries() (0x8000)
	uint32_t        tileid_mask = 0;            // the y-mask in tileid = (u>>4)&0xff | (v<<4)&tileid_mask

	// 23-4 stencil: the C412 sram (m_texram) — "ram-based tiles for alpha-cutout drawing". stencil_lookup
	// reads it per pixel to decide whether a stencil_enabled poly draws. Unlike the ROM-derived arrays this
	// is live RAM the game writes, so — like the palette — the pointer is stable but the contents change per
	// frame, and the consumer re-reads it every frame. u16 entries; the shader loads them out of a u32 view.
	uint16_t const *texram = nullptr;           // m_c412.sram, 0x20000 u16 (256 KB)
	uint32_t        texram_count = 0;           // 0x20000
};

// Hands the consumer the texture ROM pointers (above). Called by the driver from the render_flush frame
// bracket under S23VK; the pointers are stable, so the consumer uploads the ROM-derived buffers once and
// re-reads only the palette each frame.
void set_texture_rom(texture_rom const &t);
texture_rom const &get_texture_rom();

// 23-5: the 2D-over text overlay. System 23 sandwiches the text/HUD tilemap over the 3D, exactly as
// System 22's plain path does (screen_update_namcos22). screen_update draws the text into `bitmap` and
// marks its pixels priority 4 in screen.priority(); with the GPU owning the 3D nothing else writes the
// priority buffer, so priority 4 is the whole text layer. capture_over (below) snapshots those pixels as
// an opaque overlay, transparent elsewhere, and the renderer redraws it above the GPU 3D. The software
// render_flush loop forces prioverchar = 2 on every primitive, so no primitive ever sits over the text
// (MAME's priority-7 case never arises here) — there is no over-pass to match, unlike Super System 22.
//
// over_begin returns where to write w*h pixels (or nullptr if none capturing); over_end marks it
// readable; over_pixels returns it to the frontend or nullptr if none this frame or the overlay is
// disabled; over_forget drops it (called at frame_begin so a frame that captures none presents the
// passthrough, not a stale overlay).
uint32_t *over_begin(int width, int height);
void      over_end();
uint32_t const *over_pixels(int &width, int &height);
void      over_forget();

// system23_2d_overlay (deferred to 23-7's option set): whether the captured HUD/text is drawn back over
// the GPU 3D. On by default (the accurate sandwich). M2VK_S23_HUD overrides it (0 = force off). Read in
// over_pixels(), which returns nullptr when off, collapsing the whole OVER draw.
void set_option_hud(bool on);

// The seam entry points. namcos23.cpp calls these under S23VK; the shared OSD always links them, inert
// until a tap is attached (M2VK_S23TAP) or the GPU path is armed (23-2+).
void frame_begin(int variant);   // variant reserved for the driver subclass family (0 for now)
void frame_end();
void submit(poly const &p);

namespace detail {
extern bool g_active;        // any consumer wants the stream (tap or, later, the GPU path)
extern bool g_sw_owns_3d;    // the software rasteriser still draws the 3D (true until the GPU path)
} // namespace detail

// True once any consumer is attached. The submit hooks in the driver are cheap, but the caller can
// gate the snapshot build on this.
inline bool active()      { return detail::g_active; }
inline bool sw_owns_3d()  { return detail::g_sw_owns_3d; }

// 23-2 GPU capture. set_gpu(true) attaches the record consumer (active() goes true) and hands
// sw_owns_3d() back false, so render_flush stops walking the 64-way software dispatch and the GPU owns
// the 3D. set_gpu(false) is the reverse. Called by the OSD once the renderer decision is made, for the
// namcos23 family only — in the other builds these are compiled but never called (no S23 seam fires).
void set_gpu(bool on);

// M2VK_NO_3D: neither the GPU nor the software rasteriser draws the 3D — the background reference. Hands
// sw_owns_3d() back false too, so the driver skips render_flush's 3D and the picture is just the 2D
// layers. Capture stays off, so the seam is inert.
void set_no_3d();

// True only when the GPU genuinely owns the 3D (set_gpu(true)) — unlike sw_owns_3d(), which is ALSO
// false under set_no_3d()'s "neither draws" reference. Reserved for later phases (2D-over gating); the
// driver's render_flush takes the same !sw_owns_3d() branch either way at 23-2.
bool gpu_owns_3d();

// The GPU record consumer (s23_geom.cpp). Called on the emulation thread from the seam plumbing when
// set_gpu(true) has turned capture on; type-free so the seam and the driver need no renderer headers.
void record_begin(int variant);
void record_poly(poly const &p);
void record_end();

// The 2D-over capture (23-5). Called from screen_update after both text-mix passes; snapshots the pixels
// whose priority equals `prival` (the text tiles, 4) as an opaque overlay and leaves every other pixel
// transparent, so the renderer can redraw the HUD above the GPU 3D. A no-op while the software rasteriser
// still owns the 3D (the passthrough already has the text on top then), so it costs one predicate in the
// software A/B and the diagnostic tap. Templated so this header need not know the MAME types — Bitmap is
// a bitmap_rgb32, Priority a bitmap_ind8, Rect a rectangle. The high byte is forced to 0xff so a text
// pixel that happens to be pure black is not read as transparent by the overlay shader (which discards an
// all-zero texel). This is s22_seam.h's capture_over, verbatim in intent.
template <typename Bitmap, typename Priority, typename Rect>
inline void capture_over(Bitmap const &bm, Priority const &pri, int prival, Rect const &clip)
{
	if (sw_owns_3d())
		return;

	const int w = clip.width();
	const int h = clip.height();
	if ((w <= 0) || (h <= 0))
		return;

	uint32_t *dst = over_begin(w, h);
	if (dst == nullptr)
		return;

	for (int y = 0; y < h; y++, dst += w)
	{
		auto const *const srcpix = &bm.pix(clip.top() + y, clip.left());
		auto const *const pripix = &pri.pix(clip.top() + y, clip.left());
		for (int x = 0; x < w; x++)
			dst[x] = (int(pripix[x]) == prival) ? (0xff000000u | (uint32_t(srcpix[x]) & 0x00ffffffu)) : 0u;
	}

	over_end();
}

} // namespace s23

#endif // MAME_OSD_LIBRETRO_M2_S23_SEAM_H
