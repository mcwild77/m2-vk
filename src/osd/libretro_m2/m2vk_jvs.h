// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    M2VK_JVS_HLE — whether namcos23.cpp's timecrs2/timecrs2v4a/crszone default to the HLE JVS I/O
    board (namco_tssio_hle, bus/jvs/namcoio.cpp) instead of the real namco_tssio/namco_csz1 board,
    each of which interprets a Hitachi H8/3334 for no reason other than reading a light gun, a foot
    pedal, and coins. See devnotes/plan_system23optimization.md, Lever 1 / phase O2.

    Composition matches every other M2VK_* gate in this OSD (m2vk_baud, m2vk_snd): the core option
    (system23_jvs_hle, once it ships in the menu) seeds set_option_enabled() before the machine is
    built; the environment variable overrides it for the host harness. Where neither has spoken, the
    default is platform-dependent — ON for Android (the device this lever exists for), OFF elsewhere,
    so a host A/B against the real board stays the out-of-the-box comparison until the accuracy hand-
    check (numbered list, no scripted input) has passed.

    This header is deliberately free of MAME/emu includes — it is meant for the driver (namcos23.cpp,
    under its own S23VK guard) and the OSD to share without pulling either side's heavy headers.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_JVS_H
#define MAME_OSD_LIBRETRO_M2_M2VK_JVS_H

#pragma once

namespace m2vk_jvs {

// M2VK_JVS_HLE (env, overrides the option): 0 = off (real MCU board), 1 = on (HLE board). Resolved
// once per seed; see set_option_enabled().
bool tssio_hle_enabled();

// Force the resolved value, from the core option, before the machine starts — the namcos23.cpp config
// hooks read it when they choose set_default_option(). The environment variable still wins when set,
// same composition as m2vk_baud::set_option_enabled() / m2vk_snd::set_option_enabled().
void set_option_enabled(bool on);

} // namespace m2vk_jvs

#endif // MAME_OSD_LIBRETRO_M2_M2VK_JVS_H
