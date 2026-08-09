// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the steering read-out bar.

    A bar across the top of the picture showing what the WHEEL is doing, as opposed to what the thumb
    is doing. Off by default, turned on by model2_steering_display.

    It exists because devnotes/steering-curve.md's shaping pipeline is invisible: the three options
    change how much lock a given thumb position produces, and the only way to see that today is
    M2VK_STEER_LOG on a terminal, which is a number scrolling past while both hands are on a pad. The
    hand-check (devnotes/steering-handcheck.md) asks a player to compare five response curves by feel
    alone. This draws the same number the read-out prints, in the place they are already looking.

    WHAT IT DRAWS, and this is the decision worth not undoing: the green bar is the RESOLVED IPT_PADDLE
    PORT VALUE, normalised about its own centre — the number the driver reads, after our shaping, after
    MAME's analog_field::apply_settings and after PORT_MINMAX scaling. Not the stick, and not the
    shaped axis either. Both of those are upstream of things that can still change the answer, and
    "what percentage is actually going into the game" is the question the bar exists to answer. The raw
    stick is drawn too, as a one-notch tick, precisely so the GAP between the two is visible — that gap
    is the curve, and watching it open and close as the response option changes is the whole point.

    Structured the same way as m2vk_reticle.h, for the same reasons, and the comments there apply:

      * WHERE the numbers come from. steer_frame() publishes once per emulated frame on the emulation
        thread, from m2vk_steer.h, which is the only place that holds both the pad's sample and a
        pointer to the paddle field. Normalised, because the publisher does not know the picture's
        size and both consumers do.
      * WHAT it looks like. The layout below is the whole asset — six fractions and five colours, no
        bitmap, no font.
      * HOW it is drawn, which is twice, because the two renderers composite in different places:
        a CPU blit into MAME's finished frame for renderer=software (steerbar_blit), and a scissored
        fullscreen triangle after the foreground layer for renderer=vulkan
        (renderer_vk/shaders/steerbar.frag).

    The two blitters are separate; the GEOMETRY and the PART TEST are not. steerbar_part_at() below is
    written once in C++ and once in GLSL — the same duplication the reticle accepts, with the same
    justification, that a divergence shows up immediately as the two renderers disagreeing about
    something on screen the whole time.

    🚨 OFF BY DEFAULT, and that is load-bearing rather than a taste. Every fixture in
    devnotes/ab-baselines.md and devnotes/res-baselines.md differences against a background reference
    that both renderers must produce bit-identically, and a bar drawn across the top of the picture is
    by construction pixels neither renderer's 3D path produced. Nothing here draws, and NO PORT IS
    READ, unless the option asked for it — see steerbar_on().

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_STEERBAR_H
#define MAME_OSD_LIBRETRO_M2_M2VK_STEERBAR_H

#pragma once

#include <cstdint>

