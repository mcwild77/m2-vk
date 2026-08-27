// license:BSD-3-Clause
// copyright-holders:mcwild77
//
// All user-facing core-option TEXT in one place.
//
// Every string the frontend shows for a core option — the title (`desc`), the help paragraph
// (`info`), and the prose value labels — lives here, so wording can be edited without opening the
// option table in retro_options.cpp. Only display copy belongs here. The option keys, the STORED
// value strings ("vulkan", "5%", "496x384") and the defaults are logic, not copy, and stay in
// retro_options.cpp; likewise a bare numeric/percent/dimension label that merely echoes its value
// ("2%", "90%", "1024x768") is left inline there — it is not translatable text.
//
// To localise later: add a parallel set of these constants for another language and pick between the
// sets when the table is built. Nothing else in retro_options.cpp needs to change.

#ifndef MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_TEXT_H
#define MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_TEXT_H

namespace m2txt {

// Shared value labels — identical text reused by several options. The per-option ORDER of the
// values (which is On, which is Off first) stays in the table; this is only the wording.
inline constexpr char const *V_ON  = "On";
inline constexpr char const *V_OFF = "Off";

// --- 3D Renderer -------------------------------------------------------------
inline constexpr char const *RENDERER_LABEL = "3D Renderer";
inline constexpr char const *RENDERER_INFO =
	"How Model 2's 3D is drawn. Vulkan is why this core exists. Software is MAME's own "
	"rasteriser — slower, but it is the reference the Vulkan output is checked against, and it "
	"is the fallback on machines with no usable Vulkan driver. Applied when content is loaded.";
inline constexpr char const *RENDERER_VULKAN   = "Vulkan (hardware)";
inline constexpr char const *RENDERER_SOFTWARE = "Software (MAME)";

// --- Internal Resolution -----------------------------------------------------
inline constexpr char const *RES_LABEL = "Internal Resolution";
inline constexpr char const *RES_INFO =
	"The framebuffer the game is drawn into. Native is the hardware's own resolution; anything above "
	"it draws the same scene with more pixels, so polygon edges stop stair-stepping. Textures do not "
	"get sharper — the mip level comes from the game, not from the resolution. Costs memory and fill "
	"rate with the pixel count. Vulkan only; the software renderer always draws at native. Takes "
	"effect immediately.";
// The three sizes that can be a hardware native (Model 2 / System 21 / System 22). Both the plain and
// the "(Native)" label are here because set_native_resolution() swaps between them at startup.
inline constexpr char const *RES_496x384_NATIVE = "496x384 (Native)";
inline constexpr char const *RES_496x384        = "496x384";
inline constexpr char const *RES_496x480_NATIVE = "496x480 (Native)";
inline constexpr char const *RES_496x480        = "496x480";
inline constexpr char const *RES_640x480_NATIVE = "640x480 (Native)";
inline constexpr char const *RES_640x480        = "640x480";

// --- Transparency ------------------------------------------------------------
inline constexpr char const *TRANSPARENCY_LABEL = "Transparency";
inline constexpr char const *TRANSPARENCY_INFO =
	"How see-through surfaces are drawn. Model 2 has no alpha blender, so the hardware fakes them "
	"with a 50% screen door — a checkerboard of holes, which is what Screen Door reproduces. "
	"Blended draws them as real half-transparency instead: smoke, glass, shadows and headlight "
	"cones stop shimmering, at the cost of no longer matching the arcade. Vulkan only; the "
	"software renderer always uses the screen door. Takes effect immediately.";
inline constexpr char const *TRANSPARENCY_STIPPLE = "Screen Door (accurate)";
inline constexpr char const *TRANSPARENCY_BLENDED = "Blended";

// --- Texture Filtering (System 22) -------------------------------------------
inline constexpr char const *S22_TEXTURE_FILTER_LABEL = "Texture Filtering";
inline constexpr char const *S22_TEXTURE_FILTER_INFO =
	"Smooths the textures on the 3D polygons with a bilinear blend. System 22 sampled its textures "
	"one texel at a time, so distant and stretched surfaces show hard blocky steps; this softens "
	"them. It is an enhancement, not accuracy — off matches the arcade. Vulkan only; System 22 "
	"games only. Takes effect immediately.";

// --- Fog (System 22) ---------------------------------------------------------
inline constexpr char const *S22_FOG_LABEL = "Fog";
inline constexpr char const *S22_FOG_INFO =
	"Whether System 22 draws its distance fog. The hardware fades far polygons toward a fog colour — "
	"the haze over Ridge Racer's hills, the murk in the tunnels — and On reproduces it. Off removes "
	"the fog entirely, so the whole scene draws at full clarity; unlike the other System 22 options "
	"this is the reverse of accuracy, a look rather than a fix. Vulkan only; System 22 games only. "
	"Takes effect immediately.";

// --- Flat Shaded (System 22) -------------------------------------------------
inline constexpr char const *S22_NO_TEXTURES_LABEL = "Flat Shaded";
inline constexpr char const *S22_NO_TEXTURES_INFO =
	"Draws every 3D surface in plain white instead of its texture, lit by the hardware's own shading "
	"so the picture becomes a clean greyscale view of the geometry — the polygons and how they are lit, "
	"with none of the artwork. System 22 lights by brightness alone (there are no coloured lights), so "
	"the result is a true greyscale. A diagnostic look the arcade could not produce; off matches the "
	"arcade. Vulkan only; System 22 games only. Takes effect immediately.";

// --- Flat Shading (both renderers) -------------------------------------------
inline constexpr char const *FLAT_SHADING_LABEL = "Flat Shading";
inline constexpr char const *FLAT_SHADING_INFO =
	"Draws every polygon in its base colour with no texture, which is roughly what the geometry "
	"looked like on the workstations these games were modelled on. Acts on both renderers, so the "
	"software and Vulkan pictures stay comparable. Takes effect immediately.";
inline constexpr char const *FLAT_SHADING_UNTEXTURED = "Untextured";

// --- Unlit (both renderers) --------------------------------------------------
inline constexpr char const *FLAT_LUMA_LABEL = "Unlit";
inline constexpr char const *FLAT_LUMA_INFO =
	"Draws every surface at full brightness, so you get the texture and the polygon's own colour "
	"with nothing shaded onto them. Model 2 lights each face by how it is angled to the light, "
	"which is what makes cars darken as they turn and rooms fall into shadow; switching it off "
	"gives a flat, evenly lit picture that shows the artwork as it was drawn. Acts on both "
	"renderers, so the software and Vulkan pictures stay comparable. Takes effect immediately.";

// --- Polygon Count -----------------------------------------------------------
inline constexpr char const *POLY_COUNTER_LABEL = "Polygon Count";
inline constexpr char const *POLY_COUNTER_INFO =
	"Shows the number of 3D polygons the game is drawing each frame, as a small read-out in the "
	"top-right corner. A curiosity — watch it climb as a scene fills with cars or scenery. It counts "
	"what the hardware renderer is handed, so it only appears on the Vulkan renderer, not the software "
	"one. Takes effect immediately.";

// --- 2D Overlay (System 22) --------------------------------------------------
inline constexpr char const *S22_2D_OVERLAY_LABEL = "2D Overlay (HUD)";
inline constexpr char const *S22_2D_OVERLAY_INFO =
	"Whether the 2D HUD layer — the score, the lap counter, the on-screen text — is drawn over the 3D. "
	"On is how the game looks; Off hides that layer, leaving a clean view of the 3D scene over its 2D "
	"background, which is useful for screenshots. It does not touch the sky or the background artwork, "
	"only the foreground HUD. Vulkan only; System 22 games only. Takes effect immediately.";

// --- Steering Response -------------------------------------------------------
inline constexpr char const *STEERING_RESPONSE_LABEL = "Steering Response";
inline constexpr char const *STEERING_RESPONSE_INFO =
	"How stick movement maps to the wheel on the driving games. A real cabinet's wheel is most of "
	"a turn lock to lock and a thumbstick is about a centimetre, so mapping the two straight onto "
	"each other makes the car dart at the slightest touch — that is what Linear does, and it is "
	"what this core did before. The other settings keep full lock at full deflection but make the "
	"movement around centre finer, which is what lets you hold a line. Stronger is finer. Only "
	"affects games with a wheel; the fighting and gun games are untouched. Takes effect immediately.";

// --- Steering Deadzone -------------------------------------------------------
inline constexpr char const *STEERING_DEADZONE_LABEL = "Steering Deadzone";
inline constexpr char const *STEERING_DEADZONE_INFO =
	"How far the stick must move before the wheel does. Raise it if the car wanders on a straight "
	"with your thumb off the stick; a worn stick needs more than a new one. The travel it costs is "
	"given back to the rest of the sweep rather than thrown away, so a larger deadzone does not "
	"cost you lock. Only affects games with a wheel. Takes effect immediately.";
inline constexpr char const *PCT_0_OFF = "0% (off)";

// --- Steering Range ----------------------------------------------------------
inline constexpr char const *STEERING_RANGE_LABEL = "Steering Range";
inline constexpr char const *STEERING_RANGE_INFO =
	"How much of the wheel full stick deflection reaches. At 100% the stick's edge is full lock. "
	"Lowering it trades top-end lock for finer control everywhere, which suits the tracks that "
	"never ask for a hairpin — but if you cannot get round one, it is set too low. Only affects "
	"games with a wheel. Takes effect immediately.";
inline constexpr char const *PCT_100_FULL_LOCK = "100% (full lock)";

// --- Steering Damping --------------------------------------------------------
// Both damping options share these value labels.
inline constexpr char const *DAMP_0  = "0 (instant)";
inline constexpr char const *DAMP_2  = "2 frames";
inline constexpr char const *DAMP_4  = "4 frames";
inline constexpr char const *DAMP_8  = "8 frames";
inline constexpr char const *DAMP_16 = "16 frames (slow)";

inline constexpr char const *STEERING_DAMP_DRIVE_LABEL = "Steering Damping (Turn)";
inline constexpr char const *STEERING_DAMP_DRIVE_INFO =
	"How quickly the wheel follows the stick when you turn. A real cabinet's wheel has weight and "
	"cannot snap to full lock the way a thumbstick can, so the games — and the original emulator — "
	"ease it there over a few frames, which is a large part of why a stick feels twitchy without it. "
	"Lower is faster (fewer frames to full lock); 0 is instant, the old behaviour. Pair it with "
	"Return below. Only affects games with a wheel. Takes effect immediately.";

inline constexpr char const *STEERING_DAMP_RETURN_LABEL = "Steering Damping (Return)";
inline constexpr char const *STEERING_DAMP_RETURN_INFO =
	"How quickly the wheel recentres when you let go of the stick, as a self-centring wheel's spring "
	"does. Usually a little slower than Turn above — that asymmetry is what a real wheel feels like. "
	"Lower is faster (fewer frames back to centre); 0 is instant, the old behaviour. Only affects "
	"games with a wheel. Takes effect immediately.";

// --- Steering Visualizer -----------------------------------------------------
inline constexpr char const *STEERING_DISPLAY_LABEL = "Steering Visualizer";
inline constexpr char const *STEERING_DISPLAY_INFO =
	"Draws a bar across the top of the screen showing how much steering the game is actually "
	"receiving: red is the wheel you are not using, green is the wheel you are. A white notch "
	"shows where the stick itself is, so the gap between the notch and the end of the green is "
	"what the three settings above are doing. Only appears on games with a wheel. Meant for "
	"setting the steering up rather than for playing with it on. Takes effect immediately.";

// --- Analog Deadzone ---------------------------------------------------------
inline constexpr char const *ANALOG_DEADZONE_LABEL = "Analog Deadzone";
inline constexpr char const *ANALOG_DEADZONE_INFO =
	"How far the analog stick must move before the game responds, on the stick games (Star Blade, "
	"the twin-stick and flight sets). Raise it if your aim drifts with your thumb off the stick; a "
	"worn stick needs more than a new one. The travel it costs is given back to the rest of the "
	"sweep rather than thrown away. Only affects games with an analog stick; the wheel, gun and "
	"fighting games are untouched. Takes effect immediately.";

// --- Analog Reach ------------------------------------------------------------
inline constexpr char const *ANALOG_REACH_LABEL = "Analog Reach";
inline constexpr char const *ANALOG_REACH_INFO =
	"How far you must push the analog stick to reach full deflection, on the stick games. At 100% "
	"the very edge of the stick is full input; lowering it means full input arrives before the edge, "
	"so a stick that no longer reaches its corners can still peg the aim. Only affects games with an "
	"analog stick; the wheel, gun and fighting games are untouched. Takes effect immediately.";
inline constexpr char const *PCT_100_FULL_DEFLECTION = "100% (full deflection)";

// --- Diagnostic Input --------------------------------------------------------
inline constexpr char const *DIAGNOSTIC_INPUT_LABEL = "Diagnostic Input (Test Menu)";
inline constexpr char const *DIAGNOSTIC_INPUT_INFO =
	"Button combination that flips the cabinet's test switch, for player 1. This core draws none "
	"of MAME's menus, so with this set to None there is no way to reach a game's test mode or "
	"change its settings. The buttons a combination names are consumed by it, so the combo does "
	"not also fire its normal function; Hold Select wants about a second held down. Setting "
	"anything other than None also puts the service coin (a free credit) on L3 alone — L3 + R3 "
	"uses that same button as half its chord, so tapping L3 by itself still spends the credit "
	"while L3 + R3 together opens the test switch instead. Applied when content is loaded.";

} // namespace m2txt

#endif // MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_TEXT_H
