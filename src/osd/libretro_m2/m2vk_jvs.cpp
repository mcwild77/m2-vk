// license:BSD-3-Clause
// copyright-holders:mcwild77
//
// M2VK_JVS_HLE — gate mechanism. Rationale is in m2vk_jvs.h; this file just resolves the value.

#include "m2vk_jvs.h"

#include <cstdlib>


namespace m2vk_jvs {

namespace {

int g_option = -1;   // -1 = not seeded, else 0/1 from the core option
int g_cached = -1;   // resolved value; -1 forces re-resolution on the next call

int mode()
{
	if (g_cached < 0)
	{
		char const *const env = std::getenv("M2VK_JVS_HLE");
		if (env && env[0] != '\0')
		{
			g_cached = (std::atoi(env) != 0) ? 1 : 0;
		}
		else if (g_option >= 0)
		{
			g_cached = g_option;
		}
		else
		{
			// No option has seeded this yet (e.g. a standalone OSD=sdl3 build, which has no core
			// options at all) and no override is set. Default ON for Android — the platform this
			// lever exists for — OFF everywhere else, so a host A/B keeps comparing against the real
			// MCU board until the HLE board's accuracy hand-check has passed.
#if defined(__ANDROID__)
			g_cached = 1;
#else
			g_cached = 0;
#endif
		}
	}
	return g_cached;
}

} // anonymous namespace


void set_option_enabled(bool on)
{
	// Seeded from retro_load_game before the machine is built, so namcos23.cpp's config hooks read the
	// resolved value when they decide which JVS board option to default to. Re-seeding clears the
	// cache so a reload with a changed option takes effect; the environment variable still wins.
	g_option = on ? 1 : 0;
	g_cached = -1;
}

bool tssio_hle_enabled()
{
	return mode() != 0;
}

} // namespace m2vk_jvs
