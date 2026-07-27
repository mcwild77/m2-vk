// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the lightgun reticle.

    MAME draws no crosshair here and cannot be made to. render_crosshair::draw adds a quad to the
    screen's render container (src/emu/crsshair.cpp), and this OSD reads pixels straight off
    screen->curbitmap() and composites the layers itself, so container primitives are never drawn by
    anything. PORT_CROSSHAIR on the six gun sets is dead weight. If the player is to see where the gun
    is pointing, this file is what draws it. devnotes/lightgun.md §1.5, §1.7.

    Three things live here and the split is deliberate:

      * WHERE it is. retro_run publishes one normalised position per gun port, from the same frontend
        poll the input module reads, while the emulation thread is parked on the baton. Normalised
        rather than in pixels because the publisher does not know the picture's size and the two
        consumers do — and because the Vulkan path may be drawing into an oversized attachment.
      * WHAT it looks like. The shape below is the whole asset: a gapped cross, generated from four
        numbers. Nothing is copied from anywhere — flycast's vmu_xhair.cpp is GPL-2.0-or-later and
        devnotes/legalstuff.md rests the release on there being no GPL-tagged object in the link, so
        a 16x16 bitmap is not worth borrowing when the geometry is four constants.
      * HOW it is drawn, which is twice, because the two renderers composite in different places.
        renderer=software hands MAME's finished frame straight to the frontend, so the reticle is
        blitted into it on the CPU (reticle_blit, below). renderer=vulkan composites two 2D layers
        and a polygon pass on the GPU and never sees a finished frame at all, so it draws the same
        cross as a scissored fullscreen triangle after the foreground layer (renderer_vk/reticle.frag).

    The two blitters are separate and that is accepted rather than worked around, but the ASSET is
    not duplicated: RETICLE_SHAPE below is the only place the geometry exists, and the shader is
    handed it in a push constant rather than carrying its own copy. What the shader does duplicate is
    the four-line predicate that turns those numbers into a pixel test — the same expression written
    once in C++ and once in GLSL, with each pointing at the other. A change to one that is not made
    to the other shows up as the two renderers disagreeing about a shape that is on screen the whole
    time, which is the cheapest kind of bug to notice.

    🚨 It must be invisible unless a port is actually set to a gun. Every accuracy fixture in
    devnotes/ab-baselines.md and devnotes/res-baselines.md differences against a background reference
    that both renderers produce bit-identically; a reticle drawn on a run that did not ask for one
    would move those pixels and quietly invalidate the whole harness. Nothing here draws until
    reticle_publish() is called with on = true, which happens only for a port whose libretro device is
    RETRO_DEVICE_LIGHTGUN. M2VK_NO_RETICLE=1 turns it off even then, which is what lets a gun game be
    put through ab.sh at all.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_RETICLE_H
#define MAME_OSD_LIBRETRO_M2_M2VK_RETICLE_H

#pragma once

#include <cstdint>

namespace m2vk {

// One per gun-capable port. Matches libretro_m2_input::MAX_GUNS, which is asserted where the two
// meet (retro_entry.cpp) rather than by including the input header here — nothing else in this file
// wants MAME or the input module, and the Vulkan side includes it.
enum : unsigned { RETICLE_MAX = 2 };


//============================================================
//  the shape
//============================================================

// The cross, measured in PICTURE pixels from its centre outwards. A gapped cross with 2 px arms
// 6 px long and a 4 px hole in the middle is 16x16 overall, which is the size every emulator's
// crosshair converges on because it is the smallest one that reads at 1x without hiding what is
// being aimed at.
//
// Everything is a distance from the centre, so the picture is symmetric by construction and there is
// no bitmap to get upside down. half_thick is a half-width because the arm straddles the centre line;
// gap and arm are the inner and outer ends of the arm along its own axis.
struct reticle_shape
{
	float half_thick;
	float gap;
	float arm;
	float outline;      // how far the dark border grows beyond all three of the above
};

constexpr reticle_shape RETICLE_SHAPE = { 1.0f, 2.0f, 8.0f, 1.0f };

// Is (dx, dy) — a signed offset from the centre, in picture pixels — inside the cross grown by
// `grow`? Called twice per pixel: once at 0 for the cross itself and once at RETICLE_SHAPE.outline
// for the border under it.
//
// ⚠️ renderer_vk/shaders/reticle.frag is the same four lines in GLSL, and there is no way to share
// them. Change both.
//
// The outline shrinks the gap as well as growing the arm, so the border wraps the arm's inner end
// instead of stopping short of it. It cannot fill the middle: at grow = 1 the horizontal arm still
// requires |dx| >= 1, and the centre pixel is half a pixel from the centre.
constexpr bool reticle_covers(float dx, float dy, float grow)
{
	const float ax = (dx < 0.0f) ? -dx : dx;
	const float ay = (dy < 0.0f) ? -dy : dy;
	const float t = RETICLE_SHAPE.half_thick + grow;
	const float g = RETICLE_SHAPE.gap - grow;
	const float a = RETICLE_SHAPE.arm + grow;
	return ((ay <= t) && (ax >= g) && (ax <= a))       // the horizontal arm
			|| ((ax <= t) && (ay >= g) && (ay <= a));  // the vertical one
}

// What the scissor and the blit's bounding box have to cover, in picture pixels either side of the
// centre. The +1 is the half-pixel each way that a centre landing between two pixels reaches.
constexpr float RETICLE_RADIUS = RETICLE_SHAPE.arm + RETICLE_SHAPE.outline + 1.0f;


//============================================================
//  where it is
//============================================================

struct reticle_state
{
	bool     on = false;
	float    x = 0.0f;      // normalised across the picture, 0..1, origin top left
	float    y = 0.0f;
	uint32_t colour = 0;    // 0x00RRGGBB
	uint32_t outline = 0;
};

// libretro thread, once per port per frame from retro_run, before the emulation thread is released.
// Both consumers read it afterwards — the software blit on the emulation thread, the Vulkan draw on
// this one — and neither can overlap with this, which is the same argument that already covers the
// input module's state.
//
// `on` false is the resting state and the only thing a run without a gun ever sets. Offscreen counts
// as off: the pointer is not on the picture, so drawing it pinned to a corner would be a lie about
// where the shot is going.
void reticle_publish(unsigned port, bool on, float x, float y);

// Content unloaded. Not merely tidiness: a second game in the same process must not inherit the
// first one's aim, and the frontend's port selection outlives the machine.
void reticle_end_run();

reticle_state const &reticle_get(unsigned port);

// Is there anything to draw at all? The one test both renderers make before doing any work, so that
// the 77 non-gun sets and every harness run pay a predicate and nothing else.
bool reticle_any();

// M2VK_NO_RETICLE=1 — drawn nowhere, however the ports are set. Read once, on first use.
//
// It exists for the accuracy harness: ab.sh and res.sh difference against an M2VK_NO_3D=1 background
// that both renderers have to produce bit-identically, and a reticle is by construction the one thing
// on screen that neither renderer's 3D path produced. Without this there would be no way to measure a
// gun game with a gun selected.
bool reticle_enabled();


//============================================================
//  the software path's blitter
//============================================================

// Draws every active reticle into a tightly packed width*height buffer of 0xAARRGGBB, which is what
// MAME's bitmap_rgb32 and the frontend's XRGB8888 both are. Opaque: the cross replaces what is under
// it and the border replaces what is under the border, exactly as the shader's discard does, so the
// two paths produce the same pixels rather than merely the same shape.
void reticle_blit(uint32_t *pixels, int width, int height);

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_RETICLE_H
