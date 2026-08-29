// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 23 — the polygon pass (23-3: textured, painter's order).

    The System 23 analogue of s22_geom.h / m1_geom.h. The seam (s23_seam.h) taps the sorted primitive
    stream render_flush walks and, when set_gpu(true) has turned capture on, hands each polygon to the
    record consumer here on the emulation thread. On the frontend's thread, inside retro_run, the record
    becomes vertex/index buffers and is rasterised over MAME's finished 2D frame — the software rasteriser
    having been switched off at the seam at the same time (render_flush skips its 64-way software dispatch).

    Two things are settled and are the ones to understand before editing:

      * DEPTH IS DRAW ORDER, NOT z, as it is for System 22 and Model 2. render_flush qsorts the frame by
        the 24-bit zkey and the seam records in that back-to-front order, so this is a plain painter's
        pass: draw in record order, last writer wins. No depth buffer, so the pipeline disables the depth
        test and leaves the ring's depth attachment (Model 2's) untouched.

      * TEXTURED. System 23 has no untextured path — every render_scanline pixel is a texture fetch — so
        23-3 transliterates namcos23_renderer::texture_lookup into s23.frag: the tile system (tmrom_decoded
        / texattr_decoded / texrom) and the palette are uploaded as storage buffers (the ROM-derived three
        once, the palette per frame), addressed through one descriptor set, and the per-pixel SHADE step is
        applied on top. This draws the hash-0 corner only; the shading tail render_hash selects (stencil /
        poly-fade / colour-fade / blend / poly-alpha) is 23-4, ignored here.

    Sprites and 2D-over compositing are out of scope here (23-5/23-6): sprite-flagged entries are skipped,
    and the 3D draws over the whole 2D background without the priority-map sandwich.

    Like s22_geom / m1_geom, per-frame buffers are host-visible device-local and written with no staging
    copy, from data the emulation thread wrote and is now parked against, so there is no lock. Everything
    is built lazily on the first captured frame, so a build that never captures System 23 pays nothing.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_S23_GEOM_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_S23_GEOM_H

#pragma once

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

#include <cstdint>

namespace s23 {

using m2vk::vk_funcs;

// Built and destroyed with the image ring, sharing its render pass and indexed by the same sync index.
// The pipeline and per-slot buffers are built lazily on the first upload that carries geometry, so a
// build that never captures System 23 pays nothing for this file.
bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass render_pass, uint32_t slot_count);

// Destroys everything, making Vulkan calls; the caller must have waited for the device.
void geom_destroy();

// Drops every handle without calling Vulkan, for when the device is already gone.
void geom_forget();

// Turns the current record into this slot's buffers. Called on the frontend's thread after the slot's
// fence has retired. Returns whether there is anything for geom_draw() to do — false when nothing is
// capturing, the record is empty, or the pipeline could not be built.
bool geom_upload(uint32_t slot);

// Records the polygon pass. The caller has set the viewport and scissor to the attachment's extent and
// drawn the 2D background already; this draws the 3D over it. width/height are the VISIBLE extent
// (640x480) the vertex shader turns into NDC AND the space the per-poly viewport windows live in;
// draw_width/draw_height are the attachment's, used to scale the per-run scissor to it. One indexed draw
// per viewport-window run; the scissor is restored to the full attachment before returning.
void geom_draw(uint32_t slot, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height);

// The run is over. Resets the "reported once" latches so a second game reports its own numbers.
void geom_end_run();

// The recorded frame's primitive count, for the polygon-counter HUD.
uint32_t geom_primitive_count();

} // namespace s23

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_S23_GEOM_H
