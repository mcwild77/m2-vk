// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the Vulkan context's lifecycle.

    The frontend owns the VkInstance, the VkPhysicalDevice, the VkDevice and the queue; we are handed
    them through RETRO_HW_RENDER_INTERFACE_VULKAN and may submit on the shared queue under
    lock_queue/unlock_queue. This file is the whole of that arrangement: declare the context, take
    delivery of the interface at context_reset, drop it at context_destroy, and answer the one
    question the rest of the core asks — is there a device to draw with right now.

    Three rules that the code here exists to keep:

      * The interface is only valid after context_reset has fired, which is after retro_load_game
        has returned. So the renderer is guaranteed absent for at least the first frame or two, and
        the caller must be able to present nothing. retro_load_game must never wait for a context;
        that deadlocks.
      * context_destroy can arrive mid-run, and after it the handles are dead. This is safe against
        the emulator only because no Vulkan call is ever made from the emulation thread — the baton
        parks it inside update() for the whole of retro_run.
      * cache_context is false, so a reset always rebuilds from nothing. A second context_reset
        without an intervening context_destroy is legal and means exactly that.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_CONTEXT_H
#define MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_CONTEXT_H

#pragma once

#include "renderer_vk/vk_funcs.h"

#include "libretro.h"
#include "libretro_vulkan.h"


namespace m2vk {

// Asks the frontend for a Vulkan context and registers the two lifecycle callbacks. Called from
// retro_load_game, and only when the renderer option says vulkan.
//
// Returns false if the frontend cannot provide one, which is not an error: retrohost is exactly
// such a frontend, and so is RetroArch with a GL video driver. The caller falls back to presenting
// software frames and the core keeps running.
bool declare_hw_render(retro_environment_t environ_cb, retro_log_printf_t log_cb);

// Called from retro_unload_game. The frontend normally fires context_destroy first; this makes the
// state safe either way, and stops a context_reset arriving for a machine that no longer exists.
void forget_hw_render();

// True once context_reset has delivered a usable interface — the question retro_run asks before
// deciding whether it has a picture to present.
bool have_context();

// For the renderer proper, from step 3 on. Null until have_context().
const retro_hw_render_interface_vulkan *context_interface();
const vk_funcs &context_funcs();

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_RENDERER_VK_VK_CONTEXT_H
