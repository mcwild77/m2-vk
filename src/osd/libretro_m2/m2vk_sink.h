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
    That is deliberate:

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

#include <cstdint>

namespace m2vk {

// as declared by model2_state::polygon
enum : int { MAX_VERTICES = 8 };

struct vertex
{
	float   x, y;       // screen space, after projection and viewport transform
	float   rz;         // textured polygons: 1/z. solid polygons: z, unreciprocated — the solid
	                    // scanline renderer ignores the vertex parameters, so model2_v.cpp only
	                    // reciprocates for the textured case. Beware when comparing depth ranges
	                    // across renderer classes.
	float   uz, vz;     // u/z, v/z, premultiplied by rz and scaled by 1/8. Textured only.
};

// One polygon, exactly as the software rasterizer is about to receive it. The texture fields are
// zero unless (renderer & 2) — model2_v.cpp resolves them only for textured polygons, and the
// object_data slot they live in is recycled, so anything else in there is stale.
struct poly
{
	vertex          v[MAX_VERTICES];
	uint8_t         num_verts;
	uint8_t         renderer;       // bit1 = textured, bit0 = translucent; indexes m_render_callbacks
	uint16_t        bucket;         // sort bucket (polygon::z). Within one bucket, draw order is
	                                // submission order, and only submission order breaks the tie.
	uint8_t         window;
	uint16_t        texheader[4];   // the raw header words, for anything not decoded below
	uint8_t         luma;
	int32_t         texlod;
	int16_t         viewport[4];
	int16_t         center[2];
	int32_t         clip[4];        // viewport after clipping against the screen: left, top, right, bottom

	// resolved parameters, i.e. m2_poly_extra_data
	uint32_t        lumabase;
	uint32_t        colorbase;
	uint8_t         checker;
	uint32_t        texwidth, texheight;
	uint32_t        texx, texy;
	uint8_t         texwrapx, texwrapy;
	uint8_t         texmirrorx, texmirrory;
	uint8_t         sheet;          // 0 = textureram0, 1 = textureram1
	uint8_t         utex;           // microtexture enable
	uint8_t         utexminlod;
	uint32_t        utexx, utexy;
	uint32_t const *texsheet[2];    // live texture RAM: [0] this polygon's sheet, [1] the other one.
	                                // Valid for the duration of the submit() call only.
};

// Implemented by anything that wants the stream. run_begin/run_end bracket one machine; the frame
// pair brackets one rendered frame. Every method is optional.
class consumer
{
public:
	virtual ~consumer() = default;

	virtual void run_begin() { }
	virtual void run_end() { }
	virtual void frame_begin(uint32_t submitted) { }
	virtual void submit(poly const &p) { }
	virtual void frame_end() { }
};

namespace detail {

// True while at least one consumer is attached. Read at the seam for every polygon, so it is a
// plain bool rather than a call into the sink; written only when consumers are attached or dropped.
extern bool g_active;

} // namespace detail

// Cheap enough to sit in front of the per-polygon conversion below.
inline bool active() { return detail::g_active; }

// One machine's run. Called by the OSD from init() and osd_exit().
void sink_open();
void sink_close();

// The seam, driver-type-free half.
void frame_begin(uint32_t submitted);
void submit(poly const &p);
void frame_end();

// The seam, driver-facing half: converts one polygon and hands it over. Templated so that this
// header carries no dependency on the driver's headers; Polygon is model2_state::polygon, Extra is
// m2_poly_extra_data and Rect is the clipped viewport rectangle.
template <typename Polygon, typename Extra, typename Rect>
inline void submit(Polygon const &src, Extra const &extra, uint8_t renderer, Rect const &vp)
{
	if (!active())
		return;

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
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_SINK_H
