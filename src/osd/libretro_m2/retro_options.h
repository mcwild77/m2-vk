// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — core options.

    The whole of the core's user-facing configuration. Deliberately small: this is a single-driver
    core, MAME's own options are not exposed, and anything a frontend already does better (video
    scaling, audio latency, input remapping) is left to the frontend.

    Every option is read in retro_load_game(). All but two are then re-read by retro_run() when the
    frontend raises GET_VARIABLE_UPDATE, and apply on the next frame:

      model2_internal_res       the renderer compares it against the ring it built and rebuilds
      model2_flat_shading       a global the polygon seam reads, so the next frame simply sees it
      model2_flat_luma          the same, resolved per polygon as it crosses the seam
      model2_transparency       a global the polygon pass latches at the top of each upload
      model2_steering_deadzone  parked in m2vk::steer(), which the pad reads as it shapes the axis
      model2_steering_response  the same
      model2_steering_range     the same

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
#include "retro_options_text.h"

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

// Force every polygon's luma to full scale, which is what "no lighting" means at this seam. Model 2's
// lighting is real — a per-face diffuse and ambient term — but MAME's copro emulation has already
// collapsed it to one 8-bit number per polygon before anything reaches us, so there is no lighting
// stage here to switch off and flattening that number is the whole of it. Hence the key: it is named
// for what it does, per user-options.md §5, while the option a player reads is called No Lighting.
//
// Acts on BOTH renderers, like model2_flat_shading and unlike the two Vulkan-only options, because it
// removes a feature — see m2vk::FLAT_LUMA for what the flattened value leaves behind.
inline constexpr char const *KEY_FLAT_LUMA = "model2_flat_luma";

// How the `checker` polygon flag is drawn. Model 2 has no alpha blender: a translucent surface is a
// 50 % screen door, one pixel on and one off, and that is what the hardware and MAME both produce.
// "blended" replaces it with a real 50 % blend.
//
// ⚠️ An ENHANCEMENT, not an accuracy fix, and off by default for that reason. Vulkan only — MAME's
// rasteriser has nothing to blend with — so like model2_internal_res it cannot obey the
// both-renderers rule, and an A/B or resolution-invariance run must leave it alone.
inline constexpr char const *KEY_TRANSPARENCY = "model2_transparency";

// System 22 only — bilinear filtering on the 3D texture tail. The hardware point-sampled its textures,
// so this is an enhancement (off by default), and it is hidden from the Model 2 menu via hide_option()
// (its renderer does not read it). Vulkan only. s22::set_option_filter() parks it; M2VK_S22_FILTER wins.
inline constexpr char const *KEY_S22_TEXTURE_FILTER = "system22_texture_filter";

// System 22 only — a per-pixel depth buffer (interpolated 1/z) in place of the painter's algorithm.
// The hardware is a sorting rasteriser with no depth buffer, so this is an enhancement (off by default),
// hidden from the Model 2 menu via hide_option(). It resolves the overlap errors a single per-poly sort
// key leaves (ridge racer's road z-fighting). Vulkan only, and reload-gated — the depth state is baked
// into the pipeline. s22::set_option_depth() parks it; M2VK_S22_DEPTH wins.
inline constexpr char const *KEY_S22_DEPTH_BUFFER = "system22_depth_buffer";

// Draw a small HUD read-out of the 3D primitive count each frame, top-right. All three families; off by
// default. Vulkan only — it counts GPU-submitted primitives, so it is inert on the software renderer.
// m2vk::set_option_counter() parks it; M2VK_POLYCOUNT wins.
inline constexpr char const *KEY_POLY_COUNTER = "model2_poly_counter";

// Wall-clock frame-rate read-out in the top-left corner. On by default. Vulkan only.
// m2vk::set_option_fps() parks it; M2VK_FPS wins.
inline constexpr char const *KEY_FPS_DISPLAY = "model2_fps_display";

// Bilinear-filter the opaque 2D under-layer (background tilemaps) when Internal Resolution magnifies the
// picture above native. An enhancement, off by default; a no-op at native, where the layer is one texel
// per pixel. Under layer ONLY — the color-keyed foreground/HUD overlay keeps NEAREST, because bilinear
// breaks its exact pixel-0 transparency test and bleeds the key colour into glyph edges. Shown for
// Model 2 / System 22 / Model 1 / System 23 (the shared composite path); hidden from the System 21
// menu, whose background is composited in pen space with texelFetch and cannot read this sampler.
// Vulkan only. m2vk::set_option_smooth_2d() parks it; M2VK_SMOOTH_2D wins.
inline constexpr char const *KEY_SMOOTH_2D = "model2_smooth_2d";

