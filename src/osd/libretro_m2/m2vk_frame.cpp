// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 — the frame record. See m2vk_frame.h for what crosses and why; this file is the
    storage.

    One record, not two. The baton in libretro_m2_osd.cpp already serialises the emulation thread
    against retro_run — the writer is parked for the whole of the read — so a second buffer would buy
    nothing but memory. If the run loop ever stops being lock-step, this is the file that has to
    change, and the assumption is stated here rather than left to be rediscovered.

    Nothing here allocates in a steady state, which is the property to preserve when editing it:

      * the layers are resized when the picture's geometry changes and never shrunk. 496x384x4 is
        762 KB per layer.
      * the polygon vector is grown to the frame's polygon count and never shrunk, so it settles at
        the run's high-water mark within a few frames. 1450 polys — the worst case observed — is
        about 350 KB.
      * the two table snapshots are fixed size and allocated on the first captured frame.

*********************************************************************************************************************************/

#include "m2vk_frame.h"

namespace m2vk {

namespace detail {

bool g_capturing = false;

} // namespace detail

namespace {

frame_record g_record;

// Fold the gamma table into the colour ramps and copy the luma translator, reporting whether either
// actually changed. The compare rides along with the copy rather than being a separate memcmp: both
// tables are read start to finish either way, and the alternative is touching 56 KB twice to save
// nothing.
//
// gamma[colorxlat[i] & 0xff] is exactly what the scanline renderers compute — every read of a ramp
// in model2rd.ipp is `colortable_x[luma] & 0xff` immediately followed by `gamma_value[...]` — so the
// fold is lossless and it turns two dependent lookups per colour component into one.
bool snapshot_tables(frame_tables const &tables)
{
	if ((tables.colorxlat == nullptr) || (tables.lumaram == nullptr) || (tables.gamma == nullptr))
		return false;

	// Not "did the contents change" but "is what we hold unusable": the first snapshot of a run has
	// to count as a change even in the vanishingly unlikely case that every baked byte is zero, or
	// the renderer would never upload it.
	bool changed = !g_record.tables_valid;

	if (g_record.colorxlat.size() != COLORXLAT_ENTRIES)
	{
		g_record.colorxlat.resize(COLORXLAT_ENTRIES);
		changed = true;
	}
	if (g_record.lumaram.size() != LUMARAM_ENTRIES)
	{
		g_record.lumaram.resize(LUMARAM_ENTRIES);
		changed = true;
	}

	uint8_t *const xlat = g_record.colorxlat.data();
	for (uint32_t i = 0; i < COLORXLAT_ENTRIES; i++)
	{
		uint8_t const baked = tables.gamma[tables.colorxlat[i] & 0xff];
		changed |= (xlat[i] != baked);
		xlat[i] = baked;
	}

	uint8_t *const luma = g_record.lumaram.data();
	for (uint32_t i = 0; i < LUMARAM_ENTRIES; i++)
	{
		changed |= (luma[i] != tables.lumaram[i]);
		luma[i] = tables.lumaram[i];
	}

	g_record.tables_valid = true;
	return changed;
}

} // anonymous namespace


//============================================================
//  the geometry
//============================================================

void geometry_begin(uint32_t submitted, frame_tables const &tables)
{
	if (!detail::g_capturing)
		return;

	// The storage is about to be overwritten in place, so the previous frame's polygons are gone
	// whether or not this frame completes. Saying so is more honest than leaving a half-written
	// stream marked good; in practice nothing between here and geometry_end can bail out.
	g_record.geometry_valid = false;
	g_record.poly_count = 0;
	g_record.submitted = submitted;

	// The geometry engine's own count is the right size to grow to: every polygon in the list is
	// rendered exactly once, so it is the frame's exact total and not merely a bound.
	if (g_record.polys.size() < submitted)
		g_record.polys.resize(submitted);

	if (snapshot_tables(tables))
		g_record.tables_serial++;

	// Not snapshotted — see frame_record. Re-read every frame anyway because the shares are found
	// when the machine starts and this is the only place that hears about it.
	for (int i = 0; i < 2; i++)
	{
		g_record.texram[i] = tables.texram[i];
		g_record.texram_words[i] = tables.texram_words[i];
	}
}

void geometry_submit(poly const &p)
{
	if (!detail::g_capturing)
		return;

	// Only reached if a frame renders more polygons than the geometry engine said it queued, which
	// has never been observed — the polytap counts both and reports any disagreement. Growing rather
	// than dropping means an unexpected frame is recorded correctly and the count still shows it.
	if (g_record.poly_count == g_record.polys.size())
		g_record.polys.resize(g_record.polys.size() + (g_record.polys.size() / 2) + 64);

	g_record.polys[g_record.poly_count++] = p;
}

void geometry_end()
{
	if (!detail::g_capturing)
		return;

	g_record.geometry_valid = true;
	g_record.geometry_serial++;
}

void geometry_none()
{
	if (!detail::g_capturing)
		return;

	// What geometry_begin and geometry_end would do between them for a frame with no polygons, minus
	// the two things that would only be waste. The tables are not snapshotted — 56 KB scanned on the
	// emulation thread to shade nothing, on 47 % of vstriker's frames — and the texture shares are not
	// refreshed; neither is read while poly_count is zero, and both are left holding whatever the last
	// frame with geometry put there.
	//
	// The serial is bumped, and that is the point of the whole call: it is what tells the renderer this
	// is a new frame that is empty rather than the same frame over again.
	g_record.poly_count = 0;
	g_record.submitted = 0;
	g_record.geometry_valid = true;
	g_record.geometry_serial++;
}


//============================================================
//  the 2D layers
//============================================================

uint32_t *layer_begin(int which, int width, int height)
{
	if (!detail::g_capturing)
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


//============================================================
//  the run
//============================================================

frame_record const *frame_current()
{
	return g_record.complete() ? &g_record : nullptr;
}

void frame_enable(bool on)
{
	detail::g_capturing = on;
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

	g_record.poly_count = 0;
	g_record.submitted = 0;
	g_record.geometry_valid = false;
	g_record.geometry_serial = 0;

	// Dropped rather than kept stale: these point into a machine that is going away, and the next
	// run's shares are somewhere else entirely.
	for (int i = 0; i < 2; i++)
	{
		g_record.texram[i] = nullptr;
		g_record.texram_words[i] = 0;
	}

	// The tables are marked stale rather than cleared, for the same reason: the next run's first
	// captured frame refills them, and it will report itself as a change because of this flag.
	g_record.tables_valid = false;
	g_record.tables_serial = 0;
}

} // namespace m2vk
