// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "m2vk_steerbar.h"

#include <cstdlib>
#include <cstring>

namespace m2vk {

namespace {

steerbar_state s_bar;

bool s_option = false;      // model2_steering_display
bool s_switch_known = false;
bool s_switch_set = false;  // M2VK_STEERBAR was named...
bool s_switch_on = false;   // ...and said so

void read_switch()
{
	if (s_switch_known)
		return;
	s_switch_known = true;

	char const *const v = std::getenv("M2VK_STEERBAR");
	s_switch_set = (v != nullptr);
	// Same parse as every other valued switch here: present and non-"0" is on. A bare M2VK_STEERBAR=
	// is off, which is what a shell that always exports the variable produces.
	s_switch_on = s_switch_set && (v[0] != '\0') && (std::strcmp(v, "0") != 0);
}

} // anonymous namespace


void steerbar_publish(bool on, float value, float raw)
{
	s_bar.on = on;

	// Kept only while live, so a machine that stops steering — or a run that switches the option off
	// mid-lap — leaves no stale deflection for a later frame to draw.
	if (on)
	{
		s_bar.value = value;
		s_bar.raw = raw;
	}
}

void steerbar_end_run()
{
	s_bar = steerbar_state{};
}

steerbar_state const &steerbar_get()
{
	return s_bar;
}

void set_option_steerbar(bool on)
{
	s_option = on;
}

bool steerbar_on()
{
	read_switch();
	return s_switch_set ? s_switch_on : s_option;
}


//============================================================
//  the software path's blitter
//============================================================

void steerbar_blit(uint32_t *pixels, int width, int height)
{
	if ((pixels == nullptr) || (width <= 0) || (height <= 0))
		return;

	const steerbar_state &b = steerbar_get();
	if (!b.on || !steerbar_on())
		return;

	// The bar's rectangle in picture pixels. Everything is derived from the two fractions, so the
	// shader's arithmetic below is the same and there is no second layout.
	const float bw = STEERBAR.width * float(width);
	const float bh = STEERBAR.height * float(height);
	const float x0 = (float(width) - bw) * 0.5f;
	const float y0 = STEERBAR.top * float(height);

	int px0 = int(x0);
	int py0 = int(y0);
	int px1 = int(x0 + bw) + 1;
	int py1 = int(y0 + bh) + 1;
	if (px0 < 0) px0 = 0;
	if (py0 < 0) py0 = 0;
	if (px1 > width) px1 = width;
	if (py1 > height) py1 = height;
	if ((px1 <= px0) || (py1 <= py0))
		return;

	const float half = bw * 0.5f;
	const float cx = x0 + half;

	for (int y = py0; y < py1; y++)
	{
		// Each pixel's own centre, for the reason the reticle's blit measures from +0.5: the bar's
		// edges are positions on the picture rather than pixel indices, and measuring from the corner
		// makes a two-pixel border come out three.
		const float v = ((float(y) + 0.5f) - y0) / bh;
		uint32_t *const row = pixels + (size_t(y) * size_t(width));
		for (int x = px0; x < px1; x++)
		{
			const float u = ((float(x) + 0.5f) - cx) / half;
			const steerbar_part part = steerbar_part_at(u, v, b.value, b.raw);
			if (part != STEERBAR_NONE)
				row[x] = 0xff000000u | STEERBAR_COLOUR[part];
		}
	}
}

} // namespace m2vk
