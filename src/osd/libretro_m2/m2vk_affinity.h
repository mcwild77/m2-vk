// license:BSD-3-Clause
//============================================================
//  m2vk_affinity.h — keep the frame-critical threads on the big-core cluster
//
//  Diagnosis (Quest 3, 2026-09-01): the Android scheduler migrates the frame-critical threads
//  freely across clusters, and this chip is 2 little (cpu0-1, held 1.38 GHz) + 4 big (cpu2-5,
//  held 1.92 GHz). Three threads matter: the emulation thread, the sound worker, and the
//  frontend thread that calls retro_run — the emulation thread parks on the baton until that
//  third one comes back, so ITS wake-up latency on a little core is emulation time lost.
//
//  A thread may set its own affinity without any privilege. But Android WIPES the mask back to
//  the cpuset default on app-state transitions (observed live: the pin read 2-5, then 0-5 a few
//  minutes later), so a one-shot pin is not enough — each thread re-asserts periodically via
//  m2vk_repin_self(), one cheap syscall against a mask computed once.
//
//  Cluster detection is by cpuinfo_max_freq (hardware max, unaffected by DVFS): clusters are
//  added fastest-first until the mask holds at least two CPUs, so a 1-prime + 4-mid topology
//  doesn't cram everything onto the one prime core. Homogeneous CPUs (every core the same max)
//  disable the whole mechanism, as does non-Linux.
//============================================================
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_AFFINITY_H
#define MAME_OSD_LIBRETRO_M2_M2VK_AFFINITY_H

#pragma once

#if defined(__linux__)

#include <sched.h>
#include <cstdio>

namespace m2vk_affinity_detail {

struct big_mask
{
	cpu_set_t mask;
	int       count = 0;   // CPUs in the mask; 0 = pinning disabled

	big_mask()
	{
		struct { int cpu; long khz; } info[64];
		int n = 0;
		for (int c = 0; c < 64; c++)
		{
			// Offline/hidden cores (the Quest reserves two for the compositor) have no readable
			// cpufreq node; skip them rather than stopping at the first gap.
			char path[96];
			std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
			std::FILE *f = std::fopen(path, "r");
			if (!f)
				continue;
			long khz = 0;
			if (std::fscanf(f, "%ld", &khz) != 1)
				khz = 0;
			std::fclose(f);
			if (khz > 0 && n < 64)
				info[n++] = { c, khz };
		}
		CPU_ZERO(&mask);
		if (n < 2)
			return;
		int in_mask = 0;
		long floor = 0;   // slowest tier admitted so far
		while (in_mask < 2)
		{
			long tier = 0;   // fastest tier not yet admitted
			for (int i = 0; i < n; i++)
				if ((floor == 0 || info[i].khz < floor) && info[i].khz > tier)
					tier = info[i].khz;
			if (tier == 0)
				break;
			for (int i = 0; i < n; i++)
				if (info[i].khz == tier)
				{
					CPU_SET(info[i].cpu, &mask);
					in_mask++;
				}
			floor = tier;
		}
		if (in_mask >= 2 && in_mask < n)   // in_mask == n: homogeneous, constrains nothing
			count = in_mask;
	}
};

inline const big_mask &get_mask()
{
	static const big_mask m;   // computed once, thread-safe magic static
	return m;
}

} // namespace m2vk_affinity_detail

// Pin the calling thread to the fastest CPU cluster(s). Returns the number of CPUs in the
// applied mask, or 0 if nothing was done. Cheap after the first call (one syscall), so callers
// re-assert it periodically — Android wipes thread affinity on app cpuset transitions.
inline int m2vk_pin_self_to_big_cores()
{
	const auto &m = m2vk_affinity_detail::get_mask();
	if (m.count == 0)
		return 0;
	if (sched_setaffinity(0, sizeof(m.mask), &m.mask) != 0)
		return 0;
	return m.count;
}

#else

inline int m2vk_pin_self_to_big_cores() { return 0; }

#endif

// Per-thread periodic re-assert: call once per frame/iteration from the thread itself.
// One syscall every `interval` calls; the first call pins immediately.
inline void m2vk_repin_self(unsigned &counter, unsigned interval = 128)
{
	if (counter++ == 0)
	{
		m2vk_pin_self_to_big_cores();
		return;
	}
	if (counter >= interval)
		counter = 0;
}

#endif // MAME_OSD_LIBRETRO_M2_M2VK_AFFINITY_H
