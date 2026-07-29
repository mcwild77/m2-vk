// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Savestates — MAME's save_manager reached through libretro's three entry points.

    See devnotes/savestates.md. The short version of why this is thin: MAME already has exactly the
    calls libretro wants — save_manager::write_buffer / read_buffer (save.h:308) and
    ram_state::get_size (save.cpp:598), the latter being retro_serialize_size() with no guesswork —
    and no Model 2 set carrying MACHINE_SUPPORTS_SAVE does NOT disable any of them. That flag drives a
    UI warning, the -autosave load, and a fatalerror on devices that register nothing; do_write and
    do_read have no supported() check at all.

    Three properties this rests on, each measured rather than assumed:

      - The state's 32-byte header carries the system's short name and a CRC32 over the whole
        registry's *structure* (save.cpp:423, signature() at save.cpp:495). A state from another game,
        another build, or a build where a save_item was added is REJECTED before a byte is applied.
        The corruption failure mode is structurally unavailable for a mismatched state.

      - write_buffer's size check is an equality, not a minimum (save.cpp:367). A frontend handing us
        a larger buffer would fail, so everything goes through an owned exact-size vector and is
        memcpy'd out. Do not "simplify" this into passing the frontend's pointer through.

      - The save point is the libretro frame boundary, with the emulation thread parked on the baton
        in libretro_m2_osd_interface::update(). That is what makes calling these from the frontend
        thread safe, and it is also why the several-megabyte raster->poly_list and its array of raw
        POINTERS, poly_sorted_list, are not part of the state: geo_parse() rebuilds both from
        m_bufferram at every vblank (model2_v.cpp:2356 -> render_frame_start at 675), and the parked
        point sits after the frame's polygons were consumed and before the next vblank builds them.
        MAME cannot make that claim because it serialises at arbitrary scheduler points. We can,
        because we serialise at exactly one.

    ⚠ Include this AFTER emu.h — it names running_machine, which only emu.h brings in.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_SAVESTATE_H
#define MAME_OSD_LIBRETRO_M2_M2VK_SAVESTATE_H

#pragma once

#include <cstddef>

class running_machine;

namespace m2vk {

// Size of one serialised state, in bytes, cached on first call. 0 means the machine cannot produce
// one. Must be stable for the whole session, which it is: registration closes in
// save_manager::allow_registration(false) (save.cpp:82) as the machine starts, and retro_load_game
// blocks until the first frame, so every caller runs after it.
size_t state_size(running_machine &machine);

// Both return false rather than throwing, and log which save_error came back. size must be at least
// state_size(); the exact-size buffer is ours.
bool state_save(running_machine &machine, void *data, size_t size);
bool state_load(running_machine &machine, void const *data, size_t size);

// M2VK_SAVE_LOG=1 — the registry as MAME sees it, printed once. Registration count, the byte size and
// its breakdown, and save_manager::supported() with the reason it says what it says.
void state_log(running_machine &machine);

// The diagnostic switches, all read from the environment and all off by default. Between them they
// answer the three questions a failing savestate raises, in the order worth asking them:
//
//   M2VK_SAVE_VERIFY=1        Did the LOAD restore what it was handed? Serialises straight back out
//                             after read_buffer and names every entry that came back different. This
//                             separates "the load dropped X" from "the machine then ran differently",
//                             which a diff taken a frame later cannot do.
//   M2VK_SAVE_DIFF=<file>     Does this state match a reference taken by another run at the same
//                             emulated frame? Names the entries that differ.
//   M2VK_SAVE_DIFF_MAX=<n>    Raise the 40-entry print cap on either of the above. The timer entries
//                             sort late and are invisible under the default.
//   M2VK_SAVE_DUMP=<substr>   Print the serialised bytes of every entry whose name contains substr.
//                             The companion to the diff: it says what is IN an entry, which is what a
//                             question like "was the CPU mid-burst-stall when this was taken" needs.
//
// state_save also reports two preconditions MAME enforces on itself and libretro cannot: live
// anonymous timers, and non-empty FIFOs. Both are explained at the check in the .cpp.

// Drops the cached size. Called as the machine is torn down, so that a second content load in the
// same process cannot inherit the first machine's number.
void state_close();

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_SAVESTATE_H
