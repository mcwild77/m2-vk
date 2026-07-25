// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 — the frame record. See m2vk_frame.h for what it is and why the two layers are
    separate; this file is the storage.

    One record, not two. The baton in libretro_m2_osd.cpp already serialises the emulation thread
    against retro_run — the writer is parked for the whole of the read — so a second buffer would buy
    nothing but memory. If the run loop ever stops being lock-step, this is the file that has to
    change, and the assumption is stated here rather than left to be rediscovered.

    The vectors are resized when the geometry changes and never shrunk, so a steady state does no
    allocation at all. 496x384x4 is 762 KB per layer.

*********************************************************************************************************************************/

#include "m2vk_frame.h"

namespace m2vk {

namespace detail {

bool g_want_layers = false;

} // namespace detail

namespace {

frame_record g_record;

} // anonymous namespace


uint32_t *layer_begin(int which, int width, int height)
{
	if (!detail::g_want_layers)
		return nullptr;
	if ((which < 0) || (which >= LAYER_COUNT) || (width <= 0) || (height <= 0))
		return nullptr;

	frame_layer &l = g_record.layer[which];

	if ((l.width != width) || (l.height != height))
	{
		l.width = width;
		l.height = height;
		l.valid = false;    // whatever was in there described a different picture
		l.pixels.resize(size_t(width) * size_t(height));
	}

	return l.pixels.data();
}

void layer_end(int which)
{
	if ((which < 0) || (which >= LAYER_COUNT))
		return;

	g_record.layer[which].valid = true;

	// The over layer is the last thing screen_update() produces, so it is what closes the frame.
	if (which == LAYER_OVER)
		g_record.serial++;
}

frame_record const *frame_current()
{
	return g_record.complete() ? &g_record : nullptr;
}

void frame_enable(bool on)
{
	detail::g_want_layers = on;
	frame_end_run();
}

void frame_end_run()
{
	for (frame_layer &l : g_record.layer)
	{
		l.valid = false;
		l.width = 0;
		l.height = 0;
		// The storage is kept: a run that follows another one is almost always the same geometry, and
		// this is not a path where holding 1.5 MB matters.
	}
	g_record.serial = 0;
}

} // namespace m2vk
