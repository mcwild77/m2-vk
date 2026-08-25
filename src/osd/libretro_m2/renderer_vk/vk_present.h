// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the image ring, and the frame's submit.

    The frontend has a swapchain; we do not. What we own is a small ring of images that the frontend
    samples from, one per sync index, and the arrangement is entirely defined by three of its entry
    points:

      * get_sync_index_mask() says how many there may be — bit N set means get_sync_index() can
        return N — so the ring is sized from it rather than guessed at. RetroArch reports three on
        this machine, and the value can change (a fullscreen toggle changes swapchain length), at
        which point the spec guarantees the device is idle.
      * get_sync_index() says which slot this frame owns. Every per-frame resource — image, command
        pool, fence — is indexed by it and nothing is ever shared between slots.
      * wait_sync_index() blocks until the frontend has finished with that slot's image, including
        releasing queue-family ownership back to us.

    Two rules that are not visible in the code and are expensive to rediscover:

      * The queue belongs to the frontend. Every vkQueueSubmit sits between lock_queue and
        unlock_queue, and so does vkDeviceWaitIdle, which is a queue wait in disguise. Skipping this
        produces corruption that reads as a driver bug.
      * The retro_vulkan_image handed to set_image must stay alive until retro_video_refresh_t has
        returned, and the frontend may reuse the older pointer if a later frame is duped. So it
        lives in the slot, at a stable address, and is never a temporary.

    Everything here runs on the frontend's thread, inside retro_run or the two context callbacks.
    The emulation thread is parked on the baton throughout and never touches Vulkan.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_PRESENT_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_PRESENT_H

#pragma once

#include <cstdint>


namespace m2vk {

// Draws this frame and hands the result to the frontend with set_image, building the ring first if
// it does not exist or no longer matches the geometry. Returns true if the frontend now has an
// image, which is the caller's cue to pass RETRO_HW_FRAME_BUFFER_VALID to video_cb; false means
// there is nothing to present and the frame should be duped.
//
// `pixels` is MAME's finished software frame: width * height tightly packed 0xAARRGGBB words, the
// same buffer the software path hands straight to video_cb. It is read and copied before this
// returns, so the caller keeps ownership and may reuse it on the next frame.
//
// False is a normal answer, not only an error: context_reset does not fire until after
// retro_load_game has returned, so the first frame or two of every run have no context at all.
bool present_frame(const uint32_t *pixels, unsigned width, unsigned height);

// Destroys the ring. Called from context_destroy, which is the last moment at which the device is
// still alive, and again on unload in case the frontend never sent one. Idempotent, and safe to
// call with no ring — but not safe once the device is gone: use present_abandon() for that.
void present_shutdown();

// Drops the ring without making a single Vulkan call, for when the device it was built on may
// already be destroyed — a context_reset that arrived without a context_destroy before it. Leaks
// whatever that device still held, which is the correct trade: the alternative is destroying handles
// that no longer exist, and nothing observable from here can tell the two cases apart (MoltenVK
// recycles VkDevice handle values, so comparing them is not a liveness test).
void present_abandon();

// The run is over — content unloaded. Tears the ring down and resets the frame count that
// present_shutdown() deliberately preserves across a context loss.
void present_end_run();

// The core option model2_internal_res, resolved to a framebuffer size. Call it before the ring is
// built; retro_load_game does, and context_reset — which is where the size is actually latched,
// because it sizes every slot's attachments — does not fire until after that returns. 0x0 means
// "the hardware's own", which is what an unparseable option value resolves to.
//
// 🚨 M2VK_RES=<w>x<h> WINS when it is set, for the reason set_option_force_solid() gives: the harness
// sets the environment and must override whatever the frontend last remembered. M2VK_SS wins over
// both — it is a different feature (draw big, resolve back to native) and the accuracy harness is
// built on it, so a remembered resolution must not be able to disturb an ab.sh or res.sh run.
void set_option_resolution(unsigned width, unsigned height);

// poly_counter: draw a small HUD read-out of the primitive count the 3D pass submitted this frame, in
// the top-right corner. Off by default (an overlay is pixels no fixture reference has). Vulkan only —
// it counts GPU-submitted primitives, so it has no meaning on the software renderer. M2VK_POLYCOUNT
// overrides the option. Applies on the next presented frame; nothing to rebuild.
void set_option_counter(bool on);

// The extent of the image the frontend was last handed, which is NOT MAME's picture size once the
// resolution option is above native. False before the first successful present_frame(), when there is
// nothing to report and the caller should say nothing to the frontend either.
bool present_extent(unsigned &width, unsigned &height);

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_PRESENT_H
