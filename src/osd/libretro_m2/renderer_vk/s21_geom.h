// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 21 — the polygon pass (T2: untextured, z-buffered), composited in PEN-index space.

    The System 21 analogue of s22_geom.h. S21 is untextured AND flat-shaded, so a quad is four
    screen-space corners, a per-quad depth and a per-quad palette pen. The seam (s21_seam.h) taps the
    quad stream and, when set_gpu(true) has turned capture on, hands each quad to the record consumer
    here on the emulation thread; the driver additionally hands over the 2D-under pens, the layer-0 mix
    and the OVER band. On the frontend's thread, inside retro_run, all of it is composited on the GPU
    over MAME's finished 2D frame — the software rasteriser having been switched off at the seam.

    OPTION B — the whole S21 frame is composited as palette PEN INDICES, not RGB. The reason is the C355
    palette-shadow sprites in the OVER band: sprite_mix_callback does not write a colour for them, it ORs
    a bank select onto the pen already beneath (dest = 0x4000|(dest&0x1fff) / 0x6000|...), and the
    polygon-blend palette banks 1/2 resolve that against the real scene. An RGB composite has thrown that
    pen away by the time the shadow is applied, so the shadow lands on a constant; only a pen-space
    composite keeps it. So the pipeline is:

      1. pen_pass() — a PRIVATE R16_UINT render pass (per slot), off to one side of the shared present
         pass. Three draws into the pen attachment: the 2D-under pens (fullscreen), the 3D quads
         (depth-tested — S21 z-buffers in hardware, the accurate model), and the layer-0 mix (fullscreen,
         gl_FragDepth thresholded against the same depth). Leaves the composited pen in the attachment.

      2. finish_draw() — one fullscreen draw INSIDE the shared present pass. Samples the pen attachment,
         applies the OVER band (opaque pens, and the palette-shadow banks against the composited pen),
         and resolves the final pen through the CLUT to RGB. This is the S21 background for the frame;
         the reticle/steering overlays (inert on the current S21 games) draw over it as usual.

    Depth is a REAL per-quad z-buffer (renderscanline_flat tests/writes a single per-quad zsort): the pen
    pass clears its depth to 0.0, the geometry pipeline tests COMPARE_GREATER and writes, and s21.vert
    maps zsort to z = 1 - zsort/32768 so nearer wins and coplanar ties fall to the first writer — exactly
    the software `zsort < zbuf`.

    Like s22_geom, host-visible device-local per-frame buffers are written with no staging copy, from data
    the emulation thread wrote and is now parked against, so there is no lock. The pen/depth images are
    device-local and sized to the draw extent (native, or larger under a raised internal resolution or
    M2VK_SS), rebuilt when that extent changes.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_S21_GEOM_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_S21_GEOM_H

#pragma once

#include "renderer_vk/vk_funcs.h"

#include "libretro_vulkan.h"

#include <cstdint>

namespace s21 {

using m2vk::vk_funcs;

// Built and destroyed with the image ring, sharing its (present) render pass and indexed by the same
// sync index. The pipelines and per-slot buffers are built lazily on the first frame that captures
// anything, so a build that never captures (model2 / namcos22) pays nothing for this file.
// present_render_pass is the shared pass finish_draw runs in; the private pen render pass is this file's.
bool geom_build(const retro_hw_render_interface_vulkan &iface, const vk_funcs &fns,
		VkRenderPass present_render_pass, uint32_t slot_count);

// Destroys everything, making Vulkan calls; the caller must have waited for the device.
void geom_destroy();

// Drops every handle without calling Vulkan, for when the device is already gone.
void geom_forget();

// Turns the current record and captured layers into this slot's buffers. Called on the frontend's thread
// after the slot's fence has retired. Returns whether S21 owns the frame — i.e. the driver captured the
// 2D-under this frame (GPU mode, not M2VK_NO_3D) — in which case the caller runs pen_pass + finish_draw.
bool geom_upload(uint32_t slot);

// The private pen pass. Recorded into the command buffer BEFORE the shared present render pass begins:
// it is its own render pass, into this slot's R16_UINT pen attachment, and leaves it in
// SHADER_READ_ONLY_OPTIMAL for finish_draw to sample. width/height are the draw extent (== the shared
// pass's extent this frame), which the pen/depth images are sized to.
void pen_pass(VkCommandBuffer cmd, uint32_t slot, unsigned width, unsigned height);

// The finish pass. Recorded INSIDE the shared present render pass, in place of the UNDER background draw:
// samples the pen attachment pen_pass just wrote, applies the OVER band, resolves to RGB. width/height
// are the draw extent (for the OVER buffer's native-to-draw coordinate map via the fullscreen v_uv).
void finish_draw(VkCommandBuffer cmd, uint32_t slot, unsigned width, unsigned height);

// The run is over. Resets the "reported once" latches so a second game reports its own numbers.
void geom_end_run();

} // namespace s21

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_S21_GEOM_H
