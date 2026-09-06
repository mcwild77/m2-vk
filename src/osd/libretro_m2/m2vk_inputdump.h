// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 ioport dump — the machine describing its own controls, as JSON.

    M2VK_INPUT_DUMP=<path> writes one JSON object describing every input field the loaded set
    declares, then returns.  It is the data source for devnotes/tools/padmap.html, the layout editor:
    the whole point of that tool is that nobody has to be told which button does which, because the
    driver already says — every PORT_NAME in model2.cpp is a sentence written by somebody looking at
    the real cabinet ("VR1 (Red)", "Hand Brake", "Bat Swing", "Select (Up)").

    It exists because devnotes/reference/input-map.md, the hand audit this replaces, says in its own §6 that
    nothing was run: it is 32 port sets transcribed by eye out of model2.cpp, and five of its findings
    are marked inferred-rather-than-measured.  Transcription is exactly what a layout table must not
    rest on — the shoulder-button descriptors were inverted for months because two hand-written tables
    disagreed (§5.1).  So this reads the live ioport list, after PORT_INCLUDE and PORT_MODIFY have been
    applied, which is the only place the answer is unambiguous.

    Three derived flags accompany the fields, and they are here rather than in the tool because they
    are the two rules a layout row can break invisibly, plus the one that decides the pad's shape:

      * joystick  — the set declares some IPT_JOYSTICK_*.  A layout row may only point a numbered
                    button at a D-pad control when this is false, because the D-pad slots keep their
                    ITEM_ID_HAT1* items and the IPT_JOYSTICK_* assignments add_directional_assignments()
                    gives them; on a set with a stick, one press would feed two MAME items.
                    devnotes/plan_finished/per-game-input.md §2.3.
      * pedals    — the set declares IPT_PEDAL/PEDAL2/PEDAL3.  A row may only name the L2/R2 trigger
                    *thresholds* as a button source when this is false.  Naming one anyway is the
                    daytona collision — flooring the accelerator also presses VR3 — which is the
                    specific bug the per-game table exists to kill.
      * players   — how many PLAYER(n) blocks the set really uses, which is how the tool knows whether
                    to offer port 2 at all.

    What it deliberately does not emit: DIP switches and configuration ports.  They are not things a
    RetroPad control can be pointed at, and a list that includes them reads as if 40 of the entries
    were unassigned.  INPUT_CLASS_MISC is kept and tagged, because coin/start/service are what the
    tool has to show under "handled automatically" — omitting them entirely reads as if they were
    missing.

    ⚠ Include this AFTER emu.h, for the same reason m2vk_gunlog.h says so: it is header-only so that
    it needs no entry in the two build scripts, and it names ioport types, which only emu.h brings in.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_INPUTDUMP_H
