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
	float    ooz[6];         // 1/z, perspective-correct
	float    bri[6];         // per-vertex brightness, (bri+0.5)*ooz as the rasteriser receives it —
	                         // the untextured Gouraud shade. Divide by ooz to recover the raw value.
	uint32_t basecolor;      // pens[0] as 0x00RRGGBB: the flat colour an untextured polygon takes,
	                         // i.e. the value renderscanline_poly reads with pen == 0. The S2
	                         // "untextured first" colour before any texel is sampled.
	uint16_t color;          // color & 0x7f
	uint8_t  cmode;          // colour/blend mode; 0 once objectflags disables textures
	bool     ss22;           // Super System 22 shading path (renderscanline_poly_ss22)
	bool     textured;
	bool     alpha;          // SS22 poly alpha enabled
	bool     direct;         // pre-projected ("direct") quad, not z-clipped
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
		bool ss22, bool textured, bool alpha, bool direct, uint32_t basecolor)
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
		q.ooz[i] = float(v[i].p[0]);
		q.bri[i] = float(v[i].p[3]);     // (bri + 0.5) * ooz, as the scanline renderer receives it
	}
	for (int i = n; i < 6; i++)
		q.x[i] = q.y[i] = q.ooz[i] = q.bri[i] = 0.0f;

	q.basecolor = basecolor & 0x00ffffffu;
	q.color    = uint16_t(color & 0x7f);
	q.cmode    = uint8_t(cmode);
	q.ss22     = ss22;
	q.textured = textured;
	q.alpha    = alpha;
	q.direct   = direct;
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

} // namespace s22

#endif // MAME_OSD_LIBRETRO_M2_S22_SEAM_H