namespace m2vk {

//============================================================
//  the shape
//============================================================

// Everything as a fraction, so the bar is the same size relative to the picture at every internal
// resolution and there is nothing to scale by hand. Unlike the reticle — which is a shape and takes
// ONE scale factor so its arms stay square on a 4:3 attachment holding a 1.29:1 picture — this is a
// screen-space panel and takes both. Stretching with the picture is what it is supposed to do.
struct steerbar_layout
{
	float width;        // of the picture, centred horizontally
	float height;       // of the picture. The user asked for about 5 %.
	float top;          // gap above it, as a fraction of picture height
	float border_u;     // frame thickness, as a fraction of the bar's half-width
	float border_v;     // ... and of its height
	float tick_half;    // half-width of the raw-stick notch, in -1..1 bar units
	float centre_half;  // half-width of the centre hairline, same units
};

// 90 % wide and 5 % tall at the very top. The border is thin in u and fat in v because those
// fractions are of very different quantities: 0.004 of a 223 px half-width is one pixel, 0.09 of a
// 19 px bar is not quite two.
constexpr steerbar_layout STEERBAR = { 0.90f, 0.05f, 0.008f, 0.004f, 0.09f, 0.012f, 0.006f };

enum steerbar_part : uint32_t
{
	STEERBAR_NONE = 0,  // outside the bar entirely — the shader discards, the blitter skips
	STEERBAR_BORDER,
	STEERBAR_EMPTY,     // travel not being used. The user's "all red when there is no input".
	STEERBAR_FILL,      // travel the game is actually receiving
	STEERBAR_CENTRE,    // the hairline at dead ahead
	STEERBAR_TICK       // where the stick is, before shaping
};

// What is at (u, v) — u across the whole bar in -1..1, v down it in 0..1. `value` and `raw` are both
// in -1..1 and are the port value and the stick.
//
// ⚠️ renderer_vk/shaders/steerbar.frag is this function in GLSL and there is no way to share it.
// Change both. The ORDER of the tests is part of it: the tick sits over the fill so that full lock
// does not hide it, and the centre hairline sits under the tick so that a centred stick shows the
// notch rather than the hairline.
constexpr steerbar_part steerbar_part_at(float u, float v, float value, float raw)
{
	const float au = (u < 0.0f) ? -u : u;
	if ((au > 1.0f) || (v < 0.0f) || (v > 1.0f))
		return STEERBAR_NONE;
	if ((au > (1.0f - STEERBAR.border_u)) || (v < STEERBAR.border_v) || (v > (1.0f - STEERBAR.border_v)))
		return STEERBAR_BORDER;

	// Inside the frame, rescaled so the fill still reaches the ends at full lock.
	const float iu = u / (1.0f - STEERBAR.border_u);

	const float dt = (iu > raw) ? (iu - raw) : (raw - iu);
	if (dt <= STEERBAR.tick_half)
		return STEERBAR_TICK;

	// The fill grows from the centre outwards, towards the side being steered. Written as an unsigned
	// comparison of the same sign rather than as min/max, because a value and a u of opposite signs
	// must never fill — steering left has to leave the right half red.
	const float aiu = (iu < 0.0f) ? -iu : iu;
	const float av = (value < 0.0f) ? -value : value;
	const bool same_side = ((iu < 0.0f) == (value < 0.0f));
	if (same_side && (aiu <= av))
		return STEERBAR_FILL;

	if (aiu <= STEERBAR.centre_half)
		return STEERBAR_CENTRE;

	return STEERBAR_EMPTY;
}

// 0x00RRGGBB, one per part, indexed by steerbar_part. The shader is handed these rather than carrying
// its own copy, so this is the only place they exist.
//
// Red for unused travel is the specification; green for used travel goes with it. Both are darkened
// well below full saturation because the bar sits over the game and a pure 0xff0000 band across the
// top of Daytona's sky reads as a fault rather than as an instrument.
constexpr uint32_t STEERBAR_COLOUR[] = {
	0x000000,   // NONE, never used
	0x000000,   // BORDER
	0xb02020,   // EMPTY
	0x30c040,   // FILL
	0x101010,   // CENTRE
	0xffffff }; // TICK


//============================================================
//  where it is
//============================================================

struct steerbar_state
{
	bool  on    = false;    // the machine steers and the option asked for it
	float value = 0.0f;     // the port value, -1..+1 about its own centre
	float raw   = 0.0f;     // the stick, -1..+1, before shaping
};

// Emulation thread, once per emulated frame, from m2vk::steer_frame(). Both consumers read it
// afterwards — the software blit on that same thread a few lines later, the Vulkan draw on the
// libretro thread while this one is parked on the frame baton — so the two are never live at once.
// The identical argument already covers the input module's state and the reticle's.
void steerbar_publish(bool on, float value, float raw);

// Content unloaded. The next machine may not steer, and a stale bar would outlive the one that did.
void steerbar_end_run();

steerbar_state const &steerbar_get();

// The option, parked from the libretro thread at load and again on every live change — the bar has to
// be switchable while driving for the same reason the curve does.
void set_option_steerbar(bool on);

// Is the bar wanted at all? The one test m2vk_steer.h makes before reading the paddle port, which is
// what keeps a default run's port traffic exactly as it was.
//
// M2VK_STEERBAR=0|1 overrides the option, in both directions — it takes a VALUE rather than being a
// presence, which is the M2VK_BLEND discipline: a harness run has to be able to pin the bar OFF
// against a remembered .opt file that asks for it, or an interactive session can rewrite a baseline.
bool steerbar_on();


//============================================================
//  the software path's blitter
//============================================================

// Draws the bar into a tightly packed width*height buffer of 0xAARRGGBB, which is what MAME's
// bitmap_rgb32 and the frontend's XRGB8888 both are. Opaque, exactly as the shader is, so the two
// paths produce the same pixels and not merely the same picture.
void steerbar_blit(uint32_t *pixels, int width, int height);

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_STEERBAR_H
