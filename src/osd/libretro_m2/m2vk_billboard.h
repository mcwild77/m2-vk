// license:BSD-3-Clause
//============================================================
//  m2vk_billboard.h — park the cabinet-billboard Z80 (model2_billboard option)
//
//  Every Model 2A/2B set carries a Z80 whose only job is the cabinet's LED marquee
//  (segabill.cpp), shown through a layout view this core never renders. The Quest 3 profiler put
//  it at ~6% of the busy frame on rchase2 — output nobody can see. The main CPU only ever writes
//  to the board (io out_pe -> billboard->write); nothing flows back, so parking it is invisible
//  to the game. Parking suspends the Z80 via SUSPEND_REASON_DISABLE: it executes no cycles but
//  stays a device, so savestates keep their shape.
//
//  Applied per frame from the OSD's update() on the emulation thread, so the option is live in
//  both directions (suspend <-> resume) and no upstream file is edited. The option defaults to
//  PARKED (user call 2026-09-01 — the output is invisible on every set, so the accurate arm buys
//  nothing); M2VK_BILLBOARD=0|1 overrides for host runs, and a digest run that wants the stock
//  machine pins M2VK_BILLBOARD=1.
//============================================================
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_BILLBOARD_H
#define MAME_OSD_LIBRETRO_M2_M2VK_BILLBOARD_H

#pragma once

#include "emu.h"

#include <cstdlib>

namespace m2vk {

inline bool g_bill_park_wanted = false;    // option: true = park the board

// Set by bill_park_frame once it has looked for the device; read by retro_run to hide the menu
// entry on machines without a billboard (2O/2C, the other families).
struct bill_state { bool resolved = false; bool present = false; };
inline bill_state &bill() { static bill_state s; return s; }

inline void set_option_bill_park(bool park)
{
	// The matching M2VK_* switch overrides its option, never the reverse (project rule).
	if (char const *const env = std::getenv("M2VK_BILLBOARD"))
	{
		g_bill_park_wanted = (std::atoi(env) == 0);
		return;
	}
	g_bill_park_wanted = park;
}

// Called once per frame on the emulation thread (osd update()). Reconciles the billboard CPU's
// suspend state with the option; a machine without a billboard is a no-op every frame.
inline void bill_park_frame(running_machine &machine)
{
	static running_machine *s_machine = nullptr;
	static bool s_parked = false;
	if (s_machine != &machine)
	{
		s_machine = &machine;
		s_parked = false;
		bill().resolved = false;
	}
	if (!bill().resolved)
	{
		bill().present = machine.root_device().subdevice("billboard:billcpu") != nullptr;
		bill().resolved = true;
	}
	if (s_parked == g_bill_park_wanted)
		return;

	if (!bill().present)
	{
		s_parked = g_bill_park_wanted;    // no board on this set; settle so the check stays cheap
		return;
	}
	device_t *const dev = machine.root_device().subdevice("billboard:billcpu");
	device_execute_interface *exec = nullptr;
	if (!dev->interface(exec))
		return;
	if (g_bill_park_wanted)
		exec->suspend(SUSPEND_REASON_DISABLE, true);
	else
		exec->resume(SUSPEND_REASON_DISABLE);
	s_parked = g_bill_park_wanted;
	osd_printf_info("[m2vk] billboard Z80 %s\n", g_bill_park_wanted ? "parked" : "running");
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_BILLBOARD_H
