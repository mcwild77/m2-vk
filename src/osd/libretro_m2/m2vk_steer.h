// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 steering — paddle detector, MAME analog capture, read-out, shaping pipeline.
    Full design in devnotes/steering-curve.md.

    Include AFTER emu.h: names ioport types, which only emu.h brings in.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_STEER_H
#define MAME_OSD_LIBRETRO_M2_M2VK_STEER_H

#pragma once

#include "m2vk_steerbar.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

namespace m2vk {

namespace detail {

struct steer_field
{
	ioport_port  *port     = nullptr;
	ioport_value  mask     = 0;
	unsigned      shift    = 0;
	int32_t       minval   = 0;
	int32_t       maxval   = 0;
	int           player   = 0;
	bool          vertical = false;
	std::string   name;

	bool present() const { return port != nullptr; }
	int32_t read() const { return int32_t((port->read() & mask) >> shift); }
};

} // namespace detail

struct steer_state
{
	bool     active   = false;      // machine declares an IPT_PADDLE / IPT_PADDLE_V field
	bool     resolved = false;

	// MAME's joystick_deadzone/saturation, captured at input_init(). The pre-compensation inverts
	// these for this axis so the middle 70 % of stick travel is not the whole of a wheel's lock.
	float    deadzone   = 0.15f;
	float    saturation = 0.85f;

	// The three numbers the chain runs, resolved from options + switches by steer_apply().
	bool     shaping    = true;
	float    dz_opt     = 0.0f;
	float    gamma      = 1.0f;
	float    range_opt  = 1.0f;

	// Switches, so a live option change recomposes without re-reading the environment.
	bool     sw_linear  = false;   // M2VK_STEER_LINEAR named and non-zero → force pipeline off
	bool     sw_dz      = false;
	bool     sw_gamma   = false;
	bool     sw_range   = false;
	float    sw_dz_val    = 0.0f;
	float    sw_gamma_val = 1.0f;
	float    sw_range_val = 1.0f;

	// Written by the pad, read by the read-out. In MAME's ±65536 units.
	int32_t  raw    = 0;
	int32_t  shaped = 0;
	uint64_t polls  = 0;

