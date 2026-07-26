// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the polygon pass. This is where the GPU first draws.

    Everything up to here has been passthrough: MAME's software rasterizer drew the 3D and Vulkan
    blitted the finished picture. From here the frame record's polygon stream becomes vertex and
    index buffers and is rasterized on the GPU, in the middle of the sandwich the two 2D tilemap
    layers make (m2vk_frame.h). MAME's scanline rasterizer is switched off at the seam at the same
    time, which is also the phase's only performance win and a large one.

    Three decisions here are settled and are the ones to understand before editing:

      * DEPTH IS DRAW ORDER, NOT z. Model 2 has no depth buffer. The software renderer draws
        front-to-back with an occlusion mask (m_fillmap) and writes a pixel only if nothing has
        written it yet — first writer wins, and that is the whole hidden-surface algorithm. Polygon
        n in draw order gets the constant depth 1 - n/65536 with a GREATER test and depth writes on,
        which is exactly the same thing in hardware. It reproduces the hardware's real behaviour (a
        priority sort, not a depth sort), and it cannot z-fight — 86 % of polygons in a sampled VF2
        frame share a sort bucket with a neighbour, so interpolated z would put coplanar fighting
        over most of the frame. Interpolated depth and the decal problem are P4, together, on
        purpose. vkCmdSetDepthBias is P4's tool and must not be reached for here.

      * NOTHING BLENDS. Model 2's translucency is a cutout and `checker` is a stipple; both are
        per-pixel discards and every surviving fragment is opaque. There is no sorted transparent
        pass and there does not need to be one — the stream arrives already priority-ordered.

      * THE RECORD IS TURNED INTO BUFFERS ON THE FRONTEND'S THREAD. The polygon stream arrives on
        the emulation thread and not one Vulkan call may be made there. So this file's upload runs
        inside retro_run, from data the emulation thread wrote and is now parked against.

    What this draws is both opaque paths: draw_scanline_solid's colour chain and the checker stipple,
    and — since step 4 — draw_scanline_tex, which is the filtered texel, the mip chain, the
    microtexture blend and the lumaram tail. Textured *translucent* polygons are counted and skipped;
    the cutout is step 5. An untextured translucent polygon draws nothing at all, as in the software
    renderer, so it is dropped at upload rather than discarded in the shader.

    Buffers are per sync index and are written directly rather than staged: this device's memory
    type 1 is device-local and host-visible both (unified memory), so a staging copy would buy
    nothing. The per-frame three grow to the run's high-water mark and are never shrunk, so a run
    that has been going for a second does no allocation at all.

    Texture RAM is a fourth, fixed at 2 MB — both 1 MB sheets, raw, exactly as they sit in the
    machine's memory shares. There is no atlas, no cache and no page decode, because at 2 MB total
    there is nothing to be gained by working out which part of it a frame wants. It is copied from
    the live shares on this thread, which is safe for the same reason everything else here is: the
    emulation thread is parked on the baton for all of retro_run.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_GEOM_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_GEOM_H

#pragma once

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

#include <cstdint>


namespace m2vk {

struct frame_record;

// The depth format. D24_UNORM_S8_UINT does not exist on Apple GPUs at all — its optimalTilingFeatures
// is literally zero on both hosts measured — so the reflex desktop choice is not available. See
// devnotes/vulkan-target.md; D32_SFLOAT holds a 16-bit draw-order key with enormous margin.
extern const VkFormat GEOM_DEPTH_FORMAT;

// Built and destroyed with the image ring, because it shares the ring's render pass and is indexed
// by the same sync index. The render pass must already have a depth attachment.
bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass render_pass, uint32_t slot_count);

// Destroys everything, making Vulkan calls; the caller must have waited for the device.
void geom_destroy();

// Drops every handle without calling Vulkan, for when the device is already gone.
void geom_forget();

// Turns the record into this slot's buffers. Called on the frontend's thread, after the slot's
// fence has retired, so the buffers this touches are not in flight. Returns whether there is
// anything for geom_draw() to do — false when the hardware path does not own the 3D, when the
// emulator has produced no geometry, or when every polygon in the frame took a path not drawn yet.
bool geom_upload(uint32_t slot, frame_record const &record);

// Records the polygon pass, inside the ring's render pass and between the two 2D layer draws. The
// caller has already set the viewport and scissor to the visible extent, which is what makes
// gl_FragCoord equal the software renderer's x/scanline.
void geom_draw(uint32_t slot, VkCommandBuffer cmd, unsigned width, unsigned height);

// The run is over. Resets the "reported once" latches so a second game reports its own numbers.
void geom_end_run();

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_GEOM_H