// System 22 only — draw the hardware fog (default on, the accurate picture) or skip every fog blend.
// Unlike the two options above this is not an enhancement: fog is what the hardware does, so "on" is the
// accurate default and "off" is the debug view. Hidden from the Model 2 and System 21 menus (their seams
// do not read it). Vulkan only. s22::set_option_fog() parks it; M2VK_S22_FOG wins.
inline constexpr char const *KEY_S22_FOG = "system22_fog";

// System 22 only — replace every 3D surface with white so the per-pixel shade renders it greyscale
// (geometry + lighting only, no artwork). A debug view, off by default; distinct from Model 2's Flat
// Shading, which draws the polygon's own base colour. Hidden from the Model 2 and System 21 menus.
// Vulkan only. s22::set_option_no_textures() parks it; M2VK_S22_NOTEX wins.
inline constexpr char const *KEY_S22_NO_TEXTURES = "system22_no_textures";

// Model 1 only — smooth (Phong) shading. An enhancement, off by default: Model 1 is flat-shaded hardware,
// so this synthesises per-vertex normals and re-runs the lighting per pixel, adding a Blinn-Phong
// specular. Hidden from every other family's menu. Vulkan only. m1::set_option_smooth() parks it;
// M2VK_M1_SMOOTH wins. Applies live (a pipeline swap at draw time; no reload).
inline constexpr char const *KEY_SMOOTH_SHADING = "model1_smooth_shading";

// Model 2 only — smooth (Gouraud) shading. An enhancement, off by default: Model 2 bakes one flat luma
// per polygon, so this welds a per-vertex luma and interpolates it, removing the faceted luma banding on
// curved textured surfaces. Hidden from every other family's menu. Vulkan only. m2vk::set_option_smooth()
// parks it; M2VK_M2_SMOOTH wins. Applies live (a vertex value, no reload).
inline constexpr char const *KEY_M2_SMOOTH_SHADING = "model2_smooth_shading";

// Model 2 only (functional on the model2o / Model 1 sound-board / Daytona-class sets) — run the SEGAM1AUDIO
// board (the sound 68000) on its own worker thread, so a heavy frame's wall time approaches the slowest
// single device rather than the sum. A performance option, off by default; the accurate, single-threaded
// path is the default. Hidden from the other families' menus. Reload-gated — the board split is decided
// when the machine is built. m2vk_snd::set_option_enabled() seeds it; M2VK_SOUND_THREAD wins (harness).
inline constexpr char const *KEY_SOUND_THREAD = "model2_sound_thread";
inline constexpr char const *KEY_SELF_THROTTLE = "model2_self_throttle";
inline constexpr char const *KEY_DRIVE_BOARD = "model2_drive_board";

// System 22 only — whether the 2D HUD/text overlay is drawn back over the GPU 3D. On by default (the
// accurate picture); off hides the score/HUD/text layer, leaving the 3D above the 2D background. A
// look, not an accuracy fix. Hidden from the Model 2 and System 21 menus. Vulkan only.
// s22::set_option_hud() parks it; M2VK_S22_HUD wins.
inline constexpr char const *KEY_S22_2D_OVERLAY = "system22_2d_overlay";

// The three steering options — shape the left stick's X axis on the 30 GAME entries that declare
// an IPT_PADDLE. The machine is asked rather than a table consulted (m2vk_steer.h).
// ⚠️ Unlike every rendering option, the default is NOT the untouched path — linear-onto-a-thumbstick
// is the defect, not accuracy.
inline constexpr char const *KEY_STEERING_DEADZONE = "model2_steering_deadzone";
inline constexpr char const *KEY_STEERING_RESPONSE = "model2_steering_response";
inline constexpr char const *KEY_STEERING_RANGE    = "model2_steering_range";

// Steering damping — a rate limit on the shaped axis, in frames-to-full-lock. The official emulator
// applies one before the game reads the wheel (a self-centring wheel cannot snap), and a thumbstick
// that reaches lock in one frame feels twitchy without it. Two knobs because the reference is
// asymmetric: DRIVE is how fast the value follows the stick out, RETURN how fast it recentres when
// released. Both default Off (instant), which is the identity. m2vk_steer.h runs it.
inline constexpr char const *KEY_STEERING_DAMP_DRIVE  = "model2_steering_damp_drive";
inline constexpr char const *KEY_STEERING_DAMP_RETURN = "model2_steering_damp_return";

// Steering read-out bar. Off by default — a bar over the picture is pixels no fixture reference
// would have, so a run with it on would difference against a background that does not.
inline constexpr char const *KEY_STEERING_DISPLAY = "model2_steering_display";

