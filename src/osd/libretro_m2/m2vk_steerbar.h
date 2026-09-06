// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — steering read-out bar.

    A bar across the top of the picture showing what the wheel is doing vs what the stick is doing.
    Green = port value the game receives; red = travel it does not; white notch = the raw stick.
    Off by default: nothing here draws and no port is read unless steering_display asked.

    Drawn twice — a CPU blit in software path, a scissored fullscreen triangle on Vulkan. The
    geometry and the part test are shared as constants and duplicated as code (GLSL cannot include
    C++). Divergence shows up as the two renderers disagreeing on screen, which is cheap to notice.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_STEERBAR_H
#define MAME_OSD_LIBRETRO_M2_M2VK_STEERBAR_H

#pragma once

#include <cstdint>

namespace m2vk {

struct steerbar_layout
{
	float width;        // of the picture
	float height;
	float top;          // gap above, fraction of picture height
	float border_u;     // frame thickness, fraction of bar half-width
	float border_v;     // ... and of its height
	float tick_half;    // half-width of raw-stick notch, in -1..1 bar units
	float centre_half;  // half-width of centre hairline, same units
};

constexpr steerbar_layout STEERBAR = { 0.90f, 0.05f, 0.008f, 0.004f, 0.09f, 0.012f, 0.006f };

enum steerbar_part : uint32_t
{
	STEERBAR_NONE = 0,
	STEERBAR_BORDER,
	STEERBAR_EMPTY,
	STEERBAR_FILL,
	STEERBAR_CENTRE,
	STEERBAR_TICK
};

// ⚠️ shaders/steerbar.frag is this function in GLSL — change both. Test order matters: tick sits
// over fill (full lock does not hide it) and over centre (centred stick shows the notch).
constexpr steerbar_part steerbar_part_at(float u, float v, float value, float raw)
{
	const float au = (u < 0.0f) ? -u : u;
	if ((au > 1.0f) || (v < 0.0f) || (v > 1.0f))
		return STEERBAR_NONE;
	if ((au > (1.0f - STEERBAR.border_u)) || (v < STEERBAR.border_v) || (v > (1.0f - STEERBAR.border_v)))
		return STEERBAR_BORDER;

	const float iu = u / (1.0f - STEERBAR.border_u);

	const float dt = (iu > raw) ? (iu - raw) : (raw - iu);
	if (dt <= STEERBAR.tick_half)
		return STEERBAR_TICK;

	// Same-sign check, not min/max: steering left must leave the right half red.
	const float aiu = (iu < 0.0f) ? -iu : iu;
	const float av = (value < 0.0f) ? -value : value;
	if (((iu < 0.0f) == (value < 0.0f)) && (aiu <= av))
		return STEERBAR_FILL;

	if (aiu <= STEERBAR.centre_half)
		return STEERBAR_CENTRE;

	return STEERBAR_EMPTY;
}

// 0x00RRGGBB, indexed by steerbar_part. Darkened well below full saturation because a pure 0xff0000
// band across daytona's sky reads as a fault.
constexpr uint32_t STEERBAR_COLOUR[] = {
	0x000000,   // NONE
	0x000000,   // BORDER
	0xb02020,   // EMPTY
	0x30c040,   // FILL
	0x101010,   // CENTRE
	0xffffff }; // TICK


struct steerbar_state
{
	bool  on    = false;
	float value = 0.0f;     // port value, -1..+1 about own centre
	float raw   = 0.0f;     // stick, -1..+1, before shaping
};

void steerbar_publish(bool on, float value, float raw);
void steerbar_end_run();
steerbar_state const &steerbar_get();
void set_option_steerbar(bool on);

// The one test m2vk_steer.h makes before reading the paddle port. M2VK_STEERBAR=0|1 overrides the
// option in both directions (the M2VK_BLEND discipline).
bool steerbar_on();

// Draws into a width*height buffer of 0xAARRGGBB. Opaque, so the two paths produce the same pixels.
void steerbar_blit(uint32_t *pixels, int width, int height);

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_STEERBAR_H
