// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 — the frame record: what the emulation thread hands the renderer.

    Three things cross here, and they are the whole of what the hardware renderer is given:

      * the polygon stream, one fixed-size m2vk::poly per polygon, in draw order;
      * the colour chain the shading needs — the luma translator and the colour ramps — snapshotted
        once per frame;
      * the two 2D tilemap layers that sandwich the 3D.

    The sandwich is why the layers are here at all. model2_state::screen_update() builds the picture
    in three passes (src/mame/sega/model2_v.cpp):

      1. palette pen 0, then System-24 tilemap layers 3..0, low priority half   -> the UNDER layer
      2. render_polygons() — the 3D — copied on top with 0x00000000 transparent
      3. the same four layers again, high priority half, pen 0 transparent      -> the OVER layer

    So the 3D sits between two 2D layers that MAME draws and we do not. There is no way to composite
    a hardware-rendered 3D layer on top of MAME's finished frame; the foreground has to go over it.
    Hence this: the two 2D layers are captured separately and the compositing moves to the GPU, where
    the 3D can be inserted between them.

    The under layer is captured AFTER render_polygons, which is deliberate and is what lets one hook
    serve both renderers. While the software rasteriser still owns the 3D it is already in there, and
    the composite is under + over — exactly MAME's own result, which is what makes the step verifiable
    by cmp. Once m2vk::rasterize() goes false the same capture yields the background alone and the
    GPU's 3D goes in the gap.

    Threading: everything here is written on the emulation thread, during screen_update, and read on
    the frontend's thread from retro_run. Those never overlap — the emulation thread is parked on the
    OSD's baton for the whole of retro_run — which is why one record suffices and there is no lock.
    It is the same argument that already covers the OSD's framebuffer copy.

    Storage is reused and never reallocated in a steady state. The layers are resized only when the
    picture's geometry changes; the polygon vector grows to the frame's high-water mark and stops;
    the two table snapshots are fixed size. A run that has been going for a second does no allocation
    at all, which is what keeps RSS flat.

    A frame that renders nothing (skip_redraw, or render_polygons taking its m_render_done early
    return) leaves the record alone, so the renderer re-presents what it already has. That matches
    what MAME does with its own bitmap, and it is the "keep last frame's 3D" case.

    An EMPTY DISPLAY LIST is not that case and the difference is the whole of geometry_none(). When
    the geometry engine queues no polygons at all, render_polygons bails without copying its previous
    destmap, so MAME's 3D layer for that frame is blank. Left to look like the dupe above it, the
    hardware renderer would go on drawing the last list it was handed for the rest of the run —
    measured, before this was fixed, as vstriker compositing a whole football pitch under the
    copyright card. So the empty list is recorded as a real frame with zero polygons in it.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_FRAME_H
#define MAME_OSD_LIBRETRO_M2_M2VK_FRAME_H

#pragma once

#include <cstdint>
#include <vector>

namespace m2vk {

//============================================================
//  the polygon, as it crosses
//============================================================

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
	uint16_t        bucket;         // sort bucket (polygon::z). Within one bucket the tie is broken
	                                // by REVERSE submission order: model2_v.cpp:520-522 prepends each
	                                // polygon to its bucket's list, so render_polygons walks the
	                                // newest first. Nothing here has to undo that — the record is
	                                // taken at the seam, i.e. in traversal order, so the reversal is
	                                // already baked into the stream and into the draw-order key.
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

	// m_palram[colorbase + 0x1000], resolved on the emulation thread at submit time because that is
	// the only place it can be: the scanline callbacks read m_palram during render_polygons, which is
	// on this same thread inside screen_update, so no CPU write can land between the submit and the
	// raster. One u16 per polygon instead of a 16 KB table per frame.
	//
	// Stored raw. The solid path masks it with 0xffff and the textured path with 0x7fff, but both
	// read only the three 5-bit fields in bits 0..14, so bit 15 is never looked at either way.
	uint16_t        palcolor;

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
	                                // These are memory shares owned by the machine, so they stay valid
	                                // for its whole life — which is what makes it safe to keep them in
	                                // the record and read them on the frontend's thread.
};


//============================================================
//  the colour chain
//============================================================

// What the shading tail reads, handed over at frame_begin as pointers into model2_state and copied
// into the record immediately. Snapshotting once per frame is safe for the same reason resolving
// palcolor per polygon is: the software rasterizer reads these during render_polygons, on this
// thread, so nothing can write them between the snapshot and the draw they describe.
struct frame_tables
{
	uint16_t const *colorxlat;      // m_colorxlat: three 32x256 ramps at u16 offsets 0, 0x2000, 0x4000
	uint8_t  const *lumaram;        // m_lumaram: the luma translator, indexed by lumabase + (texel >> 1)
	uint8_t  const *gamma;          // m_gamma_table: computed once in video_start and never written again