// The two analog-stick options — shape the sticks on the sets that declare an IPT_AD_STICK (Star
// Blade, the twin-stick and flight sets), mutually exclusive with the wheel games. Deadzone is our
// own (default 5 %); reach is the deflection at which full output is reached (default 100 %). Like the
// steering options the default is NOT the untouched path — MAME's raw 15 %/85 % is the defect. The
// machine is asked (an IPT_AD_STICK field), not a table consulted (m2vk_analog.h).
inline constexpr char const *KEY_ANALOG_DEADZONE = "model2_analog_deadzone";
inline constexpr char const *KEY_ANALOG_REACH    = "model2_analog_reach";

// The values of that option. Declaration order, and the numbering is what the input module keys its
// combo table on — so the two lists cannot drift apart, because there is only one list.
enum diagnostic_input : unsigned
{
	DIAG_NONE = 0,
	DIAG_L3_R3,
	DIAG_HOLD_SELECT,
	DIAG_COUNT
};

// The names are RetroPad controls, not MAME button numbers: "A" is the pad's A button under either
// pad layout, and which MAME button that produces is the layout's business and not this option's.
inline constexpr char const *DIAGNOSTIC_VALUES[DIAG_COUNT] = {
	m2txt::DIAG_NONE_LABEL,
	m2txt::DIAG_L3_R3_LABEL,
	m2txt::DIAG_HOLD_SELECT_LABEL };

// KEY_STEERING_RESPONSE values and their gammas. One list, so nothing drifts. The curve is
// |v|^gamma — above 1, fine near centre, coarse near lock; full lock reachable at every setting.
enum steering_response : unsigned
{
	STEER_LINEAR = 0,
	STEER_SLIGHT,
	STEER_MEDIUM,
	STEER_STRONG,
	STEER_VERY_STRONG,
	STEER_RESPONSE_COUNT
};

inline constexpr char const *STEERING_RESPONSE_VALUES[STEER_RESPONSE_COUNT] = {
	m2txt::STEER_LINEAR_LABEL,
	m2txt::STEER_SLIGHT_LABEL,
	m2txt::STEER_MEDIUM_LABEL,
	m2txt::STEER_STRONG_LABEL,
	m2txt::STEER_VERY_STRONG_LABEL };

inline constexpr float STEERING_RESPONSE_GAMMA[STEER_RESPONSE_COUNT] = {
	1.0f, 1.3f, 1.7f, 2.2f, 3.0f };

// KEY_S22_TEXTURE_FILTER resolved to the bool s22::set_option_filter() takes. "on" tested rather than
// not "off", so an unreadable value lands on the accurate (point-sampled) picture.
bool get_s22_texture_filter(retro_environment_t environ_cb);

// KEY_S22_DEPTH_BUFFER resolved to the bool s22::set_option_depth() takes. "on" tested rather than not
// "off", so an unreadable value lands on the accurate (painter's) picture.
bool get_s22_depth_buffer(retro_environment_t environ_cb);

// KEY_S22_FOG resolved to the bool s22::set_option_fog() takes: true = draw fog. Here "off" is tested
// rather than "on", because fog is the accurate default — an unreadable value lands on fog ON.
bool get_s22_fog(retro_environment_t environ_cb);

// KEY_S22_NO_TEXTURES resolved to the bool s22::set_option_no_textures() takes. "on" tested rather than
// not "off", so an unreadable value lands on the accurate (textured) picture.
bool get_s22_no_textures(retro_environment_t environ_cb);

// KEY_S22_2D_OVERLAY resolved to the bool s22::set_option_hud() takes: true = draw the overlay. "off"
// tested rather than "on", because drawing the HUD is the accurate default — an unreadable value keeps it.
bool get_s22_2d_overlay(retro_environment_t environ_cb);

// Hide one option from every declared set, by key. Call before declare(). Used to keep a family's
// options off the other family's menu (the object files are shared across subtargets, so this is a
// runtime choice — see set_native_resolution()); a key that no entry matches is a silent no-op.
void hide_option(char const *key);

// Set which entry of the Internal Resolution option is the hardware's native size, before declare().
// Model 2's is 496x384 (the authored default); System 22's is 640x480; System 21's is 496x480.
// Retargets the option's default and moves the "(Native)" label onto the matching entry. `native` must
// be one of the option's value strings ("<w>x<h>"); anything else is ignored, leaving the Model 2
// default in place. The build's
// object files are shared across subtargets, so this cannot be a compile-time choice — retro_entry
// detects the driver family (driver_list) at retro_set_environment() time and calls this first.
// `native_label_496` overrides the "(Native)" text for the 496x384 entry (Model 1 and Model 2 share that
// size but want different family names); nullptr keeps the Model 2 label.
void set_native_resolution(char const *native, char const *native_label_496 = nullptr);

// Publish the option set to the frontend. Must be called from retro_set_environment(), which is
// where a frontend expects to find out about options — it is called before retro_init().
void declare(retro_environment_t environ_cb);

