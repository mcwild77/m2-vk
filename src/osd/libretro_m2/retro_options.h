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

// Puts the cabinet's service coin and test switch on the stick clicks. Off by default.
inline constexpr char const *KEY_SERVICE_BUTTONS = "model2_service_buttons";

// Publish the option set to the frontend. Must be called from retro_set_environment(), which is
// where a frontend expects to find out about options — it is called before retro_init().
void declare(retro_environment_t environ_cb);

// The frontend's value for an option, or our declared default if the frontend has no value for it
// (or supports no options at all, which is the retrohost case).
std::string get(retro_environment_t environ_cb, char const *key);

// Same, for the enabled/disabled options.
bool get_bool(retro_environment_t environ_cb, char const *key);

// True once after the user changes anything; the frontend clears the flag as we read it.
bool updated(retro_environment_t environ_cb);

} // namespace m2opt

#endif // MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_H
