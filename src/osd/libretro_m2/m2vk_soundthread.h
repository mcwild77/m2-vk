// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    M2VK_SOUND_THREAD — run the Sega Model 1 sound board (SEGAM1AUDIO: 68000 + YM3438 + 2x MultiPCM)
    on its own host thread, a frame behind the main emulation.

    Why (measured): on a Quest 3 the Model 2 emulation is CPU-bound and the per-device profile is flat;
    the sound 68000 (:m1audio:sndcpu / :audiocpu) is the single largest device in two of four profiled
    games and never below #2. Parallelising it is the lever. See devnotes/m1audio-thread-plan.md.

    Shape of the split (Stage 1, this module):

      * The board is NOT instantiated in the main machine when threading is on. Instead it lives in a
        second, headless running_machine hosted here and stepped on a worker thread, ~1 frame behind.
      * The one wire between the boards is the bidirectional i8251<->i8251 serial link. Both machines
        share the same emulated-time origin (both start at t=0), so each direction is a time-tagged
        transition queue replayed on the RECEIVING machine's own scheduler — exact bit framing is
        preserved, only the absolute phase shifts by the ~1-frame lag. This is the Stage-0 delay line
        (segam1audio.cpp) generalised across threads. See serial_line below.
      * The board's mixed stereo audio (the whole of the audio for these games) lands in a ring the
        main OSD pulls in place of the now-silent main-machine mix.

    Gate: M2VK_SOUND_THREAD (env / core option), read once. Default OFF, so one binary A/Bs cleanly and
    the shipping path is byte-identical to before. When OFF, none of this is reached — enabled() is
    false and the model2.cpp hook takes the untouched branch.

    Threading rules (who touches what):
      * MAIN (emulation) thread: enabled(), main_txd() (from the main UART's txd_handler), start()/stop()
        and pump_main() (from the OSD, once per frame while the frontend is parked).
      * WORKER thread: the entire second machine, its scheduler, its stub OSD, and the sound-side serial
        replay. Owned wholly here; nothing else reaches into it.
      * FRONTEND thread (emulation parked): the audio pull (pull_audio) and the savestate calls. The
        audio ring is mutex-guarded because the worker writes it concurrently.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_SOUNDTHREAD_H
#define MAME_OSD_LIBRETRO_M2_M2VK_SOUNDTHREAD_H

#pragma once

#include <cstddef>
#include <cstdint>

// Forward declarations only — this header is included by the driver (model2.cpp) and the OSD, neither
// of which should pull the module's MAME-heavy internals. The .cpp has emu.h.
class running_machine;
class i8251_device;
class attotime;

namespace m2vk_snd {

// Read once, on the first call, from the environment variable M2VK_SOUND_THREAD (non-zero => on). The
// core option resolves to the same variable in retro_options.cpp, matching the M2VK_* wins-over-option
// rule the rest of the options follow. Cheap and idempotent; safe from any thread after the first call.
bool enabled();

// Force the resolved value, from the core option, before the machine starts. The environment variable
// still wins when set (harness override), same composition as set_option_flat_luma() and friends.
void set_option_enabled(bool on);

// Declare that a host capable of running the worker (the libretro OSD) is present. enabled() stays false
// until this is called, so the model2.cpp hook never removes the board from the main machine in a build
// whose OSD does not host the worker (the plain SDL binary) — there it would just drop sound. Called
// once, from the libretro OSD, before the machine is built.
void engage_host();

//============================================================
//  the main-machine hook (model2.cpp, main/emulation thread)
//============================================================

// The main UART's TXD line (main -> sound). Called for every bit transition from the main i8251's
// txd_handler, tagged with the main machine's absolute emulated time. Enqueued for the worker to
// replay on the sound board's UART RXD at the same absolute time (the worker lags, so it is always in
// the worker's future). No-op until the worker is up.
void main_txd(const attotime &t, int state);

// The main UART pointer (tag "uart") and the ROM regions are found by prepare_main()/start(), so nothing
// else in the driver hook has to hand them over.

//============================================================
//  lifecycle (OSD, main/emulation thread)
//============================================================

// Allocate the sound->main serial timer on the main machine while its save-state registration is still
// open — i.e. from the OSD's init(), which runs inside running_machine::start(). Allocating it later
// (in start(), below) is refused by MAME ("register after registration closed"). Also decides whether
// this machine is a split model2o at all, by looking for the marker speaker the threaded config branch
// adds; no-op for every other machine. Must be called for every machine the OSD starts.
void prepare_main(running_machine &main);

// Bring up the second machine and the worker thread. Captures the main machine's m1audio:* ROM region
// bytes (filled into the worker's identical regions before its CPU runs) and the main i8251 pointer
// (so sound->main replies can be delivered). Called from the OSD once the main machine is RUNNING and
// only for a split model2o machine. Idempotent.
void start(running_machine &main);

// Tear the worker and its machine down. Called from the OSD as the main machine exits. Idempotent.
void stop();

// True once start() has brought a threaded run up and it is producing audio. Read from the OSD to
// decide whether to present the worker's audio ring instead of the (silent) main-machine mix.
bool running();

//============================================================
//  per-frame (OSD update(), main/emulation thread)
//============================================================

// Publish the main machine's current time (paces the worker: it never advances past this) and deliver
// any pending sound->main serial replies onto the main UART's RXD, framing preserved. Called once per
// emulated frame from the OSD's update(), on the main thread, while the frontend is parked.
void pump_main(const attotime &main_time);

//============================================================
//  audio (OSD frame_audio(), frontend thread, emulation parked)
//============================================================

// Pull up to the ring's contents (bounded to keep latency near a frame) as interleaved stereo int16.
// Returns a pointer valid until the next pull_audio()/audio_consumed(); sets sample_frames to the
// number of stereo frames. Returns nullptr / 0 when nothing is queued.
const int16_t *pull_audio(int &sample_frames);

// Mark the last pull_audio() batch as consumed (drops it from the ring). Split from pull_audio so the
// OSD can hand the pointer straight to the frontend and only then drop it.
void audio_consumed();

//============================================================
//  savestate (frontend thread, emulation parked)
//============================================================
//
// The worker machine holds sound state the main machine no longer does, so a threaded savestate must
// round-trip both. These park the worker at its pacing point, (de)serialise its machine, and release
// it. Return 0/false when not running. See m2vk_soundthread.cpp.

size_t state_size();
bool state_save(void *dst, size_t size);
bool state_load(const void *src, size_t size);

} // namespace m2vk_snd

#endif // MAME_OSD_LIBRETRO_M2_M2VK_SOUNDTHREAD_H
