// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 1 — the polygon pass (M1-2: untextured, painter's order).

    The Model 1 analogue of s22_geom.h / s21_geom.h, and the smallest of the three: Model 1 is
    untextured AND flat-shaded AND already colour-resolved at the seam, so a quad is four screen-space
    corners and one flat 0x00RRGGBB (+ a MOIRE stipple bit). There is no texture system, no palette
    buffer and no CLUT — the colour rides the vertex, so this pass needs no descriptor sets at all.

    The seam (m1_seam.h) taps the quad stream and, when set_gpu(true) has turned capture on, hands each
    quad to the record consumer here on the emulation thread. On the frontend's thread, inside retro_run,
    the record becomes vertex/index buffers and is rasterised over MAME's finished 2D frame — the
    software rasteriser having been switched off at the seam at the same time (draw_quads skips fill_quad).

    DEPTH IS DRAW ORDER, NOT z, as it is for Model 2. sort_quads has already sorted the frame back-to-front
    and the seam records in that order, so this is a plain painter's algorithm: draw in record order, last
    writer wins the pixel. That needs no depth buffer, so the pipeline disables the depth test and leaves
    the ring's depth attachment (which Model 2 needs) untouched. A z-buffer would z-fight the coplanar
    decals Model 1 resolves by submission order alone (model1_sortorder.md) — do not add one.

    Like s22_geom, per-frame buffers are host-visible device-local and written with no staging copy, from
    data the emulation thread wrote and is now parked against, so there is no lock. Everything is built
    lazily on the first captured frame, so a build that never captures Model 1 pays nothing for this file.

    M1-2 draws the 3D over the passthrough background only (no 2D-over overlay yet); the 2D-over HUD band
    is lifted back on top at M1-4, the S22-style OVER sandwich.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_M1_GEOM_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_M1_GEOM_H

#pragma once

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

#include <cstdint>

namespace m1 {

// The Vulkan function table and helpers live in namespace m2vk (the shared renderer); the Model 1 pass
// borrows the type here so its build signature reads the same as vk_geom's / s22_geom's.
using m2vk::vk_funcs;

// Built and destroyed with the image ring, sharing its render pass and indexed by the same sync index.
// The pipeline and per-slot buffers are built lazily on the first upload that carries geometry, so the
// model2 / namcos2x builds — which never capture Model 1 — pay nothing for this file.
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
// drawn the 2D background already; this draws the 3D over it. width/height are the VISIBLE extent the
// vertex shader turns into NDC; draw_width/draw_height are the attachment's, used to size the moiré
// stipple square so it stays one bitmap pixel wide at a raised internal resolution.
void geom_draw(uint32_t slot, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height);

// The run is over. Resets the "reported once" latches so a second game reports its own numbers.
void geom_end_run();

// The recorded frame's quad count, for the polygon-counter HUD.
uint32_t geom_primitive_count();

} // namespace m1

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_M1_GEOM_H
