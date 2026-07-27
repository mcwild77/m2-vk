// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — core options.

    The whole of the core's user-facing configuration. Deliberately small: this is a single-driver
    core, MAME's own options are not exposed, and anything a frontend already does better (video
    scaling, audio latency, input remapping) is left to the frontend.

    Every option here is read once, in retro_load_game(), because everything they touch is fixed for
    the life of a machine — the renderer is chosen before the first frame and the input assignments
    are baked in when the input devices are configured. retro_run() notices a later change and says
    so rather than half-applying it.

    Three declaration paths are supported. Modern frontends take RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2;
    the v1 and the pre-option "variables" forms are derived from the same table so an older frontend
    still gets the options rather than silently getting none.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_H
#define MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_H

#pragma once

#include "libretro.h"

#include <string>


namespace m2opt {

// The 3D renderer. "vulkan" is the default because it is the point of this core; "software" is
// MAME's own rasteriser, which is both the A/B reference the Vulkan path is measured against and
// the fallback where Vulkan is unavailable.
inline constexpr char const *KEY_RENDERER = "model2_renderer";

// The button combination that flips the cabinet's test switch, and with it whether the service coin
// is on L3 at all. "None" by default. FBNeo's fbneo-diagnostic-input, value list unchanged, because
// a player arriving from any other libretro arcade core should find the same words in the same order.
inline constexpr char const *KEY_DIAGNOSTIC_INPUT = "model2_diagnostic_input";

// The values of that option. Declaration order, and the numbering is what the input module keys its
// combo table on — so the two lists cannot drift apart, because there is only one list.
enum diagnostic_input : unsigned
{
	DIAG_NONE = 0,
	DIAG_HOLD_START,
	DIAG_START_AB,
	DIAG_HOLD_START_AB,
	DIAG_START_LR,
	DIAG_HOLD_START_LR,
	DIAG_HOLD_SELECT,
	DIAG_SELECT_AB,
	DIAG_HOLD_SELECT_AB,
	DIAG_SELECT_LR,
	DIAG_HOLD_SELECT_LR,
	DIAG_COUNT
};

// The names are RetroPad controls, not MAME button numbers: "A" is the pad's A button under either
// pad layout, and which MAME button that produces is the layout's business and not this option's.
inline constexpr char const *DIAGNOSTIC_VALUES[DIAG_COUNT] = {
	"None",
	"Hold Start",
	"Start + A + B",
	"Hold Start + A + B",
	"Start + L + R",
	"Hold Start + L + R",
	"Hold Select",
	"Select + A + B",
	"Hold Select + A + B",
	"Select + L + R",
	"Hold Select + L + R" };

// Publish the option set to the frontend. Must be called from retro_set_environment(), which is
// where a frontend expects to find out about options — it is called before retro_init().
void declare(retro_environment_t environ_cb);

// The frontend's value for an option, or our declared default if the frontend has no value for it
// (or supports no options at all, which is the retrohost case).
std::string get(retro_environment_t environ_cb, char const *key);

// KEY_DIAGNOSTIC_INPUT resolved to its position in DIAGNOSTIC_VALUES. A value the frontend invented,
// or one left over from an older option set, resolves to DIAG_NONE rather than to a guess — losing a
// test-menu combo is recoverable from the options menu, an unasked-for one is not.
unsigned get_diagnostic(retro_environment_t environ_cb);

// True once after the user changes anything; the frontend clears the flag as we read it.
bool updated(retro_environment_t environ_cb);

} // namespace m2opt

#endif // MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_H