	// The two texture sheets, m_textureram0 and m_textureram1, indexed the way the sheet bit in
	// texheader[2] names them rather than the way any one polygon sees them. These are memory shares
	// owned by the machine, so unlike the three tables above they are not snapshotted: they stay valid
	// for the machine's whole life and are read straight through on the frontend's thread, which is
	// safe for exactly the reason the snapshots are — the emulation thread is parked for all of it.
	uint32_t const *texram[2];
	uint32_t        texram_words[2];    // what the share actually is, in u32 words. get_texel can only
	                                    // reach 0x40000 of them; a share smaller than that is a driver
	                                    // variant we upload short and zero-fill.
};

enum : uint32_t
{
	COLORXLAT_ENTRIES = 0x6000,     // 0xc000 bytes of u16 in model2_state
	LUMARAM_ENTRIES   = 0x8000,
	GAMMA_ENTRIES     = 256,

	// One sheet, as get_texel addresses it: offset is a 16-bit-word index reaching 0x80000, and the
	// fetch is sheet[offset >> 1], so 0x40000 u32 words — exactly 1 MB. Both sheets together are the
	// whole of Model 2's texture memory, which is why there is no atlas and no cache anywhere in this
	// renderer: it is cheaper to hold all of it than to work out which part is wanted.
	TEXRAM_SHEET_WORDS = 0x40000
};


//============================================================
//  the record
//============================================================

enum : int
{
	LAYER_UNDER = 0,    // opaque background; carries the software 3D while the rasteriser owns it
	LAYER_OVER  = 1,    // foreground tilemaps, pixel value 0 is transparent
	LAYER_COUNT = 2
};

struct frame_layer
{
	std::vector<uint32_t> pixels;   // width * height, tightly packed 0xAARRGGBB
	int  width = 0;
	int  height = 0;
	bool valid = false;             // false until the first screen_update has filled it
};

struct frame_record
{
	frame_layer layer[LAYER_COUNT];

	// The polygon stream in draw order. polys.size() is the high-water mark and poly_count is how
	// many of them this frame actually filled; never read past poly_count.
	std::vector<poly> polys;
	uint32_t poly_count = 0;
	uint32_t submitted = 0;         // raster->poly_list_index, what the geometry engine put in the
	                                // list. Equal to poly_count in every frame observed so far, and
	                                // the pair is what proves the record sees the whole stream.
	bool     geometry_valid = false;
	uint64_t geometry_serial = 0;   // bumped by geometry_end. The renderer uses it to tell a frame
	                                // with new geometry from a duped one, which look identical.

	// The colour chain, sized once. colorxlat has the gamma table already folded into it —
	// gamma[colorxlat[i] & 0xff] — because every reader of one is a reader of the other and the
	// shader would otherwise do two dependent lookups per component per pixel.
	std::vector<uint8_t> colorxlat;
	std::vector<uint8_t> lumaram;
	bool     tables_valid = false;
	uint64_t tables_serial = 0;     // bumped only when the bytes actually changed, so the renderer
	                                // can skip the upload rather than push 56 KB every frame.

	// Texture RAM, carried as the live pointers rather than copied. 2 MB is too much to snapshot on
	// the emulation thread for no gain: the frontend reads it while that thread is parked, which is
	// the same guarantee the snapshot would have been resting on anyway. Null until a frame with
	// geometry has been captured.
	uint32_t const *texram[2] = { nullptr, nullptr };
	uint32_t        texram_words[2] = { 0, 0 };

	// Bumped every time a complete pair of layers lands. The renderer does not need it to be correct
	// — it presents whatever is current — but it is the cheap way to tell "the emulator produced
	// nothing this frame" from "the capture is broken", which look identical in a picture.
	uint64_t serial = 0;

	bool complete() const { return layer[LAYER_UNDER].valid && layer[LAYER_OVER].valid; }
};

// Capture is off unless the Vulkan path asked for it: in software mode the hooks in screen_update()
// and at the seam must cost a predicate and nothing else, or renderer=software stops being the
// reference it exists to be.
namespace detail {

extern bool g_capturing;

} // namespace detail

inline bool capturing() { return detail::g_capturing; }

// Emulation thread, from the seam in model2_3d_render / render_polygons. geometry_begin resets the
// count and snapshots the tables; geometry_end is what marks the frame readable, so a capture
// abandoned half-way reads as "no geometry" rather than as a torn frame.
void geometry_begin(uint32_t submitted, frame_tables const &tables);
void geometry_submit(poly const &p);
void geometry_end();

// The third case: a new display list that is empty. render_polygons bails on it before it draws
// anything, so MAME's own 3D layer is blank for that frame — which is the opposite of the dupe case
// above it, where the blank record means "keep what you have". There is no stream to bracket, so this
// does begin and end in one call and marks the record valid with no polygons in it.
void geometry_none();

// Emulation thread, from the screen_update hooks. Returns where to write width*height pixels, or
// nullptr if nobody is capturing. layer_end() is what marks it valid, so a capture abandoned
// half-way leaves the previous frame's layer in place rather than a torn one.
uint32_t *layer_begin(int which, int width, int height);
void      layer_end(int which);

// Frontend thread. The record as of the last completed screen_update, or nullptr if no frame has
// been captured yet.
frame_record const *frame_current();

// retro_load_game / retro_unload_game. enable() also clears, so a second game in the same process
// cannot present the first one's layers.
void frame_enable(bool on);
void frame_end_run();

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_FRAME_H
