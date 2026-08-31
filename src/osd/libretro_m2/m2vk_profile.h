// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 per-device CPU profiler read-out — diagnostic instrumentation for the Quest 3
    emulation-speed work.

    A heavy Model 2 frame is compute-bound on the interpreted CPUs (two i960s, the TGP geometry
    copro, a 68000 and a Z80, no recompiler), and the question the optimisation work has to answer
    first is *which* of them dominates a heavy scene. On-device sampling profiling is blocked by the
    Quest's SELinux (simpleperf fails on the untrusted-app domain), so this reads MAME's OWN
    profiler instead: the scheduler already brackets every device's execute in
    g_profiler.start(exec->m_profiler) (schedule.cpp), each bucket is labelled by device tag
    (profiler.cpp update_text), and memory-handler / video / sound time land in their own buckets.
    All that is missing is a way to see it on the phone, which is what this adds.

    It is a build-gated instrument, NOT a runtime one. g_profiler is the real collector only in a
    PROFILER=1 build (scripts/genie.lua defines MAME_PROFILER globally then); the shipping build gets
    the dummy no-op profiler, so this whole file compiles to empty inlines and costs nothing. The
    build is the switch — there is no on-device knob to deliver, and the app cannot read an env var
    the launcher never set. M2VK_PROFILE still tunes the dump cadence (frames per dump, default 60,
    0 = off) on a desktop retrohost run where getenv works.

    Reading the result: the dump is a block of "unnorm% norm% 'device-tag'" lines (plus Memory Read /
    Memory Write / Video / Sound buckets) every ~second. Whichever device tops it under a heavy race
    is where Track B aims. ⚠ Enabling the profiler inflates absolute frame time (the per-scope tick
    reads are real work), so the [speed]/realtime figure is meaningless during a profile run — only
    the per-device PERCENTAGES are the signal.

    ⚠ Include this AFTER emu.h (for running_machine and g_profiler) and after libretro/OSD headers.
    It is header-only so that it needs no entry in the two build scripts, matching the other m2vk_*
    read-outs. Unlike them it targets the phone, so it logs to logcat via __android_log rather than
    stderr (the OSD's osd_printf_* defaults to stderr, which Android drops); on a desktop build it
    falls back to stderr so a retrohost run can read it too.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_PROFILE_H
#define MAME_OSD_LIBRETRO_M2_M2VK_PROFILE_H

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace m2vk {

#ifdef MAME_PROFILER

namespace detail {

// One line to logcat (tag "m2prof") on device, or to stderr on a desktop/retrohost build. The
// caller passes a line with no trailing newline; __android_log_print adds one, and the stderr path
// supplies its own so the output reads the same either way.
inline void profile_emit(const char *line)
{
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "m2prof", "%s", line);
#else
	std::fprintf(stderr, "[m2prof] %s\n", line);
#endif
}

struct profile_state
{
	bool     started       = false;   // has g_profiler been enabled for this machine yet
	bool     off           = false;   // M2VK_PROFILE=0 kill switch, even in a PROFILER=1 build
	uint32_t period_frames = 60;      // dump cadence in emulated frames (~1 s at 57.5 Hz)
	uint64_t frame         = 0;       // frames counted since enable
};

inline profile_state &profile_singleton()
{
	static profile_state s;
	return s;
}

} // namespace detail

// Called once per emulated frame from the OSD's update(), on the emulation thread and only after the
// machine reaches RUNNING (update() returns early before that, so no phase check is needed here).
// The first call enables MAME's built-in profiler and reads the cadence; every period_frames after
// that it dumps g_profiler's per-device breakdown.
inline void profile_frame(running_machine &machine)
{
	detail::profile_state &s = detail::profile_singleton();

	if (!s.started)
	{
		s.started = true;

		// M2VK_PROFILE — dump period in FRAMES. Unset (the on-device case: getenv returns null)
		// keeps the 60-frame default; "0" turns the instrument off even though the build carries it.
		char const *const env = std::getenv("M2VK_PROFILE");
		if (env != nullptr)
		{
			const long v = std::strtol(env, nullptr, 10);
			if (v <= 0)
			{
				s.off = true;
				detail::profile_emit("off (M2VK_PROFILE=0)");
				return;
			}
			s.period_frames = uint32_t(v);
		}

		g_profiler.enable(true);

		char line[128];
		std::snprintf(line, sizeof(line),
				"on: MAME per-device profiler, dump every %u frame(s) — read percentages, not [speed]",
				s.period_frames);
		detail::profile_emit(line);
		return;
	}

	if (s.off)
		return;

	// Dump on the period. text() refreshes and zeroes its accumulators only when it is called and
	// more than 0.5 s (emulated) have passed, so calling it exactly at the dump point makes each
	// window span the whole period since the previous dump rather than a stale 0.5 s slice.
	if ((++s.frame % s.period_frames) != 0)
		return;

	const char *text = g_profiler.text(machine);
	if ((text == nullptr) || (text[0] == '\0'))
		return;

	char header[64];
	std::snprintf(header, sizeof(header), "---- f=%llu ----", (unsigned long long)s.frame);
	detail::profile_emit(header);

	// text() is a multi-line block ("unnorm% norm% 'device-tag'\n" per device, "unnorm% name\n" per
	// subsystem bucket). Split on '\n' and emit each line so logcat keeps them separate and greppable.
	char linebuf[256];
	std::size_t at = 0;
	for (const char *p = text; ; ++p)
	{
		if ((*p == '\n') || (*p == '\0'))
		{
			linebuf[at] = '\0';
			if (at > 0)
				detail::profile_emit(linebuf);
			at = 0;
			if (*p == '\0')
				break;
		}
		else if ((at + 1) < sizeof(linebuf))
		{
			linebuf[at++] = *p;
		}
	}
}

// g_profiler is a global whose device buckets index into the machine being torn down; a second
// retro_load_game builds a different device tree. Stop collecting and reset our own state so the
// next load re-enables cleanly against the new machine.
inline void profile_close()
{
	detail::profile_state &s = detail::profile_singleton();
	if (s.started && !s.off)
		g_profiler.enable(false);
	s = detail::profile_state();
}

#else // MAME_PROFILER

// Shipping build: g_profiler is the dummy no-op, there is nothing to read, and this must add no
// per-frame cost — so both entry points compile to nothing.
inline void profile_frame(running_machine &) { }
inline void profile_close() { }

#endif // MAME_PROFILER

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_PROFILE_H
