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
	"Hardware Vulkan or MAME's software rasteriser. Reload to apply.";
inline constexpr char const *RENDERER_VULKAN   = "Vulkan (hardware)";
inline constexpr char const *RENDERER_SOFTWARE = "Software (MAME)";

// --- Internal Resolution -----------------------------------------------------
inline constexpr char const *RES_LABEL = "Internal Resolution";
inline constexpr char const *RES_INFO =
	"Render resolution. Vulkan only. Requires restart.";
// The three sizes that can be a hardware native (Model 2 / System 21 / System 22). Both the plain and
// the "(Native)" label are here because set_native_resolution() swaps between them at startup.
inline constexpr char const *RES_496x384_NATIVE = "496x384 (Native Model 2)";
// Model 1's visible area is the same 496x384; only the label's family name differs (set_native_resolution
// takes it as an override so the shared 496x384 entry can read "Model 1" on a Model 1 load).
inline constexpr char const *RES_496x384_NATIVE_M1 = "496x384 (Native Model 1)";
inline constexpr char const *RES_496x384        = "496x384";
inline constexpr char const *RES_496x480_NATIVE = "496x480 (Native System 21)";
inline constexpr char const *RES_496x480        = "496x480";
inline constexpr char const *RES_640x480_NATIVE = "640x480 (Native System 22)";
inline constexpr char const *RES_640x480        = "640x480";

// --- Transparency ------------------------------------------------------------
inline constexpr char const *TRANSPARENCY_LABEL = "Use Real Transparency";
inline constexpr char const *TRANSPARENCY_INFO =
	"Off uses original screen-door transparency effect; On is real transparency. Vulkan only.";

// --- Texture Filtering (System 22) -------------------------------------------
inline constexpr char const *S22_TEXTURE_FILTER_LABEL = "Texture Filtering";
inline constexpr char const *S22_TEXTURE_FILTER_INFO =
	"Add bilinear filtering to textures. System 22 only.";

// --- Fog (System 22) ---------------------------------------------------------
inline constexpr char const *S22_FOG_LABEL = "Fog";
inline constexpr char const *S22_FOG_INFO =
	"Enable or disable fog. System 22 only.";

// --- Flat Shaded (System 22) -------------------------------------------------
inline constexpr char const *S22_NO_TEXTURES_LABEL = "Flat Shaded";
inline constexpr char const *S22_NO_TEXTURES_INFO =
	"Draws untextured geometry lit by hardware shading.";

// --- Flat Shading (both renderers) -------------------------------------------
inline constexpr char const *FLAT_SHADING_LABEL = "Flat Shaded";
inline constexpr char const *FLAT_SHADING_INFO =
	"Draws untextured geometry lit by hardware shading.";
inline constexpr char const *FLAT_SHADING_UNTEXTURED = "Untextured";

// --- Unlit (both renderers) --------------------------------------------------
inline constexpr char const *FLAT_LUMA_LABEL = "Unlit";
inline constexpr char const *FLAT_LUMA_INFO =
	"Disable lighting for fullbright textures.";

// --- Polygon Count -----------------------------------------------------------
inline constexpr char const *POLY_COUNTER_LABEL = "Polygon Count";
inline constexpr char const *POLY_COUNTER_INFO =
	"Display polygons rendered per frame. Vulkan only.";

// --- 2D Overlay (System 22) --------------------------------------------------
inline constexpr char const *S22_2D_OVERLAY_LABEL = "2D Overlay (HUD)";
inline constexpr char const *S22_2D_OVERLAY_INFO =
	"Toggles 2D elements.";

// --- Steering Response -------------------------------------------------------
inline constexpr char const *STEERING_RESPONSE_LABEL = "Steering Response";
inline constexpr char const *STEERING_RESPONSE_INFO =
	"Adjust curve on steerin wheel input.";