// Un-hide every option (s_hidden -> all false), so a fresh per-family gating pass starts from the full
// union. Paired with hide_option()+set_native_resolution() at load time in the merged (Modelizer) core:
// set_environment() cannot know the family before a set is chosen, so it declares the full union and the
// gating is (re-)applied here once family_of(system) is known.
void clear_hidden();

// Re-publish the option set after the gating has changed (clear_hidden() + hide_option() +
// set_native_resolution()). declare() caches the built definition table on first call; this rebuilds it
// and re-issues SET_CORE_OPTIONS, so the frontend's menu (visible subset, the Internal Resolution default
// and its "(Native)" label) matches the loaded family. A frontend that ignores a second declaration keeps
// the first; the render is unaffected either way (values read from DEFINITIONS[], not from the menu).
void redeclare(retro_environment_t environ_cb);

// Show or hide one already-declared option in the frontend's menu at runtime, by key
// (RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY). Unlike hide_option() — which filters the set BEFORE it is
// declared, at retro_set_environment() time when only the driver family is known — this runs after a game
// is loaded, so it can gate an option on something only the loaded machine knows (e.g. whether it has a
// wheel). Visibility only: a hidden option still reads its declared value, so a harness pin is untouched.
// A frontend that does not support the call ignores it; the option simply stays visible.
void set_option_display(retro_environment_t environ_cb, char const *key, bool visible);

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

// KEY_FLAT_LUMA resolved to the bool m2vk::set_option_flat_luma() takes. Tested against "on" rather
// than against the default, which is get_transparency()'s rule and for the same reason: a value we do
// not declare resolves to the accurate picture rather than to a guess.
bool get_flat_luma(retro_environment_t environ_cb);

// KEY_SMOOTH_SHADING resolved to the bool m1::set_option_smooth() takes. "on" tested, so an unreadable or
// invented value lands on the accurate flat default.
bool get_smooth_shading(retro_environment_t environ_cb);

// KEY_SOUND_THREAD resolved to the bool m2vk_snd::set_option_enabled() takes. "enabled" tested, so an
// unreadable or stale value lands on the accurate single-threaded default (off).
bool get_sound_thread(retro_environment_t environ_cb);

// KEY_SELF_THROTTLE resolved to whether retro_load_game passes -throttle (MAME paces itself) instead
// of -nothrottle (the frontend paces). "enabled" tested; the fallback default differs per platform —
// see the DEFINITIONS entry.
bool get_self_throttle(retro_environment_t environ_cb);

// KEY_DRIVE_BOARD resolved to whether the force-feedback drive-board Z80 runs. "enabled" tested, so
// an unreadable value lands on the accurate default (board running); the park is the opt-in.
bool get_drive_board(retro_environment_t environ_cb);

// KEY_M2_SMOOTH_SHADING resolved to the bool m2vk::set_option_smooth() takes. "on" tested, same reason.
bool get_m2_smooth_shading(retro_environment_t environ_cb);

// "on" tested rather than not "off": an unreadable value lands on the quiet answer.
bool get_steering_display(retro_environment_t environ_cb);

// KEY_POLY_COUNTER resolved to the bool m2vk::set_option_counter() takes. "on" tested, so an unreadable
// value leaves the counter off.
bool get_poly_counter(retro_environment_t environ_cb);

// KEY_FPS_DISPLAY resolved to the bool m2vk::set_option_fps() takes. Defaults on, so "off" is tested and
// an unreadable value leaves the read-out on.
bool get_fps_display(retro_environment_t environ_cb);

// KEY_SMOOTH_2D resolved to the bool m2vk::set_option_smooth_2d() takes. "on" tested, so an unreadable
// value lands on the accurate NEAREST default.
bool get_smooth_2d(retro_environment_t environ_cb);

// Anything unrecognised → accurate screen door.
unsigned get_transparency(retro_environment_t environ_cb);

// Unrecognised → the declared default (not Linear, since this option exists to move away from it).
unsigned get_steering_response(retro_environment_t environ_cb);

// Percentage strings parsed to fractions. Unparseable → the declared default.
float get_steering_deadzone(retro_environment_t environ_cb);
float get_steering_range(retro_environment_t environ_cb);

// Frame counts for the damping slew limiter — the string is a bare frame count, or "Off". Anything
// unrecognised (including "Off") → 0, which m2vk::set_option_steer_damping() reads as instant.
unsigned get_steering_damp_drive(retro_environment_t environ_cb);
unsigned get_steering_damp_return(retro_environment_t environ_cb);

// Analog-stick deadzone/reach parsed to fractions, for m2vk::set_option_analog(). Percentage strings;
// unparseable → the declared default (5 % / 100 %).
float get_analog_deadzone(retro_environment_t environ_cb);
float get_analog_reach(retro_environment_t environ_cb);

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
