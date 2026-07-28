// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — core options.

    The whole of the core's user-facing configuration. Deliberately small: this is a single-driver
    core, MAME's own options are not exposed, and anything a frontend already does better (video
    scaling, audio latency, input remapping) is left to the frontend.

    Every option is read in retro_load_game(). Two of the four are then re-read by retro_run() when
    the frontend raises GET_VARIABLE_UPDATE, and apply on the next frame:

      model2_internal_res    the renderer compares it against the ring it built and rebuilds
      model2_flat_shading    a global the polygon seam reads, so the next frame simply sees it

    The other two cannot be live and retro_run() says so rather than half-applying them:
    model2_renderer decides whether hardware render was declared at all, which happens before the
    machine starts, and model2_diagnostic_input is baked into the input assignments when the devices
    are configured.

    🚨 Anything a player is meant to *play* with belongs in the first group. Both of those shipped
    load-only on 2026-07-28 and it was reported as "the options do not work" the same day — which is
    the correct reading of a setting that changes nothing when you change it.

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

// The framebuffer the 3D is rendered into, and which the frontend is then handed. Vulkan only: MAME's
// rasteriser has no such thing to be asked for — bitmap_rgb32 is capped at 512x512, which is a large
// part of why the compositing moved to the GPU at P3 step 1 — so this is the one option in the set
// that cannot obey the both-renderers rule.
//
// 🚨 It used to render at a multiple of 496x384 and resolve back DOWN to 496x384, i.e. it was an
// antialiasing setting wearing an internal-resolution label. It is now the real thing: the picture the
// frontend receives is the size named in the value.
inline constexpr char const *KEY_INTERNAL_RES = "model2_internal_res";

// Draw every polygon flat and untextured. A diagnostic promoted to an option because it is also the
// most interesting thing this renderer can be asked to do that the arcade hardware could not; it acts
// on BOTH renderers, which is the standing rule for anything that removes a feature.
inline constexpr char const *KEY_FLAT_SHADING = "model2_flat_shading";

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

// KEY_INTERNAL_RES resolved to a framebuffer size. The value strings are "<w>x<h>" and that IS the
// parse — the value is the target size rather than a name for one, which is what lets a frontend or a
// harness name a resolution the menu does not list. Anything unparseable leaves both outputs at 0,
// which the caller reads as "native".
//
// ⚠️ The values were "1x".."4x" until 2026-07-28 and the comment in the table forbidding a resolution
// in the value was written for that: a multiplier's label would have lied if the picture size ever
// changed. The target size does not depend on the picture size, so it no longer applies.
void get_internal_size(retro_environment_t environ_cb, unsigned &width, unsigned &height);

// KEY_FLAT_SHADING resolved to the 0/1/2 m2vk::set_option_force_solid() takes. Only 0 and 2 are
// reachable from the option: mode 1 keeps translucency meaning "draw nothing", which leaves holes in
// the picture, so it stays a harness switch (M2VK_FORCE_SOLID=1) rather than something a player can
// pick by accident.
unsigned get_flat_shading(retro_environment_t environ_cb);

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
