// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "m2vk_reticle.h"

#include <cstdlib>

namespace m2vk {

namespace {

// Per port, and the two are told apart by colour because there is nothing else to tell them apart
// by: both players aim at the same picture and a vcop2 cabinet has two guns on it at once. White for
// player 1 and cyan for player 2, over a black border that is what makes either readable — a white
// cross on srallyc's sky or desert's sand is otherwise invisible, and every gun set has both bright
// and dark scenes in it. Cyan rather than yellow or red on purpose: vcop and vcop2 draw their OWN
// aiming reticle in yellow, and the whole value of ours during a test run is being able to tell the
// two apart in a screenshot.
//
// A per-port colour option is a nice-to-have and is folded into the queued core-options work rather
// than inventing a second options mechanism for it (devnotes/lightgun.md §3 step 4).
struct reticle_colours
{
	uint32_t colour;
	uint32_t outline;
};

constexpr reticle_colours COLOURS[RETICLE_MAX] = {
	{ 0xffffff, 0x000000 },
	{ 0x00e0ff, 0x000000 } };

reticle_state s_reticle[RETICLE_MAX];

bool s_enabled = true;
bool s_enabled_known = false;

} // anonymous namespace


void reticle_publish(unsigned port, bool on, float x, float y)
{
	if (port >= RETICLE_MAX)
		return;

	reticle_state &r = s_reticle[port];
	r.on = on;
	r.colour = COLOURS[port].colour;
	r.outline = COLOURS[port].outline;

	// The position is kept only while it is live, so that a port switched away from a gun leaves no
	// stale coordinate for a later frame to draw at.
	if (on)
	{
		r.x = x;
		r.y = y;
	}
}

void reticle_end_run()
{
	for (reticle_state &r : s_reticle)
		r = reticle_state{};
}

reticle_state const &reticle_get(unsigned port)
{
	static const reticle_state none;
	return (port < RETICLE_MAX) ? s_reticle[port] : none;
}

bool reticle_any()
{
	if (!reticle_enabled())
		return false;
	for (reticle_state const &r : s_reticle)
	{
		if (r.on)
			return true;
	}
	return false;
}

bool reticle_enabled()
{
	if (!s_enabled_known)
	{
		s_enabled = (std::getenv("M2VK_NO_RETICLE") == nullptr);
		s_enabled_known = true;
	}
	return s_enabled;
}


//============================================================
//  the software path's blitter
//============================================================

void reticle_blit(uint32_t *pixels, int width, int height)
{
	if ((pixels == nullptr) || (width <= 0) || (height <= 0) || !reticle_any())
		return;

	for (reticle_state const &r : s_reticle)
	{
		if (!r.on)
			continue;

		// Normalised to picture pixels. The centre is a position on the picture rather than a pixel
		// index — 0.5 lands on the boundary between the two middle columns, not on one of them — which
		// is why the loop below measures from each pixel's own centre at +0.5. It is also what makes a
		// 2 px arm come out 2 px wide instead of 3.
		const float cx = r.x * float(width);
		const float cy = r.y * float(height);

		int x0 = int(cx - RETICLE_RADIUS);
		int x1 = int(cx + RETICLE_RADIUS) + 1;
		int y0 = int(cy - RETICLE_RADIUS);
		int y1 = int(cy + RETICLE_RADIUS) + 1;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > width) x1 = width;
		if (y1 > height) y1 = height;

		const uint32_t colour = 0xff000000u | r.colour;
		const uint32_t outline = 0xff000000u | r.outline;

		for (int y = y0; y < y1; y++)
		{
			const float dy = (float(y) + 0.5f) - cy;
			uint32_t *const row = pixels + (size_t(y) * size_t(width));
			for (int x = x0; x < x1; x++)
			{
				const float dx = (float(x) + 0.5f) - cx;
				if (reticle_covers(dx, dy, 0.0f))
					row[x] = colour;
				else if (reticle_covers(dx, dy, RETICLE_SHAPE.outline))
					row[x] = outline;
			}
		}
	}
}

} // namespace m2vk
