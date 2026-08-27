// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 / System 21/22 analog-stick shaping — deadzone + reach for the IPT_AD_STICK games.

    A sibling of m2vk_steer.h with the wheel-specific machinery removed: no gamma, no damping, no
    read-out bar. The one shared idea is the pre-compensation — MAME maps every absolute axis through
    a 15 % deadzone / 85 % saturation curve, so the middle 70 % of stick travel is dead-then-full, and
    this inverts that curve for the stick axes exactly as steer_shape() does for the wheel. The knobs
    are OUR deadzone (default 5 %) and a "reach" (the deflection at which full output is reached,
    default 100 %), replacing the wheel's range/gamma.

    Detector: the machine declaring an IPT_AD_STICK_X/Y/Z field — Star Blade, the twin-stick and the
    flight sets — mutually exclusive with the wheel games (which declare IPT_PADDLE), so a driving
    game is untouched by construction and never shows these options.

    The transform is applied per axis, independently, exactly the way MAME's deadzone/saturation is,
    so a diagonal (raw_x == raw_y) stays a true 45°: f(raw_x) == f(raw_y). Full design in
    devnotes/analog-deadzone-reach-plan.md.

    Include AFTER emu.h: names ioport types, which only emu.h brings in.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_ANALOG_H
#define MAME_OSD_LIBRETRO_M2_M2VK_ANALOG_H

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace m2vk {

struct analog_state
{
	bool     active   = false;      // machine declares an IPT_AD_STICK_X/Y/Z field
	bool     resolved = false;

	// MAME's joystick_deadzone/saturation, captured at input_init(). The pre-compensation inverts these
	// so the middle 70 % of a stick's travel is not dead-then-full. Same numbers steer_state captures.
	float    deadzone   = 0.15f;
	float    saturation = 0.85f;

	// The two numbers the chain runs, resolved from options + switches by analog_apply().
	bool     shaping   = true;
	float    dz_opt    = 0.05f;     // our deadzone
	float    reach_opt = 1.0f;      // deflection at which full output is reached

	// Switches, so a live option change recomposes without re-reading the environment.
	bool     sw_linear = false;     // M2VK_ANALOG_LINEAR named and non-zero → force pipeline off
	bool     sw_dz     = false;
	bool     sw_reach  = false;
	float    sw_dz_val    = 0.0f;
	float    sw_reach_val = 1.0f;

	uint64_t frame       = 0;
	uint64_t resolved_at = 0;
};

inline analog_state &analog()
{
	static analog_state s;
	return s;
}

constexpr int32_t ANALOG_ABS_MAX = 65536;
static_assert(osd::input_device::ABSOLUTE_MAX == ANALOG_ABS_MAX, "MAME's absolute axis scale moved");

namespace detail {

inline bool analog_env(char const *name, float &out, float lo, float hi)
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

// Options parked by set_option_analog() from the libretro thread. Live outside analog_state so they
// survive analog_close(). Initialised to the shipping defaults so a missed setter still shapes sanely.
// Named distinctly from steer's g_opt_* — both live in m2vk::detail.
inline float g_analog_dz    = 0.05f;
inline float g_analog_reach = 1.0f;

} // namespace detail

// Resolve the two numbers the chain runs. Called from analog_config() at machine start and from
// set_option_analog() on live changes, so both mean the same thing.
inline void analog_apply()
{
	analog_state &s = analog();
	s.dz_opt    = s.sw_dz    ? s.sw_dz_val    : detail::g_analog_dz;
	s.reach_opt = s.sw_reach ? s.sw_reach_val : detail::g_analog_reach;
	s.shaping   = !s.sw_linear;
}

inline void set_option_analog(float deadzone, float reach)
{
	detail::g_analog_dz    = std::clamp(deadzone, 0.0f, 0.5f);
	detail::g_analog_reach = std::clamp(reach,    0.5f, 1.0f);
	analog_apply();
}

// Read the M2VK_ANALOG_* switches. Called once per machine from input_init().
inline void analog_config()
{
	analog_state &s = analog();

	// M2VK_ANALOG_LINEAR takes a value, not a presence: =1 forces linear, =0 leaves shaping alone
	// (the M2VK_STEER_LINEAR discipline). A harness run must be able to pin shaping on as well as off.
	char const *const linear = std::getenv("M2VK_ANALOG_LINEAR");
	s.sw_linear = (linear != nullptr) && (std::strtol(linear, nullptr, 10) != 0);

	// Fractions (0.05 = 5 %), matching M2VK_STEER_DEADZONE/RANGE. Presence overrides the option.
	s.sw_dz    = detail::analog_env("M2VK_ANALOG_DEADZONE", s.sw_dz_val,    0.0f, 0.5f);
	s.sw_reach = detail::analog_env("M2VK_ANALOG_REACH",    s.sw_reach_val, 0.5f, 1.0f);

	analog_apply();
}

// Shape one stick axis. In/out in ±ANALOG_ABS_MAX. Applied to each axis independently — the same
// scalar transform on X and Y, which is what keeps a diagonal (raw_x == raw_y) at a true 45°.
//
// The centre-returns-zero path (mag <= dz_opt) is the invariant every ab.sh baseline rests on: no
// accuracy fixture scripts an analog axis, so a centred stick keeps the pre-existing digest.
inline int32_t analog_shape(int32_t raw)
{
	const analog_state &s = analog();
	if (!s.shaping || !s.active)
		return raw;

	// MAME's thresholds as integers, exactly the way input_device_joystick's constructor computes them
	// (inputdev.cpp:449-451), so the inversion below matches what will actually be applied.
	const int32_t dz  = int32_t(double(s.deadzone) * double(ANALOG_ABS_MAX));
	const int32_t sat = int32_t(double(s.saturation) * double(ANALOG_ABS_MAX));
	const int32_t rng = sat - dz;
	if (rng <= 0)
		return raw;

	const double u   = std::clamp(double(raw) / double(ANALOG_ABS_MAX), -1.0, 1.0);
	const double mag = std::fabs(u);
	const double dzo = double(s.dz_opt);
	const double rch = double(s.reach_opt);

	if (mag <= dzo)
		return 0;
	if (rch <= dzo)          // degenerate reach ≤ deadzone: leave the axis unshaped rather than divide by ~0
		return raw;

	const double a = std::clamp((mag - dzo) / (rch - dzo), 0.0, 1.0);

	// Pre-compensation: MAME computes floor((|r| - dz) * ABS_MAX / rng), so emitting dz + ceil(a * rng)
	// makes that the identity — the same inversion steer_shape() applies.
	const int64_t want = int64_t(a * double(ANALOG_ABS_MAX) + 0.5);
	int32_t out = dz + int32_t((want * rng + (ANALOG_ABS_MAX - 1)) / ANALOG_ABS_MAX);
	if (out > ANALOG_ABS_MAX)
		out = ANALOG_ABS_MAX;
	return (u < 0.0) ? -out : out;
}

namespace detail {

inline void analog_resolve(running_machine &machine, uint64_t frame)
{
	analog_state &s = analog();
	s.resolved    = true;
	s.resolved_at = frame;

	for (auto &port : machine.ioport().ports())
	{
		for (ioport_field const &field : port.second->fields())
		{
			const auto t = field.type();
			if ((t == IPT_AD_STICK_X) || (t == IPT_AD_STICK_Y) || (t == IPT_AD_STICK_Z))
				s.active = true;
		}
	}

	if (s.sw_linear || s.sw_dz || s.sw_reach)
		std::fprintf(stderr, "[analog] M2VK_ANALOG_* is set; it overrides the matching core option. Shaping is %s%s\n",
				s.shaping ? "ON" : "OFF (M2VK_ANALOG_LINEAR)",
				(s.shaping && !s.active) ? ", but this machine has no analog stick, so nothing is shaped" : "");
}

} // namespace detail

// Called once per emulated frame from the OSD's update(). Waits for safe_to_read() because osd().init()
// runs before ioport_manager::initialize() — the same trap m2vk_steer.h documents.
inline void analog_frame(running_machine &machine)
{
	analog_state &s = analog();
	const uint64_t frame = s.frame++;

	if (!machine.ioport().safe_to_read())
		return;
	if (!s.resolved)
		detail::analog_resolve(machine, frame);
}

// Drop the detector when the machine goes away; a second retro_load_game builds new ports.
inline void analog_close()
{
	analog() = analog_state();
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_ANALOG_H
