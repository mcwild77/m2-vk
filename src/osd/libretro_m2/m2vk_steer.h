// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 steering — the detector, the captured MAME analog settings, the read-out, and the
    shaping pipeline.

    This is steps 1 to 3 of devnotes/steering-curve.md. The pipeline is ON BY DEFAULT as of step 3:
    model2_steering_deadzone, model2_steering_response and model2_steering_range supply its three
    parameters, the M2VK_STEER_* switches override them one for one, and M2VK_STEER_LINEAR=1 turns the
    whole chain back into the identity — which is what a harness run pins itself with, because a
    remembered .opt file must not be able to rewrite a baseline.

    ⚠️ At step 2 "no switch set" meant "no shaping at all", and that is what made that step a no-op by
    construction. It does not any more: a default run of a steering game IS shaped. What keeps every
    ab.sh baseline byte-exact is narrower and worth knowing before changing the chain — a centred
    stick is the one input the pipeline returns unchanged at every setting (see the deadzone step
    below), and no accuracy fixture scripts an analog axis.

    Five things live here.

    1. THE DETECTOR (§3.2). An unconditional curve on left-stick X is wrong — the same axis feeds
       IPT_JOYSTICK_* on the fighters, IPT_AD_STICK_X on von, and IPT_LIGHTGUN_X on the gun sets,
       whose linearity is a deliberate measured property (devnotes/lightgun.md §1.1). So the curve
       applies iff the machine actually steers, and that is asked of the machine rather than
       authored per game: any field whose type() is IPT_PADDLE or IPT_PADDLE_V. Correct by
       construction for all 90 GAME entries, including the ~68 on the generic layout row, with
       nothing to keep in sync.

       🚨 It CANNOT be done in input_init(). osd().init() runs at machine.cpp:156 and
       ioport_manager::initialize() at 169, so at that point the port list is EMPTY and a detector
       there reports "no paddle" on a game that has one. Same trap m2vk_inputdump.h:125 documents.
       It is resolved from the OSD's update() instead, behind safe_to_read(), which is where
       gun_log_frame and input_dump_frame already resolve.

       The flag DEFAULTS TO OFF, so if the ordering is ever wrong the failure is one unshaped frame
       rather than a crash. Step 1 measures that ordering rather than assuming it — see polls below.

    2. MAME'S DEADZONE AND SATURATION, captured once in input_init() where the machine's options are
       in hand. The pad registers as DEVICE_CLASS_JOYSTICK, so input_device_joystick::adjust_absolute_value
       (inputdev.cpp:475) applies joystick_deadzone (0.15) and joystick_saturation (0.85) to
       everything we emit and this OSD overrides neither. Step 2's pre-compensation (§3.3) inverts
       exactly that transform for the steering axis alone, which is why the two numbers are read from
       the options rather than hard-coded: a user who HAS overridden them still gets a correct
       inversion.

    3. THE READ-OUT, M2VK_STEER_LOG. Modelled on m2vk_gunlog.h and for the same reason — the thing
       that has to be checked is deflection in -> port value out, and the port value is the only end
       of that which nothing else can show. What it prints is the number the DRIVER will read, after
       MAME's whole chain (device item -> assignment -> analog_field::apply_settings -> PORT_MINMAX
       scaling), next to what the frontend gave us.

         unset   silent, and the detector still runs
         =0      the one-shot resolve report only
         =n      the resolve report plus a line every n frames

    4. THE PIPELINE, steer_shape(), §3.4: deadzone -> curve -> range -> pre-compensation, applied to
       the primary stick X and to nothing else. X only, never Y — desert's brake is IPT_AD_STICK_Y on
       the same stick (model2.cpp:1812) and a curve on Y would bend a pedal.

       🚨 The pre-compensation (§3.3) is the half that is easy to leave out and impossible to notice
       missing by eye. MAME applies joystick_deadzone/joystick_saturation to everything we emit, so a
       curve alone would sit on top of a second deadzone with 15 % of the travel still thrown away at
       each end. Rather than move those options globally — which would reach the gun, von's twin
       stick and every digital-from-analog fighter — step 4 of the chain emits the value that makes
       input_device_joystick::adjust_absolute_value the IDENTITY for this one axis. The signature of
       it working is in the sweep: today the last 15 % of thumb travel is a flat 0xe0 plateau on
       daytona's STEER, and with the pipeline on at gamma=1/dz=0/range=1 the plateau is gone and the
       line is straight from 0x20 to 0xe0.

       Shaping the primary stick X is safe on exactly the machines the detector fires on, and that is
       a property of the driver rather than luck: add_directional_assignments binds that axis to
       IPT_PADDLE, IPT_AD_STICK_X and IPT_LIGHTGUN_X at once, but no paddle-bearing port set in
       model2.cpp declares an IPT_AD_STICK_X (desert's is _Y), and no gun set declares a paddle. On a
       RETRO_DEVICE_LIGHTGUN port the axis has already been zeroed by update()'s gate before it gets
       here, and shaping zero is zero.

    5. THE COMPOSITION OF OPTIONS AND SWITCHES, steer_apply(). The three core options are parked here
       by set_option_steering() from the libretro thread — at load and again whenever the player
       changes one, because a steering feel that needs a content reload to try is unusable; the whole
       point is nudging it between laps. Each switch that is present wins over its option, in BOTH
       directions, which is the standing rule the harness rests on (M2VK_STEER_GAMMA=1 pins a straight
       line against an option asking for a curve, exactly as M2VK_FLAT_LUMA=0 pins the lighting on).

    ⚠️ The default is deliberately NOT linear, and that is the one decision in this file that is a
    judgement rather than a measurement (§3.5). Every rendering option in this core defaults to the
    accurate path and the reflex is to do the same here; it does not transfer, because there is no
    accuracy ground truth for a control that does not exist on the target device. A real cabinet's
    wheel is 270° of travel, and linear-onto-a-thumbstick is not faithful to it — merely unshaped.
    Shipping Linear by default would be shipping the defect and asking the player to find the fix.

    ⚠ Include this AFTER emu.h. It is header-only so that it needs no entry in the two build
    scripts, and it names ioport types, which only emu.h brings in — and only a .cpp may include
    that.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_STEER_H
#define MAME_OSD_LIBRETRO_M2_M2VK_STEER_H

#pragma once

#include "m2vk_steerbar.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

namespace m2vk {

namespace detail {

// One IPT_PADDLE / IPT_PADDLE_V field, located once and then read every frame. The read is the
// driver's own value: mask, shift and PORT_MINMAX are the field's, so 0x20..0xe0 on daytona comes
// out as 0x20..0xe0 here.
struct steer_field
{
	ioport_port  *port     = nullptr;
	ioport_value  mask     = 0;
	unsigned      shift    = 0;
	int32_t       minval   = 0;
	int32_t       maxval   = 0;
	int           player   = 0;
	bool          vertical = false;     // IPT_PADDLE_V
	// ioport_field::name() composes a string (the driver's PORT_NAME, or the type's own name where
	// there is none), so it is kept rather than pointed at
	std::string   name;

	bool present() const { return port != nullptr; }

	int32_t read() const { return int32_t((port->read() & mask) >> shift); }
};

} // namespace detail

// Everything the steering work shares between the two threads. The emulation thread writes the
// detector's findings and reads the pad's samples; the libretro thread does the reverse. No locking,
// for the reason every other snapshot here relies on: poll_frontend() runs while the emulation
// thread is parked on the frame baton, so the two are never live at once.
struct steer_state
{
	// --- written by the detector, read by the pad (step 2) ---
	bool     active   = false;      // this machine has an IPT_PADDLE / IPT_PADDLE_V field
	bool     resolved = false;

	// --- written by input_init(), read by the pre-compensation ---
	float    deadzone   = 0.15f;    // MAME's joystick_deadzone, i.e. what it will apply to us
	float    saturation = 0.85f;    // ... and joystick_saturation

	// --- written by steer_apply(), read by the shaping ---
	// The three numbers the chain actually runs, each already resolved from its core option and its
	// switch, plus the master gate. shaping is ON unless M2VK_STEER_LINEAR=1 says otherwise: as of
	// step 3 the options supply the shape and there is nothing left for a run to opt into.
	bool     shaping    = true;
	float    dz_opt     = 0.0f;     // our deadzone, as a fraction of stick travel
	float    gamma      = 1.0f;     // >1 = fine near centre, coarse near lock
	float    range_opt  = 1.0f;     // cap on maximum lock

	// --- written by steer_config(), read by steer_apply() ---
	// Which switches the run named and what they said. Kept apart from the three resolved values above
	// so that a live option change can be recomposed against them without re-reading the environment,
	// and so that the read-out can say which source each number came from.
	bool     sw_linear  = false;    // M2VK_STEER_LINEAR was named at all...
	bool     sw_force_linear = false;   // ...and its value was non-zero
	bool     sw_dz      = false;
	bool     sw_gamma   = false;
	bool     sw_range   = false;
	float    sw_dz_val    = 0.0f;
	float    sw_gamma_val = 1.0f;
	float    sw_range_val = 1.0f;

	// --- written by the pad every frame, read by the read-out ---
	// Primary stick X on port 0, in MAME's +-65536 absolute units: what the frontend gave us, and
	// what we hand MAME after the chain. Equal whenever the pipeline is off, and the read-out
	// printing them equal is the evidence that a run was unshaped.
	int32_t  raw    = 0;
	int32_t  shaped = 0;
	uint64_t polls  = 0;            // port-0 pad updates, i.e. frontend samples taken

	// --- the read-out's own ---
	uint32_t period       = 0;      // 0 = no periodic line
	bool     asked        = false;  // M2VK_STEER_LOG present at all
	uint64_t frame        = 0;
	uint64_t resolved_at  = 0;      // frame the detector ran on
	uint64_t polls_at     = 0;      // frontend samples already taken by then — see §7 question 2
	unsigned count        = 0;      // paddle fields found, including any beyond the two kept
	detail::steer_field field[2];
};

inline steer_state &steer()
{
	static steer_state s;
	return s;
}

// MAME's absolute-axis full scale. Everything here is in these units because that is what
// libretro_m2_pad_device::update() leaves in m_axes and what MAME's item getters read back.
constexpr int32_t STEER_ABS_MAX = 65536;
static_assert(osd::input_device::ABSOLUTE_MAX == STEER_ABS_MAX, "MAME's absolute axis scale moved");

namespace detail {

// One switch, as a fraction, clamped to the band the plan gives it (§3.6). Out of range is clamped
// rather than refused, for the reason set_option_resolution gives: a run that names an absurd value
// should still run, and the resolve report prints what was actually used.
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

// The three core options' values, parked here by set_option_steering() from the libretro thread. They
// live OUTSIDE steer_state deliberately: that struct belongs to the loaded machine and steer_close()
// resets it, while these belong to the player and must survive a content change — and they are set at
// retro_load_game(), i.e. before the machine that will read them exists.
//
// The initialisers are the three options' declared defaults (5 %, Slight, 100 %), and they are what
// the chain runs if set_option_steering() is never called at all. ⚠️ They must agree with
// DEFINITIONS[] in retro_options.cpp, and nothing checks that they do — one cannot derive from the
// other without dragging libretro.h into a header the emulation thread includes.
inline float g_opt_dz    = 0.05f;
inline float g_opt_gamma = 1.3f;
inline float g_opt_range = 1.0f;

} // namespace detail

// Resolve the three numbers the chain runs, from the options and the switches that override them.
// Called from steer_config() when the machine starts and from set_option_steering() whenever the
// player changes an option, so that a live change and a change at load mean exactly the same thing —
// which is the reason this is a function rather than four lines in each caller (m2vk_sink.cpp's
// apply_force_solid() exists for the same reason and says so).
inline void steer_apply()
{
	steer_state &s = steer();

	s.dz_opt    = s.sw_dz    ? s.sw_dz_val    : detail::g_opt_dz;
	s.gamma     = s.sw_gamma ? s.sw_gamma_val : detail::g_opt_gamma;
	s.range_opt = s.sw_range ? s.sw_range_val : detail::g_opt_range;

	// The only thing that can switch the chain off entirely. An explicit M2VK_STEER_LINEAR=0 is a
	// request for shaping and beats nothing, because shaping is already what a run without the switch
	// does — it exists so that a harness script can be explicit rather than rely on that staying true.
	s.shaping = !s.sw_force_linear;
}

// The core options' values, from the libretro thread. Safe without locking for the same reason
// set_option_force_solid() is: the only caller runs in retro_load_game() before the machine exists, or
// in retro_run() with the emulation thread parked on the frame baton.
//
// ⚠️ At load this runs BEFORE steer_config() has read the switches, so it composes against switch
// flags that are still all false. That cannot shape a frame wrongly — steer_shape() also requires
// steer().active, which only the detector sets and which cannot have run yet — and input_init()
// recomposes with the real flags a moment later.
inline void set_option_steering(float deadzone, float gamma, float range)
{
	// Clamped to the same bands the switches are, so that the two sources cannot mean different things
	// — and clamped rather than refused for steer_env()'s reason: a run that names an absurd value
	// should still run, with the read-out printing what was actually used. Nothing the menu offers is
	// out of band; a hand-written .opt file naming "0%" range is what this catches, and a wheel that
	// does not turn would read as a broken core rather than as a setting.
	detail::g_opt_dz    = (deadzone < 0.0f) ? 0.0f : ((deadzone > 0.5f) ? 0.5f : deadzone);
	detail::g_opt_gamma = (gamma    < 1.0f) ? 1.0f : ((gamma    > 4.0f) ? 4.0f : gamma);
	detail::g_opt_range = (range    < 0.5f) ? 0.5f : ((range    > 1.0f) ? 1.0f : range);
	steer_apply();
}

// The switches, read once per machine. Called from input_init(), where every other per-machine
// capture happens and which runs long before the first frontend sample — so the pad never shapes a
// frame against a half-built configuration.
inline void steer_config()
{
	steer_state &s = steer();

	char const *const linear = std::getenv("M2VK_STEER_LINEAR");
	s.sw_linear = (linear != nullptr);
	// A value, not a presence — the same discipline M2VK_BLEND has and for the same reason: now that
	// the options exist, a harness run has to be able to pin shaping ON as well as off, and =0 is how.
	s.sw_force_linear = s.sw_linear && (std::strtol(linear, nullptr, 10) != 0);

	s.sw_dz    = detail::steer_env("M2VK_STEER_DEADZONE", s.sw_dz_val,    0.0f, 0.5f);
	s.sw_gamma = detail::steer_env("M2VK_STEER_GAMMA",    s.sw_gamma_val, 1.0f, 4.0f);
	s.sw_range = detail::steer_env("M2VK_STEER_RANGE",    s.sw_range_val, 0.5f, 1.0f);

	steer_apply();
}

// The chain, §3.4. In and out are the frontend's deflection and the value MAME is to be handed, both
// in ±STEER_ABS_MAX. Called from the pad's publish_steer() on every port, after the lightgun gate, so
// what it returns is the last word on that axis.
//
// Off unless the machine steers — the detector is the whole gate as of step 3, and it is what keeps a
// curve off vf2's digital-from-analog joystick, von's twin stick and the gun sets' pointer. The other
// half, M2VK_STEER_LINEAR, is the harness's and not a feature.
inline int32_t steer_shape(int32_t raw)
{
	const steer_state &s = steer();
	if (!s.shaping || !s.active)
		return raw;

	// MAME's two thresholds as integers, computed exactly the way input_device_joystick's constructor
	// computes them (inputdev.cpp:449-451) — truncation included, so the inversion below is against
	// the numbers that will actually be applied rather than against the fractions they came from.
	const int32_t dz  = int32_t(double(s.deadzone) * double(STEER_ABS_MAX));
	const int32_t sat = int32_t(double(s.saturation) * double(STEER_ABS_MAX));
	const int32_t rng = sat - dz;
	if (rng <= 0)   // a pathological -joystick_deadzone/-saturation pair; leave the axis alone
		return raw;

	double u = double(raw) / double(STEER_ABS_MAX);
	if (u > 1.0)  u = 1.0;
	if (u < -1.0) u = -1.0;
	const double mag = std::fabs(u);

	// 1. Deadzone, rescaled rather than merely clipped, so the travel it costs is given back to the
	//    rest of the sweep and slow movements stay available. This is also the only exit that returns
	//    a hard zero, which is what keeps a centred stick centred at any setting — including dz_opt=0,
	//    where a zero sample lands here and leaves as one.
	if (mag <= double(s.dz_opt))
		return 0;
	double a = (mag - double(s.dz_opt)) / (1.0 - double(s.dz_opt));

	// 2. Curve. Full lock stays reachable at every gamma; only the distribution of travel changes.
	if (s.gamma != 1.0f)
		a = std::pow(a, double(s.gamma));

	// 3. Range: a cap on maximum lock, composed with the curve rather than replacing it.
	a *= double(s.range_opt);
	if (a > 1.0)
		a = 1.0;

	// 4. Pre-compensation. MAME will compute floor((|r| - dz) * ABS_MAX / rng) and saturate at sat, so
	//    emitting dz + ceil(a * rng) makes that the identity: a==1 lands exactly on sat, which is the
	//    branch returning full scale, and small a survives because rng/ABS_MAX is 0.7 rather than 0.
	//    Not every output is reachable — 0.7 * ABS_MAX inputs cannot cover ABS_MAX + 1 outputs — so
	//    the round trip is exact to about 1.5 LSB of 65536, i.e. under 0.003 of a 96-count wheel.
	const int64_t want = int64_t(a * double(STEER_ABS_MAX) + 0.5);
	int32_t out = dz + int32_t((want * rng + (STEER_ABS_MAX - 1)) / STEER_ABS_MAX);
	if (out > STEER_ABS_MAX)
		out = STEER_ABS_MAX;
	return (u < 0.0) ? -out : out;
}

namespace detail {

// Locates the paddle fields and prints the reference line a sweep is checked against. Runs once,
// whether or not the read-out was asked for: steer().active is what step 2 gates on, so it cannot be
// conditional on a diagnostic variable being set.
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

	// 🚨 Announced even when the read-out was not asked for, and gated on a switch being PRESENT rather
	// than on the pipeline being on — because as of step 3 the pipeline is on in a default run, and a
	// line printed every time is one nobody reads. "Read [model2] options: before believing a result"
	// is the rule the whole harness rests on, and a switch is exactly the case where that line and the
	// run disagree. retro_entry.cpp announces the same switches on the libretro side, before the
	// machine starts; this one is the emulation thread's, and it is the one that can say whether the
	// machine being announced about actually steers.
	if (s.sw_linear || s.sw_dz || s.sw_gamma || s.sw_range)
	{
		std::fprintf(stderr, "[steer] M2VK_STEER_* is set; it overrides the matching core option. Shaping is %s%s\n",
				s.shaping ? "ON" : "OFF (M2VK_STEER_LINEAR)",
				(s.shaping && !s.active) ? ", but this machine has no paddle, so nothing is shaped" : "");
	}

	if (!s.asked)
		return;

	std::fprintf(stderr, "[steer] %s: %u IPT_PADDLE%s field(s) -> steering shaping %s apply here\n",
			machine.system().name, s.count, "/IPT_PADDLE_V",
			s.active ? "WOULD" : "would NOT");

	for (unsigned n = 0; n < s.count && n < std::size(s.field); n++)
	{
		const steer_field &f = s.field[n];
		std::fprintf(stderr, "[steer]   p%d \"%s\" %s range 0x%03x..0x%03x centre 0x%03x\n",
				f.player + 1, f.name.c_str(),
				f.vertical ? "IPT_PADDLE_V" : "IPT_PADDLE",
				f.minval, f.maxval, (f.minval + f.maxval) / 2);
	}

	// The ordering check §7 question 2 asks for, measured rather than argued. The claim is that the
	// first frame the frontend ever sees is the first RUNNING one (libretro_m2_osd.cpp:274-278), so
	// the detector resolves before poll_frontend() has ever run and no frame is ever polled with the
	// flag still off. polls=0 here is that claim holding.
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
		// Each number says where it came from, because the two sources disagree exactly when it
		// matters: a run pinned by a switch and a run reading a .opt file left in some state by an
		// interactive session look identical from everywhere else.
		std::fprintf(stderr, "[steer] pipeline ON: deadzone=%.3f (%s) gamma=%.2f (%s) range=%.3f (%s) + pre-compensation\n",
				double(s.dz_opt),    s.sw_dz    ? "switch" : "core option",
				double(s.gamma),     s.sw_gamma ? "switch" : "core option",
				double(s.range_opt), s.sw_range ? "switch" : "core option");
	}

	if (s.period != 0)
		std::fprintf(stderr, "[steer] read-out active, every %u frame(s)\n", s.period);
}

} // namespace detail