	uint32_t period       = 0;
	bool     asked        = false;
	uint64_t frame        = 0;
	uint64_t resolved_at  = 0;
	uint64_t polls_at     = 0;
	unsigned count        = 0;
	detail::steer_field field[2];
};

inline steer_state &steer()
{
	static steer_state s;
	return s;
}

constexpr int32_t STEER_ABS_MAX = 65536;
static_assert(osd::input_device::ABSOLUTE_MAX == STEER_ABS_MAX, "MAME's absolute axis scale moved");

namespace detail {

inline bool steer_env(char const *name, float &out, float lo, float hi)
{
	char const *const text = std::getenv(name);
	if (text == nullptr)
		return false;
	float v = float(std::strtod(text, nullptr));
	if (v < lo) v = lo;
	if (v > hi) v = hi;
	out = v;
	return true;
}

// Options parked by set_option_steering() from the libretro thread. Live outside steer_state so
// they survive steer_close(). Initialised to the identity so a missed set_option_steering() call
// produces a no-op shape rather than a mystery one.
inline float g_opt_dz    = 0.0f;
inline float g_opt_gamma = 1.0f;
inline float g_opt_range = 1.0f;

} // namespace detail

// Resolve the three numbers the chain runs. Called from steer_config() at machine start and from
// set_option_steering() on live changes, so both mean the same thing.
inline void steer_apply()
{
	steer_state &s = steer();
	s.dz_opt    = s.sw_dz    ? s.sw_dz_val    : detail::g_opt_dz;
	s.gamma     = s.sw_gamma ? s.sw_gamma_val : detail::g_opt_gamma;
	s.range_opt = s.sw_range ? s.sw_range_val : detail::g_opt_range;
	s.shaping   = !s.sw_linear;
}

inline void set_option_steering(float deadzone, float gamma, float range)
{
	detail::g_opt_dz    = std::clamp(deadzone, 0.0f, 0.5f);
	detail::g_opt_gamma = std::clamp(gamma,    1.0f, 4.0f);
	detail::g_opt_range = std::clamp(range,    0.5f, 1.0f);
	steer_apply();
}

// Read the M2VK_STEER_* switches. Called once per machine from input_init().
inline void steer_config()
{
	steer_state &s = steer();

	// M2VK_STEER_LINEAR takes a value, not a presence: =1 forces linear, =0 leaves shaping alone
	// (the M2VK_BLEND discipline). A harness run must be able to pin shaping on as well as off.
	char const *const linear = std::getenv("M2VK_STEER_LINEAR");
	s.sw_linear = (linear != nullptr) && (std::strtol(linear, nullptr, 10) != 0);

	s.sw_dz    = detail::steer_env("M2VK_STEER_DEADZONE", s.sw_dz_val,    0.0f, 0.5f);
	s.sw_gamma = detail::steer_env("M2VK_STEER_GAMMA",    s.sw_gamma_val, 1.0f, 4.0f);
	s.sw_range = detail::steer_env("M2VK_STEER_RANGE",    s.sw_range_val, 0.5f, 1.0f);

	steer_apply();
}

// Shape the primary stick X. In/out in ±STEER_ABS_MAX. X only, never Y — desert's brake is
// IPT_AD_STICK_Y on the same stick, so a curve on Y would bend a pedal.
//
// The centre-returns-zero path (mag <= dz_opt) is the invariant every ab.sh baseline rests on:
// no accuracy fixture scripts an analog axis, so a centred stick keeps the pre-existing digest.
inline int32_t steer_shape(int32_t raw)
{
	const steer_state &s = steer();
	if (!s.shaping || !s.active)
		return raw;

	// MAME's thresholds as integers, exactly the way input_device_joystick's constructor computes
	// them (inputdev.cpp:449-451), so the inversion below matches what will actually be applied.
	const int32_t dz  = int32_t(double(s.deadzone) * double(STEER_ABS_MAX));
	const int32_t sat = int32_t(double(s.saturation) * double(STEER_ABS_MAX));
	const int32_t rng = sat - dz;
	if (rng <= 0)
		return raw;

	double u = std::clamp(double(raw) / double(STEER_ABS_MAX), -1.0, 1.0);
	const double mag = std::fabs(u);

	if (mag <= double(s.dz_opt))
		return 0;
	double a = (mag - double(s.dz_opt)) / (1.0 - double(s.dz_opt));

	if (s.gamma != 1.0f)
		a = std::pow(a, double(s.gamma));
	a = std::min(a * double(s.range_opt), 1.0);

	// Pre-compensation: MAME computes floor((|r| - dz) * ABS_MAX / rng), so emitting dz + ceil(a *
	// rng) makes that the identity.
	const int64_t want = int64_t(a * double(STEER_ABS_MAX) + 0.5);
	int32_t out = dz + int32_t((want * rng + (STEER_ABS_MAX - 1)) / STEER_ABS_MAX);
	if (out > STEER_ABS_MAX)
		out = STEER_ABS_MAX;
	return (u < 0.0) ? -out : out;
}

namespace detail {

inline void steer_resolve(running_machine &machine, uint64_t frame)
{
	steer_state &s = steer();
	s.resolved    = true;
	s.resolved_at = frame;
	s.polls_at    = s.polls;

	char const *const period = std::getenv("M2VK_STEER_LOG");
	s.asked  = (period != nullptr);
	s.period = s.asked ? uint32_t(std::strtoul(period, nullptr, 10)) : 0;

	for (auto &port : machine.ioport().ports())
	{
		for (ioport_field const &field : port.second->fields())
		{
			const bool vertical = (field.type() == IPT_PADDLE_V);
			if ((field.type() != IPT_PADDLE) && !vertical)
				continue;

			s.active = true;
			if (s.count < std::size(s.field))
			{
				steer_field &f = s.field[s.count];
				f.port     = port.second.get();
				f.mask     = field.mask();
				f.shift    = 0;
				while (((f.mask >> f.shift) & 1) == 0)
					f.shift++;
				f.minval   = int32_t(field.minval());
				f.maxval   = int32_t(field.maxval());
				f.player   = field.player();
				f.vertical = vertical;
				f.name     = field.name();
			}
			s.count++;
		}
	}

	if (s.sw_linear || s.sw_dz || s.sw_gamma || s.sw_range)
	{
		std::fprintf(stderr, "[steer] M2VK_STEER_* is set; it overrides the matching core option. Shaping is %s%s\n",
				s.shaping ? "ON" : "OFF (M2VK_STEER_LINEAR)",
				(s.shaping && !s.active) ? ", but this machine has no paddle, so nothing is shaped" : "");
	}

	if (!s.asked)
		return;

	std::fprintf(stderr, "[steer] %s: %u IPT_PADDLE/IPT_PADDLE_V field(s) -> steering shaping %s apply here\n",
			machine.system().name, s.count, s.active ? "WOULD" : "would NOT");

	for (unsigned n = 0; n < s.count && n < std::size(s.field); n++)
	{
		const steer_field &f = s.field[n];
		std::fprintf(stderr, "[steer]   p%d \"%s\" %s range 0x%03x..0x%03x centre 0x%03x\n",
				f.player + 1, f.name.c_str(),
				f.vertical ? "IPT_PADDLE_V" : "IPT_PADDLE",
				f.minval, f.maxval, (f.minval + f.maxval) / 2);
	}

	std::fprintf(stderr, "[steer] resolved on frame %llu after %llu frontend poll(s)%s\n",
			(unsigned long long)s.resolved_at, (unsigned long long)s.polls_at,
			(s.polls_at == 0) ? " — flag is set before the first sample" : " ⚠ SAMPLES TAKEN UNSHAPED");
	std::fprintf(stderr, "[steer] MAME joystick_deadzone=%.3f joystick_saturation=%.3f (the pipeline inverts these)\n",
			double(s.deadzone), double(s.saturation));

	if (!s.shaping)
	{
		std::fprintf(stderr, "[steer] pipeline OFF — M2VK_STEER_LINEAR; the axis reaches MAME unshaped\n");
	}
	else
	{
		std::fprintf(stderr, "[steer] pipeline ON: deadzone=%.3f (%s) gamma=%.2f (%s) range=%.3f (%s) + pre-compensation\n",
				double(s.dz_opt),    s.sw_dz    ? "switch" : "core option",
				double(s.gamma),     s.sw_gamma ? "switch" : "core option",
				double(s.range_opt), s.sw_range ? "switch" : "core option");
	}

	if (s.period != 0)
		std::fprintf(stderr, "[steer] read-out active, every %u frame(s)\n", s.period);
}

} // namespace detail

// Called once per emulated frame from the OSD's update(). Waits for safe_to_read() because
// osd().init() runs before ioport_manager::initialize() — same trap m2vk_inputdump.h documents.
inline void steer_frame(running_machine &machine)
{
	steer_state &s = steer();
	const uint64_t frame = s.frame++;

	if (!machine.ioport().safe_to_read())
		return;
	if (!s.resolved)
		detail::steer_resolve(machine, frame);

	// The port is read ONLY when the bar is on, so a default run's port traffic is unchanged.
	if (steerbar_on() && s.active && s.field[0].present())
	{
		const detail::steer_field &f = s.field[0];
		const float centre = 0.5f * float(f.minval + f.maxval);
		const float half   = 0.5f * float(f.maxval - f.minval);
		float value = (half > 0.0f) ? std::clamp((float(f.read()) - centre) / half, -1.0f, 1.0f) : 0.0f;
		float raw   = std::clamp(float(s.raw) / float(STEER_ABS_MAX), -1.0f, 1.0f);

		// A vertical paddle steers nothing a horizontal bar can describe.
		steerbar_publish(true, f.vertical ? 0.0f : value, f.vertical ? 0.0f : raw);
	}
	else
	{
		steerbar_publish(false, 0.0f, 0.0f);
	}

	if ((s.period == 0) || ((frame % s.period) != 0))
		return;

	char line[256];
	int at = std::snprintf(line, sizeof(line),
			"[steer] f=%llu  raw=%+.4f (%+6d)  shaped=%+.4f (%+6d)%s",
			(unsigned long long)frame,
			double(s.raw) / 65536.0, s.raw,
			double(s.shaped) / 65536.0, s.shaped,
			(s.raw == s.shaped) ? "" : " *");

	for (unsigned n = 0; n < s.count && n < std::size(s.field); n++)
	{
		const detail::steer_field &f = s.field[n];
		if (!f.present())
			continue;
		at += std::snprintf(line + at, sizeof(line) - at, "  p%d=0x%03x", f.player + 1, f.read());
	}

	std::fprintf(stderr, "%s\n", line);
}

// Drop the located fields when the machine goes away; a second retro_load_game builds new ports.
inline void steer_close()
{
	steer() = steer_state();
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_STEER_H
