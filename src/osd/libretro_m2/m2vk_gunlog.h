// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 lightgun read-out — diagnostic instrumentation for the lightgun work.

    M2VK_GUN_LOG=<n> prints the resolved IPT_LIGHTGUN_X/Y port values every n emulated frames. It
    exists because there is nothing else to look at: MAME's crosshair is a render-container quad
    (crsshair.cpp:444) and this OSD reads pixels straight off screen->curbitmap(), so no crosshair is
    ever drawn here and a scripted aim cannot be checked by eye. See devnotes/lightgun.md §1.5.

    What it prints is the number the *driver* will read — the value after MAME's whole input chain
    (device item -> assignment -> analog_field::apply_settings -> PORT_MINMAX scaling), not what the
    frontend was told. That is the point: the axis mapping is only correct if full-scale OSD input
    lands exactly on the ends of PORT_MINMAX, and this is the only place that can be seen.

    The offscreen column is model2_state::lightgun_offscreen_r (model2.cpp:1136) transliterated —
    same 5 % border, same integer truncation, same inclusive comparisons — so a scripted reload can
    be checked against a bit rather than against the game's behaviour. It is a copy of the driver's
    test and not a tap of it: tapping would mean touching an upstream file, and the size of the diff
    against upstream is a budget this fork spends carefully.

    Two things this deliberately does not do. It does not read the trigger, because a trigger is an
    ordinary button and the existing input path already proves those. And it does not print when
    nothing changed, because a sweep is exactly the case where every frame matters.

    ⚠ Include this AFTER emu.h. It is header-only so that it needs no entry in the two build scripts,
    and it names ioport types, which only emu.h brings in — and only a .cpp may include that.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_GUNLOG_H
#define MAME_OSD_LIBRETRO_M2_M2VK_GUNLOG_H

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace m2vk {

namespace detail {

// One IPT_LIGHTGUN_X or _Y field, located once and then read every frame.
struct gun_axis
{
	ioport_port  *port   = nullptr;
	ioport_value  mask   = 0;
	unsigned      shift  = 0;
	int32_t       minval = 0;
	int32_t       maxval = 0;

	bool present() const { return port != nullptr; }

	int32_t read() const { return int32_t((port->read() & mask) >> shift); }

	// The border test from lightgun_offscreen_r, kept identical down to the truncation: the driver
	// assigns a float product to an int, so a range of 0x1f3 gives a border of 24 and not 24.95.
	bool offscreen(int32_t value) const
	{
		const int border = int((maxval - minval) * 0.05f);
		return (value <= (minval + border)) || (value >= (maxval - border));
	}
};

struct gun_state
{
	bool     resolved = false;
	uint32_t period   = 0;      // 0 = off; also the "not asked for" state, so nothing is located
	uint64_t frame    = 0;
	bool     any      = false;
	gun_axis axis[2][2];        // [player][0 = X, 1 = Y]
};

inline gun_state &gun_log_state()
{
	static gun_state s;
	return s;
}

// Locates the four fields and prints the reference line a sweep is checked against: the range each
// axis is scaled into, and the two trip points where the driver starts reporting offscreen.
inline void gun_log_resolve(running_machine &machine)
{
	gun_state &s = gun_log_state();
	s.resolved = true;

	char const *const period = std::getenv("M2VK_GUN_LOG");
	s.period = (period != nullptr) ? uint32_t(std::strtoul(period, nullptr, 10)) : 0;
	if (s.period == 0)
		return;

	for (auto &port : machine.ioport().ports())
	{
		for (ioport_field const &field : port.second->fields())
		{
			const unsigned which = (field.type() == IPT_LIGHTGUN_X) ? 0 : (field.type() == IPT_LIGHTGUN_Y) ? 1 : 2;
			if ((which > 1) || (field.player() > 1))
				continue;

			gun_axis &axis = s.axis[field.player()][which];
			axis.port   = port.second.get();
			axis.mask   = field.mask();
			axis.shift  = 0;
			while (((axis.mask >> axis.shift) & 1) == 0)
				axis.shift++;
			axis.minval = int32_t(field.minval());
			axis.maxval = int32_t(field.maxval());
			s.any = true;
		}
	}

	if (!s.any)
	{
		std::fprintf(stderr, "[gun] this set has no IPT_LIGHTGUN_X/Y ports; nothing to report\n");
		return;
	}

	std::fprintf(stderr, "[gun] active, every %u frame(s)\n", s.period);
	for (unsigned player = 0; player < 2; player++)
	{
		for (unsigned which = 0; which < 2; which++)
		{
			const gun_axis &axis = s.axis[player][which];
			if (!axis.present())
				continue;

			const int border = int((axis.maxval - axis.minval) * 0.05f);
			std::fprintf(stderr, "[gun] p%u %c range 0x%03x..0x%03x  offscreen at <=0x%03x or >=0x%03x\n",
					player + 1, which ? 'y' : 'x',
					axis.minval, axis.maxval, axis.minval + border, axis.maxval - border);
		}
	}
}

} // namespace detail

// Called once per emulated frame, on the emulation thread, from the OSD's update(). The frame
// number counts frames handed to the frontend, so it is the same index a retrohost control script
// and the M2VK_HOST_* triggers use.
inline void gun_log_frame(running_machine &machine)
{
	detail::gun_state &s = detail::gun_log_state();

	// Counted before anything can bail out, so the number stays the frontend's frame index whatever
	// the state of the machine.
	const uint64_t frame = s.frame++;

	// Resolution waits for safe_to_read(). The first update() lands before the port list is
	// complete — asking then finds no lightgun fields on a set that has four, which is exactly the
	// kind of "the instrument says no" that would have been believed.
	if (!machine.ioport().safe_to_read())
		return;
	if (!s.resolved)
		detail::gun_log_resolve(machine);
	if ((s.period == 0) || !s.any || ((frame % s.period) != 0))
		return;

	char line[256];
	int at = std::snprintf(line, sizeof(line), "[gun] f=%llu", (unsigned long long)frame);
	for (unsigned player = 0; player < 2; player++)
	{
		const detail::gun_axis &x = s.axis[player][0];
		const detail::gun_axis &y = s.axis[player][1];
		if (!x.present() && !y.present())
			continue;

		const int32_t xv = x.present() ? x.read() : 0;
		const int32_t yv = y.present() ? y.read() : 0;

		// The driver ORs the two axes into one bit per player, so this does too — a reload asserted
		// on X alone would otherwise read as offscreen here and not in the game.
		const bool off = (x.present() && x.offscreen(xv)) || (y.present() && y.offscreen(yv));

		at += std::snprintf(line + at, sizeof(line) - at, "  p%u x=0x%03x y=0x%03x off=%d",
				player + 1, xv, yv, off ? 1 : 0);
	}
	std::fprintf(stderr, "%s\n", line);
}

// The located fields belong to the machine that is going away; a second retro_load_game builds new
// ports and the pointers above would be dangling. Resolving again also re-reads the environment,
// which costs nothing and keeps the two loads independent.
inline void gun_log_close()
{
	detail::gun_log_state() = detail::gun_state();
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_GUNLOG_H
