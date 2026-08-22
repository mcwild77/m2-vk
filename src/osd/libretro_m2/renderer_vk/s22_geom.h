// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco (Super) System 22 — the polygon pass (S2: untextured first).

    The System 22 analogue of vk_geom.h, deliberately much smaller. The seam in namcos22_v.cpp taps
    the quad stream (s22_seam.h) and, when set_gpu(true) has turned capture on, hands each projected
    quad to the record consumer here on the emulation thread. On the frontend's thread, inside
    retro_run, the record becomes vertex and index buffers and is rasterised on the GPU, over MAME's
    finished 2D frame — the software rasteriser having been switched off at the seam at the same time.

    Two things are settled and are the ones to understand before editing:

      * DEPTH IS DRAW ORDER, NOT z, as it is for Model 2 — but the System 22 tree is walked
        BACK-TO-FRONT (see s22_seam.h), the opposite of Model 2's stream, so the ordering is a plain
        painter's algorithm: draw in record order, last writer wins the pixel. That needs no depth
        buffer at all, so this pipeline disables the depth test and leaves the ring's depth attachment
        (which Model 2 needs) untouched.

      * UNTEXTURED FIRST. S2 lands the geometry before the texture tail. Every quad is drawn as a flat
        Gouraud-shaded polygon in its base palette colour (pens[0], resolved at the seam) modulated by
        the per-vertex brightness the hardware interpolates. Textured surfaces therefore come out as
        flat colour regions for now; the texel fetch is a later S2 step. An untextured polygon is
        exactly right; a textured one is the right shape and the right shading, the wrong fill.

    Like vk_geom, the record is turned into buffers on the frontend's thread from data the emulation
    thread wrote and is now parked against, and the buffers are written straight into device-local
    host-visible memory with no staging copy.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_S22_GEOM_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_S22_GEOM_H

#pragma once

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

#include <cstdint>

namespace s22 {

// The Vulkan function table and helpers live in namespace m2vk (the shared renderer); the System 22
// pass borrows the type here so its build signature reads the same as vk_geom's.
using m2vk::vk_funcs;

// Built and destroyed with the image ring, sharing its render pass and indexed by the same sync
// index. Stashes the handles; the pipeline and per-slot buffers are built lazily on the first upload
// that carries geometry, so the Model 2 build — which never captures — pays nothing for this file.
bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass render_pass, uint32_t slot_count);

// Destroys everything, making Vulkan calls; the caller must have waited for the device.
void geom_destroy();

// Drops every handle without calling Vulkan, for when the device is already gone.
void geom_forget();

// Turns the current record into this slot's buffers. Called on the frontend's thread after the slot's
// fence has retired. Returns whether there is anything for geom_draw() to do — false when nothing is
// capturing, when the record is empty, or when the pipeline could not be built.
bool geom_upload(uint32_t slot);

// Records the polygon pass. The caller has set the viewport and scissor to the attachment's extent
// and drawn the 2D background already; this draws the 3D over it. width/height are the VISIBLE extent
// (640x480) the vertex shader turns into NDC; draw_width/draw_height are the attachment's, unused here
// beyond the viewport the caller already set — there is no per-polygon scissor in the untextured pass.
void geom_draw(uint32_t slot, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height);

// The run is over. Resets the "reported once" latches so a second game reports its own numbers.
void geom_end_run();

} // namespace s22

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_S22_GEOM_H
