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


namespace m2vk {

// Draws this frame and hands the result to the frontend with set_image, building the ring first if
// it does not exist or no longer matches the geometry. Returns true if the frontend now has an
// image, which is the caller's cue to pass RETRO_HW_FRAME_BUFFER_VALID to video_cb; false means
// there is nothing to present and the frame should be duped.
//
// False is a normal answer, not only an error: context_reset does not fire until after
// retro_load_game has returned, so the first frame or two of every run have no context at all.
bool present_frame(unsigned width, unsigned height);

// Destroys the ring. Called from context_destroy — while the device is still alive — and again on
// unload in case the frontend never sent one. Idempotent, and safe to call with no ring.
void present_shutdown();

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_PRESENT_H
