// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco (Super) System 22 renderer seam — the landing pad for the tapped primitive stream.

    The System 22 analogue of m2vk_sink.h. The seam is namcos22_renderer in
    src/mame/namco/namcos22_v.cpp: the geometry engine has already sorted every primitive into a
    z-keyed radix tree, and render_scene() walks that tree back-to-front — which is where the depth
    key comes from for free, the same role draw order plays for Model 2. Three #ifdef S22VK sites
    there call into this header and nothing else:

      * poly3d_drawquad()  -> submit_quad(), once the quad is projected/clipped and its parameters
                              resolved into namcos22_object_data, immediately before render_triangle_fan;
      * render_sprite()    -> submit_sprite(), once per sprite node (a rows*cols block of tiles);
      * render_scene()     -> frame_begin()/frame_end() bracket the whole back-to-front walk, so a
                              single hooked function covers both screen_update variants.

    What crosses the seam is a plain snapshot carrying no MAME types (quad / sprite below): consumers
    compile without the driver's headers and cannot come to depend on its internals, and the
    conversion lives in one place. The seam is observation-only in S1 — it never touches what the
    software rasteriser draws, which is what keeps the output byte-identical to the S0 baseline.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_S22_SEAM_H
#define MAME_OSD_LIBRETRO_M2_S22_SEAM_H

#pragma once

#include <cstdint>

namespace s22 {

// One projected quad, as it reaches render_triangle_fan. num_verts is 3..6 (a near-plane z-clip can
// turn the source quad into up to six vertices); x/y are screen space and ooz is 1/z, the depth the
// hardware path will interpolate. Everything is a copy — nothing here points back into the driver.
struct quad
{
	uint8_t  num_verts;      // 3..6
	float    x[6], y[6];     // screen space
	float    ooz[6];         // 1/z, perspective-correct (the driver's clipv.p[0])
	float    uoz[6], voz[6]; // (u+0.5)*ooz, (v+0.5)*ooz (clipv.p[1]/p[2]) — divide by ooz for the texel
	float    bri[6];         // per-vertex brightness, (bri+0.5)*ooz as the rasteriser receives it —
	                         // the untextured Gouraud shade. Divide by ooz to recover the raw value.
	uint32_t basecolor;      // pens[0] as 0x00RRGGBB: the flat colour an untextured polygon takes,
	                         // i.e. the value renderscanline_poly reads with pen == 0. Used directly
	                         // for the untextured path; the textured path samples the tile instead.
	uint16_t color;          // color & 0x7f — the palette bank ((color & 0x7f) << 8 is the pens base)
	uint16_t texturebank;    // node->data.quad.texturebank; bn = texturebank << 12 in the fetch
	uint8_t  cmode;          // colour/blend mode; also selects the pen mask/shift the fetch applies
	bool     ss22;           // Super System 22 shading path (renderscanline_poly_ss22)
	bool     textured;       // extra.texture_enabled: sample the tile, else use basecolor
	bool     shade_enabled;  // extra.shade_enabled: apply the per-pixel hardware brightness
	bool     direct;         // pre-projected ("direct") quad, not z-clipped

	// The per-quad part of the shading tail (renderscanline_poly / _ss22). The per-frame globals
	// (screen fade, poly fade, alpha factor, alpha pen) are handed over once a frame by
	// set_shading_state(), not carried per quad — they are the same for every quad in the frame.
	uint8_t  fogfactor;      // extra.fogfactor (0..255); the scanline blends with 0xff - this
	uint32_t fogcolor;       // 0x00RRGGBB — per-cztype (plain S22) or the global fog colour (SS22)
	bool     zfog_enabled;   // SS22 per-z fog: look the factor up in czram[cz] per pixel
	uint8_t  cz_bank;        // 0..3, which recalc_czram table the z-fog reads
	int16_t  cz_sdelta;      // signed delta added to czram[cz] before the fog blend
	bool     alpha_enabled;  // SS22: (color & 0x7f) != m_poly_alpha_color — this quad's colour blends