// Called once per emulated frame, on the emulation thread, from the OSD's update() — so it reports
// the state the frame being handed over was emulated from, and its frame number is the index a
// retrohost control script and the M2VK_HOST_* triggers use.
inline void steer_frame(running_machine &machine)
{
	steer_state &s = steer();

	// Counted before anything can bail out, so the number stays the frontend's frame index whatever
	// the state of the machine.
	const uint64_t frame = s.frame++;

	// Resolution waits for safe_to_read() for the reason in the header: asked any earlier, the port
	// list is empty and the detector says "no paddle" about a game that steers.
	if (!machine.ioport().safe_to_read())
		return;
	if (!s.resolved)
		detail::steer_resolve(machine, frame);

	// The read-out bar, m2vk_steerbar.h. Published before the periodic-log gate below, because it is
	// drawn every frame and the log is printed every nth.
	//
	// 🚨 The port is READ ONLY WHEN THE BAR IS ON. Not an optimisation — a default run must issue
	// exactly the port traffic it did before this existed, or the claim that the bar cannot disturb an
	// accuracy fixture rests on the read being harmless rather than on it not happening. (It is also
	// harmless: 831 reads over a 4120-frame daytona run moved no digest at step 2. Both are true and
	// only one of them is a guarantee.)
	if (steerbar_on() && s.active && s.field[0].present())
	{
		const detail::steer_field &f = s.field[0];

		// Normalised about the field's OWN centre, so daytona's 0x20..0xe0 and srallyc's 0x00..0xff
		// both come out as -1..+1 and the bar means the same thing on every cabinet. An odd-width
		// range has a half-count centre (srallyc's is 0x7f.8) and this is deliberately not rounded:
		// rounding it would make dead-centre read as a hair off centre on exactly those sets.
		const float centre = 0.5f * float(f.minval + f.maxval);
		const float half   = 0.5f * float(f.maxval - f.minval);
		float value = (half > 0.0f) ? ((float(f.read()) - centre) / half) : 0.0f;
		if (value < -1.0f) value = -1.0f;
		if (value >  1.0f) value =  1.0f;

		// The stick as the frontend gave it, before any of the shaping — the gap between the two is
		// the curve, and showing it is most of why the bar is worth drawing.
		float raw = float(s.raw) / float(STEER_ABS_MAX);
		if (raw < -1.0f) raw = -1.0f;
		if (raw >  1.0f) raw =  1.0f;

		// A vertical paddle steers nothing a bar across the screen can describe, so it is reported as
		// centred rather than drawn sideways. No set in the tree has one; this is here so that a set
		// that does cannot draw a lie.
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

// The located fields belong to the machine that is going away; a second retro_load_game builds new
// ports and the pointers above would be dangling. Resolving again also re-reads the environment and
// the machine's analog options, which costs nothing and keeps the two loads independent.
//
// ⚠️ This drops the shaping configuration with everything else, so it must not run after the next
// machine's input_init(). It does not: osd_exit() belongs to the machine being torn down.
inline void steer_close()
{
	steer() = steer_state();
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_STEER_H