#define MAME_OSD_LIBRETRO_M2_M2VK_INPUTDUMP_H

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace m2vk {

namespace detail {

// JSON string escaping, over the characters a PORT_NAME can actually contain. The driver's names are
// plain ASCII with the occasional apostrophe and slash, so this is deliberately minimal rather than a
// general escaper — but it must not be *wrong*, because a stray backslash would take out the tool's
// whole data file rather than one field.
inline std::string json_escape(char const *s)
{
	std::string out;
	if (s == nullptr)
		return out;

	for (char const *p = s; *p != '\0'; p++)
	{
		switch (*p)
		{
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if (static_cast<unsigned char>(*p) < 0x20)
				out += ' ';                     // control characters have no business in a name
			else
				out += *p;
			break;
		}
	}
	return out;
}

// The settings token for a (type, player) pair — "P1_BUTTON1", "PADDLE", "SERVICE1". This is the
// stable machine-readable name of a MAME input type, and it is what the tool keys on: the numeric
// ioport_type is an enum position and would move under an upstream merge, while a token is what
// MAME's own .cfg files are written in and therefore cannot move without breaking those too.
inline char const *type_token(running_machine &machine, ioport_type type, int player)
{
	for (input_type_entry const &entry : machine.ioport().types())
	{
		if ((entry.type() == type) && (entry.player() == player))
			return entry.token();
	}
	return "";
}

// The class, as a word rather than a number, for the same reason as the token above. The tool sorts on
// this: "controller" is what the author assigns, "misc" is what MAME's own defaults already bind.
inline char const *type_class_name(ioport_type_class cls)
{
	switch (cls)
	{
	case INPUT_CLASS_CONTROLLER: return "controller";
	case INPUT_CLASS_MISC:       return "misc";
	case INPUT_CLASS_KEYBOARD:   return "keyboard";
	case INPUT_CLASS_CONFIG:     return "config";
	case INPUT_CLASS_DIPSWITCH:  return "dipswitch";
	default:                     return "internal";
	}
}

} // namespace detail


// Writes the dump and says whether it did.
//
// 🚨 It CANNOT be called from input_init(), and that mistake was made and measured before this comment
// was written: osd().init() is machine.cpp:156 and m_ioport.initialize() is machine.cpp:169, so at
// input_init() the port list is EMPTY and the dump comes out as `"fields": []` on a set with twenty of
// them. The output looked structurally perfect, which is the whole danger — m2vk_gunlog.h's
// gun_log_frame() carries the identical warning about the identical trap, and it was read afterwards
// rather than before. Hence input_dump_frame() below, which is what callers use.
//
// Returns false when the switch is not set, which is the ordinary case: every M2VK_* instrument in
// this tree is off unless asked for, so that a harness run measures the core and not the instrument.
inline bool input_dump(running_machine &machine)
{
	char const *const path = std::getenv("M2VK_INPUT_DUMP");
	if ((path == nullptr) || (*path == '\0'))
		return false;

	std::FILE *const out = std::fopen(path, "w");
	if (out == nullptr)
	{
		std::fprintf(stderr, "[inputdump] cannot write '%s'\n", path);
		return false;
	}

	// --- the derived flags, gathered in one pass before anything is printed ---
	//
	// The button set is collected over every player rather than per player on purpose: a layout row has
	// nine slots and MAME resolves the player from the device index, so "does this set have a button 7"
	// is a question about the set. player() is 0-based and PLAYER(n) is 1-based, hence the +1.
	bool joystick = false;
	bool pedals   = false;
	bool lightgun = false;
	bool buttons[9] = { false, false, false, false, false, false, false, false, false };
	int  players  = 0;

	for (auto const &port : machine.ioport().ports())
	{
		for (ioport_field const &field : port.second->fields())
		{
			const ioport_type type = field.type();

			// field.is_digital_joystick() rather than a range of our own: it is MAME's own predicate
			// (ioport.h:636, IPT_DIGITAL_JOYSTICK_FIRST..LAST) and so it covers the twin-stick types
			// too, which a hand-written IPT_JOYSTICK_UP..RIGHT range would silently miss on von.
			if (field.is_digital_joystick())
				joystick = true;
			if ((type == IPT_PEDAL) || (type == IPT_PEDAL2) || (type == IPT_PEDAL3))
				pedals = true;
			if ((type == IPT_LIGHTGUN_X) || (type == IPT_LIGHTGUN_Y))
				lightgun = true;
			if ((type >= IPT_BUTTON1) && (type <= IPT_BUTTON9))
				buttons[type - IPT_BUTTON1] = true;

			// Only controller fields say anything about the player count. A DIP switch is player 0 and
			// would make every set look like a one-player cabinet, which is true often enough to hide
			// the bug.
			if ((field.type_class() == INPUT_CLASS_CONTROLLER) && ((field.player() + 1) > players))
				players = field.player() + 1;
		}
	}

	game_driver const &system = machine.system();

	std::fprintf(out, "{\n");
	std::fprintf(out, "  \"set\": \"%s\",\n", detail::json_escape(system.name).c_str());
	std::fprintf(out, "  \"parent\": \"%s\",\n", detail::json_escape(system.parent).c_str());
	std::fprintf(out, "  \"description\": \"%s\",\n", detail::json_escape(system.type.fullname()).c_str());
	std::fprintf(out, "  \"players\": %d,\n", players);
	std::fprintf(out, "  \"flags\": { \"joystick\": %s, \"pedals\": %s, \"lightgun\": %s },\n",
			joystick ? "true" : "false", pedals ? "true" : "false", lightgun ? "true" : "false");

	std::fprintf(out, "  \"buttons\": [");
	bool first = true;
	for (unsigned n = 0; n < 9; n++)
	{
		if (!buttons[n])
			continue;
		std::fprintf(out, "%s%u", first ? "" : ", ", n + 1);
		first = false;
	}
	std::fprintf(out, "],\n");

	// --- the fields ---
	//
	// Emitted in ioport order, which is the driver's own source order, so a reader comparing this
	// against model2.cpp's INPUT_PORTS block walks the two in step.
	std::fprintf(out, "  \"fields\": [\n");

	int  fieldcount = 0;
	bool firstfield  = true;
	for (auto const &port : machine.ioport().ports())
	{
		for (ioport_field const &field : port.second->fields())
		{
			const ioport_type_class cls = field.type_class();
			if ((cls == INPUT_CLASS_DIPSWITCH) || (cls == INPUT_CLASS_CONFIG) || (cls == INPUT_CLASS_INTERNAL))
				continue;

			if (!firstfield)
				std::fprintf(out, ",\n");
			firstfield = false;
			fieldcount++;

			// name() is the resolved name and is always usable; specific_name() is the driver's own
			// PORT_NAME and is null when the field was never named. Both are emitted, because the
			// difference matters to the tool: an authored label should default to a name a human wrote
			// about a cabinet, and fall back to a generic type name only visibly.
			std::fprintf(out, "    { \"port\": \"%s\", \"mask\": %u, \"type\": %d, \"token\": \"%s\", \"class\": \"%s\",\n",
					detail::json_escape(port.first.c_str()).c_str(),
					unsigned(field.mask()),
					int(field.type()),
					detail::type_token(machine, field.type(), field.player()),
					detail::type_class_name(cls));
			std::fprintf(out, "      \"player\": %d, \"name\": \"%s\", \"named\": %s",
					field.player(),
					detail::json_escape(field.name().c_str()).c_str(),
					(field.specific_name() != nullptr) ? "true" : "false");

			if (field.is_analog())
			{
				// PORT_MINMAX, PORT_SENSITIVITY, PORT_KEYDELTA, PORT_CENTERDELTA and the four analog
				// flags. The tool shows these rather than acting on them — analog routing is a later
				// step — but they are what makes input-map.md §5.2 legible: desert's brake is an
				// AD_STICK_Y whose range starts at 0, so a self-centring stick rests it half applied.
				std::fprintf(out, ",\n      \"analog\": { \"min\": %u, \"max\": %u, \"sens\": %d, \"delta\": %d, \"center\": %d,"
						" \"reverse\": %s, \"reset\": %s, \"wraps\": %s, \"invert\": %s }",
						unsigned(field.minval()), unsigned(field.maxval()),
						int(field.sensitivity()), int(field.delta()), int(field.centerdelta()),
						field.analog_reverse() ? "true" : "false",
						field.analog_reset()   ? "true" : "false",
						field.analog_wraps()   ? "true" : "false",
						field.analog_invert()  ? "true" : "false");
			}

			std::fprintf(out, " }");
		}
	}

	std::fprintf(out, "\n  ]\n}\n");
	std::fclose(out);

	// Every Model 2 set declares at least a coin and a start, so zero fields never means "this set has
	// no controls" — it means the port list was not ready and the caller asked too early. Said loudly
	// because the file it just wrote is valid JSON and the tool would load it without complaint.
	if (fieldcount == 0)
		std::fprintf(stderr, "[inputdump] 🚨 '%s' reported ZERO fields — the port list was not ready; see the header\n", system.name);

	std::fprintf(stderr, "[inputdump] '%s': %d player(s), %d field(s), joystick=%d pedals=%d lightgun=%d -> %s\n",
			system.name, players, fieldcount, int(joystick), int(pedals), int(lightgun), path);
	return true;
}


// The per-frame entry point, and the only one a caller should use. Fires once, on the first frame at
// which the port list is readable, and then costs one bool test for the rest of the run.
//
// safe_to_read() is the gate for the reason gun_log_frame() documents: the first update() lands before
// ioport_manager::initialize() has run, and asking then reports that a set with twenty fields has none.
// The sweep script relies on this firing early — it asks for a handful of frames and expects the file
// to exist afterwards.
inline void input_dump_frame(running_machine &machine)
{
	static bool done = false;
	if (done || !machine.ioport().safe_to_read())
		return;

	done = true;
	input_dump(machine);
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_INPUTDUMP_H
