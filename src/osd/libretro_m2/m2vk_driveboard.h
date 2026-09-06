// license:BSD-3-Clause
//============================================================
//  m2vk_driveboard.h — park the force-feedback drive-board Z80 (model2_drive_board option)
//
//  The wheel cabinets (daytona, srallyc, ...) carry a Z80 drive board whose only job is force
//  feedback — output the player never feels on a gamepad. The Quest 3 profiler put it at ~6% of
//  the frame (devnotes/reference/retroarch-quest-perf.md §4.1), pure waste on a pad. Parking suspends the
//  Z80 via SUSPEND_REASON_DISABLE: it executes no cycles but stays a device, so savestates keep
//  their shape. The comm latch the main CPU writes stays writable; the board just never answers —
//  the same face the game sees during the real board's self-test window.
//
//  Applied per frame from the OSD's update() on the emulation thread, so the option is live in
//  both directions (suspend <-> resume) and no upstream file is edited. The option defaults to
//  the accurate arm (board running); M2VK_DRIVE_BOARD=0|1 overrides for host runs.
//============================================================
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_DRIVEBOARD_H
#define MAME_OSD_LIBRETRO_M2_M2VK_DRIVEBOARD_H

#pragma once

#include "emu.h"

#include <cstdlib>

namespace m2vk {

inline bool g_drive_park_wanted = false;   // option: true = park the board

inline void set_option_drive_park(bool park)
{
	// The matching M2VK_* switch overrides its option, never the reverse (project rule).
	if (char const *const env = std::getenv("M2VK_DRIVE_BOARD"))
	{
		g_drive_park_wanted = (std::atoi(env) == 0);
		return;
	}
	g_drive_park_wanted = park;
}

// Called once per frame on the emulation thread (osd update()). Reconciles the drive CPU's
// suspend state with the option; a machine without a drive board is a no-op every frame.
inline void drive_park_frame(running_machine &machine)
{
	static running_machine *s_machine = nullptr;
	static bool s_parked = false;
	if (s_machine != &machine)
	{
		s_machine = &machine;
		s_parked = false;
	}
	if (s_parked == g_drive_park_wanted)
		return;

	device_t *const dev = machine.root_device().subdevice("drivecpu");
	if (dev == nullptr)
	{
		s_parked = g_drive_park_wanted;   // no board on this set; settle so the check stays cheap
		return;
	}
	device_execute_interface *exec = nullptr;
	if (!dev->interface(exec))
		return;
	if (g_drive_park_wanted)
		exec->suspend(SUSPEND_REASON_DISABLE, true);
	else
		exec->resume(SUSPEND_REASON_DISABLE);
	s_parked = g_drive_park_wanted;
	osd_printf_info("[m2vk] drive-board Z80 %s\n", g_drive_park_wanted ? "parked" : "running");
}

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_DRIVEBOARD_H
