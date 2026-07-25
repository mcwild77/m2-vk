// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 — the frame record: what the emulation thread hands the renderer.

    A Model 2 frame is a sandwich, and only the middle of it is ours. model2_state::screen_update()
    builds the picture in three passes (src/mame/sega/model2_v.cpp):

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

    A frame that renders nothing (skip_redraw) leaves the record alone, so the renderer re-presents
    the layers it already has. That matches what MAME does with its own bitmap.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_FRAME_H
#define MAME_OSD_LIBRETRO_M2_M2VK_FRAME_H

#pragma once

#include <cstdint>
#include <vector>

namespace m2vk {

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

	// Bumped every time a complete pair of layers lands. The renderer does not need it to be correct
	// — it presents whatever is current — but it is the cheap way to tell "the emulator produced
	// nothing this frame" from "the capture is broken", which look identical in a picture.
	uint64_t serial = 0;

	bool complete() const { return layer[LAYER_UNDER].valid && layer[LAYER_OVER].valid; }
};

// Layer capture is off unless the Vulkan path asked for it: in software mode the two hooks in
// screen_update() must cost a predicate and nothing else, or renderer=software stops being the
// reference it exists to be.
namespace detail {

extern bool g_want_layers;

} // namespace detail

inline bool want_layers() { return detail::g_want_layers; }

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