	// The quad's own clip window (m_cliprect: the scene viewport vl/vr/vu/vd intersected with the visible
	// area), inclusive, in bitmap pixels. render_triangle_fan clips every scanline to this; the GPU pass
	// applies it as a per-run scissor. It is not decoration: SS22 games (tokyowar) window the 3D into a
	// letterbox — without it the sky bleeds into the black bars the clip leaves. Full-screen (0,0,639,479)
	// for the common unclipped quad, so those runs get a no-op full scissor.
	int16_t  clip_l, clip_t, clip_r, clip_b;
};

// The per-quad shading tail as it is built at the seam call site, from namcos22_object_data. A plain
// POD so the header needs none of the driver's types; copied straight into the fields on quad above.
struct quad_shading
{
	uint32_t fogcolor = 0;        // 0x00RRGGBB
	uint16_t fogfactor = 0;       // extra.fogfactor
	int16_t  cz_sdelta = 0;
	uint8_t  cz_bank = 0;
	bool     zfog_enabled = false;
	bool     alpha_enabled = false;
};

// The per-frame globals of the SS22 shading tail. Set once a frame by set_shading_state() from the
// frame bracket; the same values apply to every quad in the frame, so they ride a push constant
// (fade/poly-fade/alpha) or a per-slot buffer (the four recalc_czram z-fog tables), not the vertex.
struct shading_globals
{
	int alpha_pen = 0;        // m_poly_alpha_pen — the pen value that forces a per-pixel alpha blend
	int alpha_factor = 0;     // m_poly_alpha_factor — the alpha weight (extra.alpha)
	int fade_factor = 0;      // m_screen_fade_factor, 0 when the global fade is disabled
	int fade_r = 0, fade_g = 0, fade_b = 0;   // m_screen_fade_* (SS22 is 16-bit, 0x100 = 1.0)
	bool poly_fade_enabled = false;           // m_poly_fade_enabled
	int poly_r = 0, poly_g = 0, poly_b = 0;   // m_poly_fade_* (byte scales)
	uint8_t const *czram[4] = { nullptr, nullptr, nullptr, nullptr };  // the four recalc_czram tables
};

// The texture system the fetch reads, as raw pointers into the driver's arrays. Set once by the
// driver (the pointers are stable after init_tables); the palette pointer is stable too, its contents
// updated as the game writes palette RAM, so the consumer re-reads it every frame. Sizes are fixed by
// the hardware and are constants in the consumer (s22_geom.cpp), so only the pointers cross the seam.
struct texture_ram
{
	uint16_t const *ttmap = nullptr;    // tile numbers, 0x100000 entries
	uint8_t  const *ttattr = nullptr;   // per-tile orientation, 0x100000 entries (unpacked nibbles)
	uint8_t  const *ttdata = nullptr;   // 8bpp tile pixels, 256 bytes per tile, up to 0x1000000
	uint8_t  const *ayx = nullptr;      // attr/y/x -> in-tile pixel offset, 0x1000 entries
	uint32_t const *palette = nullptr;  // m_palette->pens(), 0x8000 entries of 0x00RRGGBB
	uint8_t  const *gamma = nullptr;    // m_gamma_proms: rlut[0x100]|glut[0x100]|blut[0x100], the plain
	                                    // System 22 final gamma LUT (namcos22_mix_text_layer). Static,
	                                    // ROM-derived; null on Super System 22 (which has no gamma PROMs).
};

// One sprite node. render_sprite() expands it to rows*cols poly3d_drawsprite() calls; the seam taps
// the node rather than each tile, which is the primitive the tree actually carries.
struct sprite
{
	uint16_t tiles;          // rows * cols
	int16_t  xpos, ypos;
	int16_t  sizex, sizey;
	uint16_t color;          // color & 0x7f
	bool     ss22;
};

namespace detail {

// True only while a consumer is attached. Read at the seam for every primitive, so it is a plain
// bool rather than a call; written once, when the tap decides whether to attach (first frame_begin).
extern bool g_active;

} // namespace detail

// Cheap enough to sit in front of the per-primitive conversion below. A build that attaches nothing
// leaves this false forever and the seam costs one predicate per primitive.
inline bool active() { return detail::g_active; }

// S2 GPU capture. set_gpu(true) turns the hardware path on: it attaches the record consumer (so
// active() goes true) and, because the GPU now owns the 3D, hands sw_owns_3d() back false so the
// driver stops calling render_triangle_fan. Called by the OSD once the renderer=vulkan decision is
// made, for the namcos22 subtarget only — in the Model 2 build these are compiled but never called.
void set_gpu(bool on);

// Hands the consumer the texture system's pointers (above). Called by the driver from the frame
// bracket; cheap enough to call every frame since it only stores pointers. Inert unless capturing.
void set_texture_ram(uint16_t const *ttmap, uint8_t const *ttattr, uint8_t const *ttdata,
		uint8_t const *ayx, uint32_t const *palette, uint8_t const *gamma);
texture_ram const &get_texture_ram();

// The per-frame globals of the SS22 shading tail (shading_globals above). Called by the driver from
// the frame bracket, once a frame; cheap (stores values and four stable pointers). Inert unless
// capturing. The czram tables' contents change per frame, so the consumer re-reads them each frame.
void set_shading_state(shading_globals const &g);
shading_globals const &get_shading_globals();

// The 2D-over overlay (s22_seam.cpp). The driver mixes its text/HUD layer straight onto the finished
// 2D frame, so with the GPU owning the 3D that text lands UNDER the polygons — the GPU 3D draws over
// the whole 2D frame. capture_over() (below) snapshots the text pixels, identified by the priority
// buffer, into a transparent overlay the renderer draws again after the 3D, giving System 22 the same
// UNDER/OVER sandwich Model 2 has. over_begin returns a width*height buffer to fill (0 = transparent),
// over_end marks it readable, over_pixels returns it to the frontend or nullptr if none this frame.
uint32_t       *over_begin(int width, int height);
void            over_end();
uint32_t const *over_pixels(int &width, int &height);
void            over_forget();

// True while MAME's own scanline rasteriser should still draw the 3D quads. Default true, so a build
// that never turns the GPU path on (S0/S1, or the diagnostic tap alone) keeps drawing in software.
// Read at the two render_triangle_fan sites in namcos22_v.cpp.
namespace detail { extern bool g_sw_owns_3d; }
inline bool sw_owns_3d() { return detail::g_sw_owns_3d; }

// Frame brackets. variant 0 = plain System 22 screen_update, 1 = Super System 22. frame_begin() is
// also where the tap performs its one-time attach decision, so it must run before any submit().
void frame_begin(int variant);
void frame_end();

// Plumbing (s22_seam.cpp). The two submit() overloads take the snapshots above.
void submit(quad const &q);
void submit(sprite const &s);

// The GPU record consumer (s22_geom.cpp). Called on the emulation thread from the seam plumbing when
// set_gpu(true) has turned capture on; type-free so the seam and the driver need no renderer headers.
// record_begin resets the frame's quad list, record_quad appends one, record_end marks it readable.
void record_begin(int variant);
void record_quad(quad const &q);
void record_end();

// Type-free seam helper for the quad site. Vertex is the driver's poly vertex (it exposes .x, .y and
// .p[0] = 1/z), deduced at the one call site so this header needs none of the driver's headers.
template <typename Vertex>
inline void submit_quad(Vertex const *v, int nverts, int color, int cmode,
		bool ss22, bool textured, bool direct, uint32_t basecolor,
		int texturebank, bool shade_enabled, quad_shading const &sh,
		int clip_l, int clip_t, int clip_r, int clip_b)
{
	if (!active())
		return;

	quad q;
	q.num_verts = uint8_t(nverts);
	const int n = (nverts < 6) ? nverts : 6;
	for (int i = 0; i < n; i++)
	{
		q.x[i]   = float(v[i].x);
		q.y[i]   = float(v[i].y);
		q.ooz[i] = float(v[i].p[0]);     // 1/z (the driver's param[0])
		q.bri[i] = float(v[i].p[3]);     // (bri + 0.5) * ooz, as the scanline renderer receives it
	}
	// The textured fetch also needs the interpolated (u+0.5)*ooz and (v+0.5)*ooz — clipv.p[1]/p[2].
	for (int i = 0; i < n; i++)
	{
		q.uoz[i] = float(v[i].p[1]);
		q.voz[i] = float(v[i].p[2]);
	}
	for (int i = n; i < 6; i++)
		q.x[i] = q.y[i] = q.ooz[i] = q.bri[i] = q.uoz[i] = q.voz[i] = 0.0f;

	q.basecolor    = basecolor & 0x00ffffffu;
	q.color        = uint16_t(color & 0x7f);
	q.texturebank  = uint16_t(texturebank);
	q.cmode        = uint8_t(cmode);
	q.ss22         = ss22;
	q.textured     = textured;
	q.shade_enabled = shade_enabled;
	q.direct       = direct;

	q.fogfactor    = uint8_t(sh.fogfactor);
	q.fogcolor     = sh.fogcolor & 0x00ffffffu;
	q.zfog_enabled = sh.zfog_enabled;
	q.cz_bank      = uint8_t(sh.cz_bank & 3);
	q.cz_sdelta    = sh.cz_sdelta;
	q.alpha_enabled = sh.alpha_enabled;
	q.clip_l       = int16_t(clip_l);
	q.clip_t       = int16_t(clip_t);
	q.clip_r       = int16_t(clip_r);
	q.clip_b       = int16_t(clip_b);
	submit(q);
}

// Sprite site: all scalars, so a plain forwarder rather than a template.
inline void submit_sprite(int rows, int cols, int xpos, int ypos,
		int sizex, int sizey, int color, bool ss22)
{
	if (!active())
		return;

	sprite s;
	s.tiles = uint16_t(rows * cols);
	s.xpos  = int16_t(xpos);
	s.ypos  = int16_t(ypos);
	s.sizex = int16_t(sizex);
	s.sizey = int16_t(sizey);
	s.color = uint16_t(color & 0x7f);
	s.ss22  = ss22;
	submit(s);
}

// The 2D-over capture. Called from screen_update after the text layer has been mixed onto the finished
// frame; snapshots the pixels whose priority equals `prival` (the text tiles) as an opaque overlay and
// leaves every other pixel transparent, so the renderer can redraw the HUD above the GPU 3D. A no-op
// while the software rasteriser still owns the 3D (the passthrough already has the text on top then),
// so it costs one predicate in S0/S1 and the diagnostic tap. Templated for the same reason submit_quad
// is — Bitmap is a bitmap_rgb32, Priority a bitmap_ind8, Rect a rectangle, and this header must not
// know that. The high byte is forced to 0xff so a text pixel that happens to be pure black is not read
// as transparent by the overlay shader (which discards an all-zero texel).
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

} // namespace s22

#endif // MAME_OSD_LIBRETRO_M2_S22_SEAM_H
