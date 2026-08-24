// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 21 — the polygon pass (T2: untextured, z-buffered).

    The System 21 analogue of s22_geom.h, and smaller still: S21 is untextured AND flat-shaded, so a
    quad is four screen-space corners, a per-quad depth and a per-quad palette pen. The seam
    (s21_seam.h) taps the quad stream and, when set_gpu(true) has turned capture on, hands each quad to
    the record consumer here on the emulation thread. On the frontend's thread, inside retro_run, the
    record becomes vertex/index buffers and is rasterised on the GPU, over MAME's finished 2D frame —
    the software rasteriser having been switched off at the seam at the same time.

    Two things are settled and are the ones to understand before editing:

      * DEPTH IS A REAL PER-QUAD z-BUFFER, unlike S22's painter's pass. renderscanline_flat tests/writes
        a single per-quad zsort, so the pipeline runs the depth test (COMPARE_GREATER, write on) against
        the ring's depth attachment, which is cleared to 0.0. s21.vert maps zsort to z = 1 - zsort/32768
        so nearer wins and coplanar ties fall to the first writer — exactly the software `zsort < zbuf`.

      * FLAT, CLUT-COLOURED. Each quad emits one palette pen; the fragment looks it up in the palette the
        driver hands the seam (m_palette->pens()), re-uploaded each frame. No texture, no shade, no fog.

    Like s22_geom, the record is turned into buffers on the frontend's thread from data the emulation
    thread wrote and is now parked against, written straight into device-local host-visible memory with
    no staging copy.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_S21_GEOM_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_S21_GEOM_H

#pragma once

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

#include <cstdint>

namespace s21 {

using m2vk::vk_funcs;

// Built and destroyed with the image ring, sharing its render pass and indexed by the same sync index.
// The pipeline and per-slot buffers are built lazily on the first upload that carries geometry, so a
// build that never captures pays nothing for this file.
bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass render_pass, uint32_t slot_count);

// Destroys everything, making Vulkan calls; the caller must have waited for the device.
void geom_destroy();

// Drops every handle without calling Vulkan, for when the device is already gone.
void geom_forget();

// Turns the current record into this slot's buffers. Called on the frontend's thread after the slot's
// fence has retired. Returns whether there is anything for geom_draw() to do.
bool geom_upload(uint32_t slot);

// Records the polygon pass. The caller has set the viewport and scissor to the attachment's extent and
// drawn the 2D background already; this draws the 3D over it, depth-tested. width/height are the VISIBLE
// extent (496x480) the vertex shader turns into NDC.
void geom_draw(uint32_t slot, VkCommandBuffer cmd, unsigned width, unsigned height);

// T2b: the pri1==4 layer-0 C355 z-mix, over the 3D and under the OVER overlay's high-priority band — the
// caller draws it there, between geom_draw and the OVER pass. A fullscreen pass (no vertex buffer, rides
// fullscreen.vert) that samples the captured tag/colour buffer s21_seam.h's capture_mix left and tests a
// per-pixel priority-bank threshold against the SAME depth attachment geom_draw just wrote — the only
// place the polygon z-buffer survives once T2a turns the software rasteriser off. No-op when the driver
// captured no mix buffer this frame (pri1 != 4, or the GPU does not own the 3D).
void geom_draw_mix(uint32_t slot, VkCommandBuffer cmd);

// The run is over. Resets the "reported once" latches so a second game reports its own numbers.
void geom_end_run();

} // namespace s21

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_S21_GEOM_H
