// license:BSD-3-Clause
// copyright-holders:mcwild77
#ifndef M2VK_TWINSTICK_H
#define M2VK_TWINSTICK_H

/*
    Single-pad twin-AD-stick binding — Cyber Sled (System 21) and any kin that model a twin-stick
    cabinet as TWO PLAYERS' analog sticks rather than as one player's IPT_JOYSTICKLEFT/RIGHT pair.

    The problem, measured on cybsled: the left tread is P1's IPT_AD_STICK_X/Y and works (it lands on
    pad 1's left stick through add_directional_assignments), but the right tread is P2's
    IPT_AD_STICK_X/Y. apply_device_defaults (ioport.cpp) binds a device's defaults only to the player
    whose number equals the device index, so P2's stick binds to pad 2's LEFT stick — dead for a
    player holding a single controller, which is exactly the hand-check result ("right stick returns
    nothing").

    The fix is a cross-player binding the per-device assignment_vector cannot express: OR pad 1's RIGHT
    stick onto the *type* default for the second (and any further) player's IPT_AD_STICK_X/Y, via
    ioport_manager::set_type_seq. It is OR'd, not replaced, so a real second controller still drives it.

    Gated on the layout row's twin_ad_stick flag, NOT on a detector — because a genuine two-player
    analog game (Model 2's gunblade, rchase2) is indistinguishable at the ioport level: it too declares
    a player-2 IPT_AD_STICK, and there the cross-bind would let player 1's right stick drag player 2's
    aim. Only a cabinet the row marks as single-pad twin-stick (cybsled) opts in. The caller supplies
    the flag; this file only performs the binding.

    Runs once from update() rather than from input_init(), for the same reason the steering and gun
    detectors do: the port list is still empty when osd().init() runs (ioport_manager::initialize()
    comes later), so the fields cannot be seen there. twin_stick_close() re-arms it on a content reload,
    the way steer_close() does for the paddle fields.

    Include AFTER emu.h — it names ioport types and input codes, which only emu.h brings in.
*/

#include <cstdio>
#include <cstdlib>

namespace m2vk {

inline bool &twin_stick_done()
{
	static bool done = false;
	return done;
}

// The loaded set's layout-row twin_ad_stick flag. input_init() has the row in hand and parks it here;
// twin_stick_frame() reads it. An inline function's local static is one instance across every
// translation unit that includes this header, so the input module and the OSD share the same bool.
inline bool &twin_stick_enabled()
{
	static bool enabled = false;
	return enabled;
}

// Re-arm on a machine going away, so a second load re-decides. The enabled flag is re-parked by the
// next input_init(); clearing it here too keeps a set with no input_init (there is none) from
// inheriting the last set's answer. Paired with the update() call the way steer_close() pairs with
// steer_frame().
inline void twin_stick_close()
{
	twin_stick_done() = false;
	twin_stick_enabled() = false;
}

// Reads twin_stick_enabled(), parked by input_init() from the layout row. When false this is a no-op,
// so a game that is not a single-pad twin-stick cabinet is never touched however its ports look.
inline void twin_stick_frame(running_machine &machine)
{
	if (twin_stick_done())
		return;
	if (!twin_stick_enabled())
	{
		twin_stick_done() = true;   // nothing to do for this set, and re-armed on the next load
		return;
	}

	// The port list is filled by ioport_manager::initialize(); until it is, there is nothing to find
	// and we must not latch "done" (the steering detector waits the same way via its own count).
	auto const &ports = machine.ioport().ports();
	if (ports.empty())
		return;
	twin_stick_done() = true;

	unsigned bound = 0;
	for (auto const &port : ports)
	{
		for (ioport_field const &field : port.second->fields())
		{
			const bool isx = (field.type() == IPT_AD_STICK_X);
			const bool isy = (field.type() == IPT_AD_STICK_Y);
			if ((!isx && !isy) || (field.player() < 1))
				continue;   // player 0's stick already has pad 1's left stick — leave it alone

			// Pad 1 is joystick device index 0; its right stick is ZAXIS (X) / RZAXIS (Y), matching
			// AXIS_ITEMS[] in libretro_m2_input.cpp.
			const input_item_id item = isx ? ITEM_ID_ZAXIS : ITEM_ID_RZAXIS;
			const input_code code(DEVICE_CLASS_JOYSTICK, 0, ITEM_CLASS_ABSOLUTE, ITEM_MODIFIER_NONE, item);

			input_seq seq = machine.ioport().type_seq(field.type(), field.player(), SEQ_TYPE_STANDARD);
			if (!seq.empty())
				seq += input_seq::or_code;
			seq += code;
			machine.ioport().set_type_seq(field.type(), field.player(), SEQ_TYPE_STANDARD, seq);
			bound++;
		}
	}

	if (bound != 0)
		std::fprintf(stderr,
				"[twinstick] %s: bound pad 1's right stick onto %u player>1 IPT_AD_STICK axis field(s)\n",
				machine.system().name, bound);
}

} // namespace m2vk

#endif // M2VK_TWINSTICK_H
