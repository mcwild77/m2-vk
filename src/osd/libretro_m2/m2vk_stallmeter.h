// license:BSD-3-Clause
//============================================================
//  m2vk_stallmeter.h — where does the emulation thread's frame time go? (Android diagnostic)
//
//  Quest 3, 2026-09-01: in a heavy race the frame rate sits at ~52 of 57.5 while the emulation
//  thread is only ~76% busy — it is losing ~4 ms of every frame to something that is not compute.
//  This meter splits each baton cycle three ways using two clocks read at the park boundary:
//    cpu   — CLOCK_THREAD_CPUTIME_ID delta = actual emulation compute
//    park  — wall time inside the m_cv.wait for retro_run's release (frontend round-trip)
//    other — the remainder: MAME's own throttle sleeps, scheduler latency, page faults
//  One __android_log_print line every ~2 s, tag "m2stall". Compiled away off Android.
//============================================================
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_STALLMETER_H
#define MAME_OSD_LIBRETRO_M2_M2VK_STALLMETER_H

#pragma once

#if defined(__ANDROID__)

#include <android/log.h>
#include <ctime>
#include <cstdint>

namespace m2vk {

struct stall_meter
{
	int64_t win_wall0 = 0;   // window start, wall
	int64_t win_cpu0  = 0;   // window start, this thread's CPU time
	int64_t park_ns   = 0;   // wall time parked on the baton this window
	int64_t t_park0   = 0;   // set by park_begin
	int     frames    = 0;

	static int64_t now(clockid_t id)
	{
		timespec ts;
		clock_gettime(id, &ts);
		return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
	}

	void park_begin() { t_park0 = now(CLOCK_MONOTONIC); }

	// Call as the thread comes back off the baton; closes the frame.
	void park_end()
	{
		const int64_t wall = now(CLOCK_MONOTONIC);
		const int64_t cpu  = now(CLOCK_THREAD_CPUTIME_ID);
		park_ns += wall - t_park0;
		frames++;
		if (win_wall0 == 0)
		{
			win_wall0 = wall;
			win_cpu0  = cpu;
			park_ns   = 0;
			frames    = 0;
			return;
		}
		if (frames >= 120)
		{
			const double f  = double(frames);
			const double fr = double(wall - win_wall0) / f / 1e6;
			const double cp = double(cpu - win_cpu0) / f / 1e6;
			const double pk = double(park_ns) / f / 1e6;
			__android_log_print(ANDROID_LOG_INFO, "m2stall",
					"frame %.2f ms (%.1f fps) = cpu %.2f + park %.2f + other %.2f",
					fr, 1000.0 / fr, cp, pk, fr - cp - pk);
			win_wall0 = wall;
			win_cpu0  = cpu;
			park_ns   = 0;
			frames    = 0;
		}
	}
};

} // namespace m2vk

#else

namespace m2vk {
struct stall_meter
{
	void park_begin() { }
	void park_end() { }
};
} // namespace m2vk

#endif

#endif // MAME_OSD_LIBRETRO_M2_M2VK_STALLMETER_H