// The five response-curve value names. These double as the STORED option values and are index-locked
// to STEERING_RESPONSE_GAMMA in retro_options.h — reword freely, but do not reorder.
inline constexpr char const *STEER_LINEAR_LABEL      = "Linear";
inline constexpr char const *STEER_SLIGHT_LABEL      = "Slight";
inline constexpr char const *STEER_MEDIUM_LABEL      = "Medium";
inline constexpr char const *STEER_STRONG_LABEL      = "Strong";
inline constexpr char const *STEER_VERY_STRONG_LABEL = "Very Strong";

// --- Steering Deadzone -------------------------------------------------------
inline constexpr char const *STEERING_DEADZONE_LABEL = "Steering Deadzone";
inline constexpr char const *STEERING_DEADZONE_INFO =
	"Adjust percentage of steering wheel dead zone.";
inline constexpr char const *PCT_0_OFF = "0% (off)";

// --- Steering Range ----------------------------------------------------------
inline constexpr char const *STEERING_RANGE_LABEL = "Steering Range";
inline constexpr char const *STEERING_RANGE_INFO =
	"Adjust how much of the full wheel input range is mapped to the stick.";
inline constexpr char const *PCT_100_FULL_LOCK = "100% (full lock)";

// --- Steering Damping --------------------------------------------------------
// Both damping options share these value labels.
inline constexpr char const *DAMP_0  = "0";
inline constexpr char const *DAMP_2  = "2f";
inline constexpr char const *DAMP_4  = "4f";
inline constexpr char const *DAMP_8  = "8f";
inline constexpr char const *DAMP_16 = "16f";

inline constexpr char const *STEERING_DAMP_DRIVE_LABEL = "Steering Damping (Turn)";
inline constexpr char const *STEERING_DAMP_DRIVE_INFO =
	"Adjust damping (in frames) when turning wheel. 0 is no effect.";

inline constexpr char const *STEERING_DAMP_RETURN_LABEL = "Steering Damping (Return)";
inline constexpr char const *STEERING_DAMP_RETURN_INFO =
	"Adjust damping (in frames) when controller is released and wheel snaps back to center. 0 is no effect.";

// --- Steering Visualizer -----------------------------------------------------
inline constexpr char const *STEERING_DISPLAY_LABEL = "Steering Visualizer";
inline constexpr char const *STEERING_DISPLAY_INFO =
	"On-screen display showing steering input vs. stick position.";

// --- Analog Deadzone ---------------------------------------------------------
inline constexpr char const *ANALOG_DEADZONE_LABEL = "Analog Deadzone";
inline constexpr char const *ANALOG_DEADZONE_INFO =
	"Adjust gamepad deadzone. Increase if you see jittering.";

// --- Analog Reach ------------------------------------------------------------
inline constexpr char const *ANALOG_REACH_LABEL = "Analog Reach";
inline constexpr char const *ANALOG_REACH_INFO =
	"Controls how much of the stick's analog movement is mapped to the game.";
inline constexpr char const *PCT_100_FULL_DEFLECTION = "100% (full deflection)";

// --- Diagnostic Input --------------------------------------------------------
inline constexpr char const *DIAGNOSTIC_INPUT_LABEL = "Diagnostic Input (Test Menu)";
inline constexpr char const *DIAGNOSTIC_INPUT_INFO =
	"Button combo that flips the TEST switch. Requires restart.";
// The three combo value names. These double as the STORED option values and are index-locked to the
// diagnostic_input enum in retro_options.h (the input module keys its combo table on it) — reword
// freely, but do not reorder. Names are RetroPad controls, not MAME button numbers.
inline constexpr char const *DIAG_NONE_LABEL        = "None";
inline constexpr char const *DIAG_L3_R3_LABEL       = "L3 + R3";
inline constexpr char const *DIAG_HOLD_SELECT_LABEL = "Hold Select";

} // namespace m2txt

#endif // MAME_OSD_LIBRETRO_M2_RETRO_OPTIONS_TEXT_H
