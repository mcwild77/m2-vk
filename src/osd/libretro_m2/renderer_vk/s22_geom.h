// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco (Super) System 22 — the polygon pass (S2b: textured).

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

      * TEXTURED. Each quad samples the tile system per fragment (s22.frag transliterates
        renderscanline_poly's texel fetch), or takes its flat base palette colour when the driver
        disabled textures; shading is per-pixel from the interpolated brightness. Fog, fade and poly
        alpha (the SS22 tail) are NOT applied yet — that is the next S2 step, so a fogged scene still
        differs from software.

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
// (640x480) the vertex shader turns into NDC AND the space the per-quad clip windows live in;
// draw_width/draw_height are the attachment's, used to scale the per-run scissor to it. One indexed
// draw per clip-window run (SS22 letterbox games window the 3D; see the scissor note in the .cpp); the
// scissor is restored to the full attachment before returning so the OVER overlay is not clipped.
void geom_draw(uint32_t slot, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height);

// The prioverchar over-pass (Super System 22 only). Redraws just the primitives flagged "priority over
// the text layer" (poly cmode&7==1, sprite cz==0xfe) a SECOND time, after the caller has drawn the OVER
// text overlay — so those primitives sit above the text, exactly as MAME's mixer priority 7 does. Draws
// nothing when there is no such primitive (every plain-S22 frame, most SS22 frames). Same args, same
// pipeline and buffers as geom_draw; restores the full attachment scissor before returning.
void geom_draw_over(uint32_t slot, VkCommandBuffer cmd, unsigned width, unsigned height,
		unsigned draw_width, unsigned draw_height);

// The run is over. Resets the "reported once" latches so a second game reports its own numbers.
void geom_end_run();

// system22_texture_filter: false = point sample (hardware-accurate), true = bilinear on the textured 3D
// poly tail (an enhancement — System 22 had no texture filter). A push-constant bit, so it applies on
// the next drawn frame with nothing to rebuild. M2VK_S22_FILTER overrides the option at draw time.
void set_option_filter(bool on);

// s22_depth_buffer: false = painter's algorithm in draw order (hardware-accurate — System 22 sorts, it
// has no depth buffer), true = per-pixel depth from interpolated 1/z, GREATER_OR_EQUAL, to resolve the
// overlap errors a single per-poly sort key cannot (ridge racer's road). An enhancement, not accuracy;
// reload-gated (baked into the pipeline). M2VK_S22_DEPTH overrides the option.
void set_option_depth(bool on);

// system22_fog: true = draw the hardware fog/z-fog (accurate default), false = skip every fog blend so
// the scene is unfogged. A push-constant flag bit, applies on the next drawn frame. M2VK_S22_FOG
// overrides the option (0 = force fog off, 1 = force it on).
void set_option_fog(bool on);

// system22_no_textures: true = replace every polygon surface with white so the per-pixel shade renders
// it as a greyscale lit view (geometry + lighting only). A push-constant flag bit. M2VK_S22_NOTEX
// overrides the option.
void set_option_no_textures(bool on);

// model2_flat_luma / "No Lighting" on the S22 path: true = skip the per-pixel shade, drawing every
// surface at full brightness. A push-constant flag bit. M2VK_FLAT_LUMA overrides the option (shared with
// the Model 2 sink so the switch behaves the same on both families).
void set_option_no_lighting(bool on);

// The recorded scene's primitive count (quads + sprite tiles), for the polygon-counter HUD.
uint32_t geom_primitive_count();

} // namespace s22

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_S22_GEOM_H
