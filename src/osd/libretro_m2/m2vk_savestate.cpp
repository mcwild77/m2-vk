// license:BSD-3-Clause
// copyright-holders:mcwild77
//
// Savestates. See m2vk_savestate.h for why this is thin and what it rests on.

#include "emu.h"

#include "m2vk_savestate.h"

#include "drawgfx.h"
#include "screen.h"
#include "tilemap.h"

#include "machine/gen_fifo.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

namespace m2vk {

namespace {

// Cached size, and the buffer save/load stage through. Both are touched only from the frontend
// thread with the emulation thread parked, which is the same discipline that makes the input
// snapshot and the reticle publish safe (retro_entry.cpp, retro_run).
size_t              s_size = 0;       // what the frontend is told: MAME's size plus our trailer
size_t              s_mame_size = 0;  // what read_buffer/write_buffer demand, exactly
bool                s_size_known = false;
std::vector<uint8_t> s_buffer;

// --- the FIFO trailer -------------------------------------------------------------------------
//
// 🚨 MAME does not save a generic_fifo's CONTENTS. gen_fifo.cpp:54 registers m_empty_triggered and
// m_full_triggered and says, in as many words, "This is not saving the fifo, let's hope it's
// empty...". On Model 2 those two FIFOs carry the geometry command stream between the i960 and the
// TGP/SHARC, so that hope is load-bearing and it is not met. Measured 2026-07-29, at save point 1500
// over the eight fixtures:
//
//   - desert (4 words) and lastbrnx (6) are the ONLY two whose copro_fifo_in is non-empty when the
//     state is taken, and they are two of the three fixtures that fail. Their words are dropped.
//   - vcop2's own FIFO is empty at save, but the machine LOADING the state has 6, 7 or 8 words in
//     its copro_fifo_in, and those survive the load untouched because nothing in the registry
//     covers them. Loading one state at eight consecutive frames gave exactly two outcomes and they
//     partition perfectly on that occupancy: 6 words -> one digest, 7 or 8 -> the other, 8 of 8.
//
// So the same omission bites in both directions — words lost on the way out, and the receiver's
// stale words left behind on the way in — and one fix closes both. The contents ride in a trailer
// appended to MAME's buffer rather than in the registry, because the registry is MAME's and this
// costs no upstream line; peek()/size()/clear()/push() are all public.
constexpr uint32_t FIFO_TRAILER_MAGIC = 0x4d32464f; // 'M2FO'
constexpr size_t   FIFO_TRAILER_MAX   = 64;        // per FIFO; the hardware ones hold 8 and 16

// Fixed-width so that state_size() is answerable before anything is serialised: a count and a
// fixed slab per FIFO, in device-enumeration order.
constexpr size_t FIFO_RECORD_BYTES = sizeof(uint32_t) * (1 + FIFO_TRAILER_MAX);

size_t fifo_count(running_machine &machine)
{
	size_t n = 0;
	for (generic_fifo_u32_device &fifo : device_type_enumerator<generic_fifo_u32_device>(machine.root_device()))
	{
		(void)fifo;
		n++;
	}
	return n;
}

size_t fifo_trailer_bytes(running_machine &machine)
{
	const size_t n = fifo_count(machine);
	return (n == 0) ? 0 : (sizeof(uint32_t) * 2) + (n * FIFO_RECORD_BYTES);
}

// How many times update() has been reached, i.e. how many frames the frontend has been handed. Only
// the probe reads it; see the note where it is printed for why it is worth having.
long s_update_count = 0;

bool env_flag(char const *name)
{
	char const *const v = std::getenv(name);
	return (v != nullptr) && (v[0] != '\0') && (v[0] != '0');
}

size_t diff_cap()
{
	char const *const v = std::getenv("M2VK_SAVE_DIFF_MAX");
	if ((v == nullptr) || (v[0] == '\0'))
		return 40;
	const long n = std::strtol(v, nullptr, 10);
	return (n > 0) ? size_t(n) : SIZE_MAX;
}

char const *error_text(save_error err)
{
	switch (err)
	{
	case STATERR_NONE:           return "none";
	case STATERR_NOT_FOUND:      return "not found";
	case STATERR_INVALID_HEADER: return "invalid header (wrong game, or a build with a different save registry)";
	case STATERR_READ_ERROR:     return "read error";
	case STATERR_WRITE_ERROR:    return "write error";
	case STATERR_DISABLED:       return "disabled";
	}
	return "unknown";
}

} // anonymous namespace


size_t state_size(running_machine &machine)
{
	if (!s_size_known)
	{
		// 🚨 Do not cache a size while the registry is still being filled. registration_allowed() is
		// the exact test — save_manager::allow_registration(false) (machine.cpp:306) is what closes
		// it, and it is also what computes m_supported and the rewinder's capacity, i.e. the moment
		// MAME itself considers the registry final. Any proxy for this (a frame count, a phase) is a
		// guess; this is the condition itself.
		//
		// This is not hypothetical caution: the first version of this instrument cached at the first
		// update() and reported 15 entries / 189 bytes for vf2 — the machine-level bookkeeping
		// registered in running_machine::start() before start_all_devices(), and nothing else.
		if (machine.save().registration_allowed())
			return 0;

		// ram_state::get_size is HEADER_SIZE plus the sum over the registry, which is exactly what
		// write_buffer will demand back (save.cpp:598, save.cpp:367) — so it is kept separately from
		// the number the frontend is told, which also covers the FIFO trailer.
		s_mame_size = ram_state::get_size(machine.save());
		s_size = s_mame_size + fifo_trailer_bytes(machine);
		s_size_known = true;

		// A machine that registered nothing has no state worth the name. HEADER_SIZE alone would
		// still "work" — it would save and load a 32-byte header and change nothing — which is a
		// worse answer than admitting there is no savestate.
		if (machine.save().registration_count() == 0)
			s_size = s_mame_size = 0;
	}
	return s_size;
}


void diff_states(running_machine &machine, void const *lhs, void const *rhs, size_t size, char const *label);
void state_diff(running_machine &machine, void const *buf, size_t size);
void state_dump(running_machine &machine, void const *buf, size_t size);
void state_probe(running_machine &machine, char const *when);


// M2VK_SAVE_PROBE=<substr>: the machine's LIVE condition, printed rather than serialised.
//
// 🚨 This asks the one question no state-file comparison can. A savestate diff compares two
// *recordings*; this reads the machine standing in front of it, which is what "is the RECEIVER ready
// to accept a load" needs — the receiver's condition is by construction not in any state file. It
// was built because loading one vcop2 state at host frame 1500 gives the wrong future and loading
// the same bytes at 1574 gives the right one to the digest, so the deciding variable is on the
// receiving side and had no read-out at all.
//
// Three groups, chosen because they are what the maincpu<->copro handshake is made of:
//   - every generic_fifo_u32_device's occupancy, which is NOT saved (gen_fifo.cpp:54) and so is
//     pure receiver residue that a load leaves exactly where it found it;
//   - every execute device's suspend mask and HALT input line, which ARE saved, so a disagreement
//     between these and the FIFO occupancy is a torn handshake rather than a missing entry;
//   - the screen's frame number and the scheduler's time, to place the sample.
//
// The optional substring additionally prints the LIVE bytes of every matching registry entry. That
// is what M2VK_SAVE_DUMP cannot do — dump reads the serialised buffer, so it can only ever show a
// moment a save was taken.
void state_probe(running_machine &machine, char const *when)
{
	char const *const want = std::getenv("M2VK_SAVE_PROBE");
	if (want == nullptr)
		return;

	std::string line = string_format(" upd=%ld", s_update_count);
	for (screen_device &screen : screen_device_enumerator(machine.root_device()))
	{
		// 🚨 frame minus upd is the whole point of printing both. update() is called once per
		// retro_run, so this difference is how many emulated frames the machine has produced that the
		// frontend never asked for — and it is NOT a constant across runs of the same command. It is
		// what makes two runs of an identical command land at different emulated frames at the same
		// host frame, which every whole-run digest comparison silently assumes cannot happen.
		line += string_format(" frame=%llu (frame-upd=%lld)",
				(unsigned long long)screen.frame_number(),
				(long long)screen.frame_number() - (long long)s_update_count);
		break;
	}
	line += string_format(" t=%s phase=%d%s", machine.time().as_string(9),
			int(machine.phase()), machine.paused() ? " PAUSED" : "");

	for (generic_fifo_u32_device &fifo : device_type_enumerator<generic_fifo_u32_device>(machine.root_device()))
		line += string_format(" %s=%zu%s", fifo.tag(), fifo.size(), fifo.is_full() ? "F" : "");

	for (device_execute_interface &exec : execute_interface_enumerator(machine.root_device()))
	{
		// suspended(reason) is (m_nextsuspend & reason) != 0, so testing one bit at a time
		// reconstructs the mask through the public interface.
		u32 mask = 0;
		for (u32 bit = 1; bit != 0; bit <<= 1)
			if (exec.suspended(bit))
				mask |= bit;
		line += string_format(" %s[susp=%02x halt=%d]", exec.device().tag(), mask,
				exec.input_line_state(INPUT_LINE_HALT));
	}

	osd_printf_info("[model2] probe %-12s%s\n", when, line);

	if (want[0] == '\0')
		return;

	save_manager &save = machine.save();
	for (int i = 0; i < save.registration_count(); i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0, blockcount = 0, stride = 0;
		char const *const name = save.indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (name == nullptr)
			break;
		if ((base == nullptr) || (std::strstr(name, want) == nullptr))
			continue;
		const size_t bytes = size_t(valsize) * valcount;
		std::string hex;
		for (size_t b = 0; (b < bytes) && (b < 32); b++)
			hex += string_format("%02x", static_cast<uint8_t const *>(base)[b]);
		osd_printf_info("[model2] probe   live %-60s %s\n", name, hex);
	}
}

bool state_save(running_machine &machine, void *data, size_t size)
{
	const size_t need = state_size(machine);
	if ((need == 0) || (data == nullptr) || (size < need))
	{
		osd_printf_error("[model2] savestate: refusing to save, need %zu bytes and was offered %zu\n", need, size);
		return false;
	}

	// 🚨 The one precondition MAME enforces on itself and we do not get for free. A TEMPORARY timer —
	// one made by timer_set(), i.e. a pending one-shot — is never registered for save (schedule.cpp:96
	// only calls register_save when !m_temporary) and device_scheduler::postload() DELETES every one
	// of them (schedule.cpp:705). So a state taken while one is outstanding silently drops a scheduled
	// event, and the machine that loads it never fires the callback. MAME's own save path does not
	// refuse in this situation, it DEFERS — machine.cpp:889 returns without cancelling and retries on a
	// later scheduler pass, giving the temporary timer time to expire. libretro has no such option:
	// retro_serialize must answer now.
	//
	// So this is a report, not a gate, and the frame boundary is deliberately not moved to dodge it.
	// See devnotes/savestates.md.
	if (!machine.scheduler().can_save())
		osd_printf_error("[model2] savestate: 🚨 saving with live anonymous timers — the state will lose them (see error.log)\n");

	state_probe(machine, "save");

	// The exact-size buffer is the whole reason this is not a straight write_buffer(data, size):
	// the check inside do_write is size == total_size, so a frontend that hands over a larger
	// buffer (rewind and netplay both may) would otherwise fail for no reason. The buffer is the
	// full size — MAME's part plus our FIFO trailer — and write_buffer is handed only its own part.
	s_buffer.assign(need, 0);

	const save_error err = machine.save().write_buffer(s_buffer.data(), s_mame_size);
	if (err != STATERR_NONE)
	{
		osd_printf_error("[model2] savestate: write_buffer failed: %s\n", error_text(err));
		return false;
	}

	// The FIFO contents, which MAME does not save. See the trailer note at the top of this file —
	// this is not belt-and-braces, two of the eight fixtures are saved with words in flight.
	// ⚠️ memcpy rather than a cast to uint32_t*. The trailer starts at s_mame_size, which is
	// HEADER_SIZE plus the sum of every registry entry's bytes — MAME registers plenty of u8 items,
	// so that offset carries NO alignment guarantee. It happens to be a multiple of 4 on the fixtures
	// measured here, which is exactly how this would survive testing on x86 and then be undefined
	// behaviour on the ARM target.
	if (need > s_mame_size)
	{
		uint8_t *w = s_buffer.data() + s_mame_size;
		auto put = [&w] (uint32_t v) { std::memcpy(w, &v, sizeof(v)); w += sizeof(v); };
		put(FIFO_TRAILER_MAGIC);
		put(uint32_t(fifo_count(machine)));
		for (generic_fifo_u32_device &fifo : device_type_enumerator<generic_fifo_u32_device>(machine.root_device()))
		{
			const size_t held = fifo.size();
			const size_t n = std::min(held, FIFO_TRAILER_MAX);
			if (held > FIFO_TRAILER_MAX)
				osd_printf_error("[model2] savestate: 🚨 %s holds %zu words, only %zu fit the trailer — raise FIFO_TRAILER_MAX\n",
						fifo.tag(), held, FIFO_TRAILER_MAX);
			put(uint32_t(n));
			// peek() walks m_values and then m_extra_values, so index order here is pop order, and
			// replaying it through push() on load rebuilds both queues with the same split.
			for (size_t i = 0; i < n; i++)
				put(fifo.peek(offs_t(i)));
			w += sizeof(uint32_t) * (FIFO_TRAILER_MAX - n);
			if (held != 0)
				osd_printf_info("[model2] savestate: %s carried %zu words in the trailer\n", fifo.tag(), n);
		}
	}

	std::memcpy(data, s_buffer.data(), need);
	state_dump(machine, s_buffer.data(), s_buffer.size());
	state_diff(machine, s_buffer.data(), s_buffer.size());
	return true;
}


// M2VK_SAVE_DUMP=<substring>: print the serialised bytes of every registry entry whose name contains
// the substring. The companion to the diff — the diff says WHICH entry differs, this says what is in
// it, which is what a question like "was the CPU mid-burst-stall when this was taken" needs.
void state_dump(running_machine &machine, void const *buf, size_t size)
{
	char const *const want = std::getenv("M2VK_SAVE_DUMP");
	if ((want == nullptr) || (want[0] == '\0'))
		return;

	uint8_t const *const a = static_cast<uint8_t const *>(buf);
	save_manager &save = machine.save();
	size_t offset = 32;
	for (int i = 0; i < save.registration_count(); i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0, blockcount = 0, stride = 0;
		char const *const name = save.indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (name == nullptr)
			break;
		const size_t bytes = size_t(valsize) * valcount * (blockcount ? blockcount : 1);
		if (offset + bytes > size)
			break;
		if (std::strstr(name, want) != nullptr)
		{
			std::string hex;
			for (size_t b = 0; (b < bytes) && (b < 32); b++)
				hex += string_format("%02x", a[offset + b]);
			if (bytes > 32)
				hex += "…";
			osd_printf_info("[model2] savestate dump: %-64s %s\n", name, hex);
		}
		offset += bytes;
	}
}


// M2VK_SAVE_DIFF=<file>: compare the state just written against a reference state file and name the
// registry entries whose bytes differ.
//
// This exists because bisecting a failing savestate by adding one save_item and rebuilding is a
// four-minute cycle per guess, and the guesses are unbounded. Two states taken at the SAME emulated
// frame by two runs that should agree will differ in exactly the entries that carry the difference —
// and anything the registry does not cover shows up as "the states are identical but the pictures are
// not", which is the other answer worth having and cannot be reached any other way.
//
// The offsets are do_write's own layout: HEADER_SIZE, then each entry's typesize*typecount*blockcount
// in registry order (save.cpp do_write). Nothing here reads MAME's private state; it walks
// indexed_item, which is public for exactly this kind of tooling.
void state_diff(running_machine &machine, void const *buf, size_t size)
{
	char const *const path = std::getenv("M2VK_SAVE_DIFF");
	if ((path == nullptr) || (path[0] == '\0'))
		return;

	std::vector<uint8_t> ref;
	{
		FILE *const f = std::fopen(path, "rb");
		if (f == nullptr)
		{
			osd_printf_error("[model2] savestate: M2VK_SAVE_DIFF cannot open %s\n", path);
			return;
		}
		std::fseek(f, 0, SEEK_END);
		const long len = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (len > 0)
		{
			ref.resize(size_t(len));
			if (std::fread(ref.data(), 1, ref.size(), f) != ref.size())
				ref.clear();
		}
		std::fclose(f);
	}

	if (ref.size() != size)
	{
		osd_printf_error("[model2] savestate: M2VK_SAVE_DIFF reference is %zu bytes, this state is %zu\n",
				ref.size(), size);
		return;
	}

	diff_states(machine, buf, ref.data(), size, path);
}


// The comparator itself, split out of state_diff so that a LOAD can use it too (state_load's
// M2VK_SAVE_VERIFY). Names the registry entries whose bytes differ between two states of the same
// build.
void diff_states(running_machine &machine, void const *lhs, void const *rhs, size_t size, char const *label)
{
	uint8_t const *const a = static_cast<uint8_t const *>(lhs);
	uint8_t const *const ref = static_cast<uint8_t const *>(rhs);
	save_manager &save = machine.save();

	// The header carries the game name and the registry CRC; a difference there is a different build
	// and nothing below it would mean anything.
	if (std::memcmp(a, ref, 32) != 0)
	{
		osd_printf_info("[model2] savestate diff: HEADERS DIFFER — different game or different build\n");
		return;
	}

	size_t offset = 32, differing = 0, diff_bytes = 0;
	osd_printf_info("[model2] savestate diff vs %s:\n", label);
	for (int i = 0; i < save.registration_count(); i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0, blockcount = 0, stride = 0;
		char const *const name = save.indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (name == nullptr)
			break;
		const size_t bytes = size_t(valsize) * valcount * (blockcount ? blockcount : 1);
		if (offset + bytes > size)
			break;

		size_t n = 0;
		for (size_t b = 0; b < bytes; b++)
			if (a[offset + b] != ref[offset + b])
				n++;
		if (n != 0)
		{
			differing++;
			diff_bytes += n;
			// Capped: a genuinely diverged machine differs in hundreds of entries and the useful
			// signal is the first few, which are in registry order (machine, then devices). Raise it
			// with M2VK_SAVE_DIFF_MAX when the question is a whole-registry audit rather than a
			// diagnosis — the timer entries in particular sort late and are invisible under the
			// default.
			if (differing <= diff_cap())
				osd_printf_info("[model2] savestate diff:   %6zu/%-8zu bytes  %s\n", n, bytes, name);
		}
		offset += bytes;
	}
	osd_printf_info("[model2] savestate diff: %zu of %d entries differ, %zu bytes total\n",
			differing, save.registration_count(), diff_bytes);
}


bool state_load(running_machine &machine, void const *data, size_t size)
{
	const size_t need = state_size(machine);
	if ((need == 0) || (data == nullptr) || (size < need))
	{
		osd_printf_error("[model2] savestate: refusing to load, need %zu bytes and was offered %zu\n", need, size);
		return false;
	}

	// The receiver, sampled BEFORE read_buffer touches anything. Paired with the "save" probe this is
	// the only way to see the half of the transfer that is not in the file.
	state_probe(machine, "pre-load");

	// read_buffer's length check is also an equality, so hand it exactly MAME's own size and ignore
	// both our trailer and any tail the frontend kept. The header is validated inside do_read before
	// a byte of machine state is touched (save.cpp:469), so a wrong-game or wrong-build state fails
	// here rather than half-applying.
	const save_error err = machine.save().read_buffer(data, s_mame_size);
	if (err != STATERR_NONE)
	{
		osd_printf_error("[model2] savestate: read_buffer failed: %s\n", error_text(err));
		return false;
	}

	// 🚨 The FIFO contents, which the registry does not cover, and which decide the outcome on three
	// of the eight fixtures. Both halves matter and the second is the one that is easy to miss:
	// clear() drops the words the RECEIVING machine happened to be holding — measured on vcop2, whose
	// own save is clean but whose loader holds 6 to 8 — and the replay restores the words the SAVING
	// machine had in flight, which desert and lastbrnx do.
	//
	// ⚠️ Order: after read_buffer, deliberately. Doing it before would mean the callbacks these calls
	// fire (m_empty_cb, m_on_fifo_unfull, m_on_fifo_unempty — all of which reach set_input_line and
	// therefore enqueue an event behind a scheduler().synchronize() TEMPORARY timer) were still
	// outstanding when read_buffer ran dispatch_postload, and postload DELETES temporary timers
	// (schedule.cpp:705) while device_input::m_qindex is not in the registry at all. That would leave
	// a queue with a pending event and no timer to drain it, and set_state_synced only arms a new
	// timer when the queue was empty (diexec.cpp:684) — so the HALT line would stop responding for
	// the rest of the session. After read_buffer there is no postload left to run and the events
	// drain on the next timeslice.
	//
	// ⚠️ memcpy rather than a cast, for the reason spelled out on the writing side: s_mame_size has no
	// alignment guarantee.
	if ((need > s_mame_size) && (size >= need))
	{
		uint8_t const *const base = static_cast<uint8_t const *>(data) + s_mame_size;
		auto get = [base] (size_t word) { uint32_t v; std::memcpy(&v, base + word * sizeof(uint32_t), sizeof(v)); return v; };
		if (get(0) != FIFO_TRAILER_MAGIC)
		{
			osd_printf_error("[model2] savestate: 🚨 the FIFO trailer is missing or misaligned; FIFO contents not restored\n");
		}
		else
		{
			const uint32_t stored = get(1);
			uint32_t index = 0;
			for (generic_fifo_u32_device &fifo : device_type_enumerator<generic_fifo_u32_device>(machine.root_device()))
			{
				if (index >= stored)
					break;
				const size_t at = 2 + size_t(index) * (1 + FIFO_TRAILER_MAX);
				const uint32_t n = std::min<uint32_t>(get(at), FIFO_TRAILER_MAX);
				if (fifo.size() != 0)
					osd_printf_info("[model2] savestate: %s dropped %zu stale words on load\n", fifo.tag(), fifo.size());
				fifo.clear();
				for (uint32_t i = 0; i < n; i++)
					fifo.push(get(at + 1 + i));
				if (n != 0)
					osd_printf_info("[model2] savestate: %s restored %u words from the trailer\n", fifo.tag(), n);
				index++;
			}
		}
	}

	// 🚨 Invalidate the two DISPLAY CACHES that MAME's own post-load handling misses on this driver.
	// Without this the emulated machine is restored perfectly and the picture is still wrong —
	// permanently, not for a frame — which is the single most confusing failure in this whole area and
	// cost most of a session to pin down. The measurement that found it: two states taken at the same
	// emulated frame, one from a straight run and one from a run that loaded a state 20 frames
	// earlier, differ in 5 of 5397 registry entries and 9 bytes, none of it emulated hardware — while
	// the pictures never reconverge. State complete, caches stale.
	//
	// Both gaps are upstream's and both are invisible to MAME because no Model 2 set claims savestate
	// support:
	//
	//   - device_gfx_interface::interface_post_load() (digfx.cpp:106) marks RAM-backed gfx dirty ONLY
	//     by walking m_gfxdecodeinfo, and segas24_tile_device has none — it calls set_gfx() directly
	//     (segaic24.cpp:98) with a gfx_element built over its own char_ram. So the decoded character
	//     cache is never invalidated, and char_ram restored by a state load is simply not looked at.
	//
	//   - tilemap_t::postload() (tilemap.cpp:670) calls mappings_update() and NOT mark_all_dirty(),
	//     so tiles whose tile_ram changed under a load are never re-fetched. The device only ever
	//     marks tiles dirty from its write handler (segaic24.cpp:533), which a state load bypasses.
	//
	// Both are fixed from here rather than in the driver, because both APIs are public — so this costs
	// no upstream line. See devnotes/savestates.md.
	machine.tilemap().mark_all_dirty();
	for (device_gfx_interface &intf : gfx_interface_enumerator(machine.root_device()))
		for (u8 i = 0; i < MAX_GFX_ELEMENTS; i++)
			if (gfx_element *const g = intf.gfx(i))
				g->mark_all_dirty();

	// M2VK_SAVE_VERIFY=1: serialise straight back out and name every entry that did not come back the
	// way it went in. This separates the two failures that a diff taken a frame later cannot tell
	// apart — "the load did not restore X" and "the machine ran one frame differently" — and it is the
	// only one of the two that can be answered without a second run to compare against.
	//
	// It is not a tautology even though it reads like one: read_buffer runs dispatch_postload, whose
	// device callbacks recompute derived fields from the ones just restored, so an entry that is
	// itself a *derived* quantity can come back different from the bytes that were handed in. That is
	// a real class of bug and this is what makes it visible.
	if (env_flag("M2VK_SAVE_VERIFY"))
	{
		// MAME's part only, on both sides: write_buffer's length check is an equality, and the
		// trailer is not in the registry so diff_states could not name it anyway.
		std::vector<uint8_t> after(s_mame_size);
		const save_error werr = machine.save().write_buffer(after.data(), after.size());
		if (werr != STATERR_NONE)
			osd_printf_error("[model2] savestate verify: write_buffer failed: %s\n", error_text(werr));
		else
			diff_states(machine, after.data(), data, s_mame_size, "the state just loaded (M2VK_SAVE_VERIFY)");
	}

	return true;
}


void state_log(running_machine &machine)
{
	// Called every frame from update(). Reports on the first call and again whenever the registry
	// count MOVES, which is the thing worth seeing: registration closes in
	// allow_registration(false) (machine.cpp:306) and anything that arrives after the first frame
	// would mean this instrument is reading the registry too early to be believed.
	static int last_count = -1;
	static int reports = 0;

	s_update_count++;

	// M2VK_SAVE_PROBE_FROM/TO=<frame>: the same probe, every frame over a window, with no savestate
	// activity at all. This is what turns "the receiver's condition decides" into a testable claim —
	// it traces the condition across the frames where loading works and the frames where it does
	// not, in ONE run, so the two samples cannot differ for any reason other than the frame.
	{
		char const *const from = std::getenv("M2VK_SAVE_PROBE_FROM");
		if (from != nullptr)
		{
			char const *const to = std::getenv("M2VK_SAVE_PROBE_TO");
			const long lo = std::strtol(from, nullptr, 10);
			const long hi = (to != nullptr) ? std::strtol(to, nullptr, 10) : (lo + 200);
			for (screen_device &screen : screen_device_enumerator(machine.root_device()))
			{
				const long n = long(screen.frame_number());
				if ((n >= lo) && (n <= hi))
					state_probe(machine, "frame");
				break;
			}
		}
	}

	if (!env_flag("M2VK_SAVE_LOG"))
		return;

	save_manager &save = machine.save();
	if ((save.registration_count() == last_count) || (reports >= 4))
		return;
	last_count = save.registration_count();
	reports++;

	// supported() is false on every Model 2 set, because none carries MACHINE_SUPPORTS_SAVE and its
	// absence puts SAVE_UNSUPPORTED on the driver's own device type (gamedrv.h:119). Printing the
	// device that caused it distinguishes "the driver never claimed support" — expected, harmless,
	// and the state of the world here — from "some library device declares it cannot be saved",
	// which would be a real finding.
	char const *culprit = "(none)";
	for (device_t &device : device_enumerator(machine.root_device()))
	{
		if (device.type().emulation_flags() & device_t::flags::SAVE_UNSUPPORTED)
		{
			culprit = device.tag();
			break;
		}
	}

	osd_printf_info("[model2] savestate: %d registry entries, %zu bytes, registration %s, supported=%s (first SAVE_UNSUPPORTED device: %s)\n",
			save.registration_count(), state_size(machine),
			save.registration_allowed() ? "STILL OPEN — too early to believe this" : "closed",
			save.supported() ? "yes" : "no", culprit);

	// The ten largest entries, which is where a surprise lives. MAME's own dump_registry() prints
	// all of them but only under -verbose, which this OSD does not plumb through.
	struct row { char const *name; size_t bytes; };
	std::vector<row> rows;
	rows.reserve(save.registration_count());
	size_t total = 0;
	for (int i = 0; i < save.registration_count(); i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0, blockcount = 0, stride = 0;
		char const *const name = save.indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (name == nullptr)
			continue;
		const size_t bytes = size_t(valsize) * valcount * (blockcount ? blockcount : 1);
		rows.push_back({ name, bytes });
		total += bytes;
	}
	std::sort(rows.begin(), rows.end(), [] (row const &a, row const &b) { return a.bytes > b.bytes; });

	osd_printf_info("[model2] savestate: %zu bytes across %zu entries; ten largest:\n", total, rows.size());
	for (size_t i = 0; (i < rows.size()) && (i < 10); i++)
		osd_printf_info("[model2] savestate:   %9zu  %s\n", rows[i].bytes, rows[i].name);
}


void state_close()
{
	// The cached size is per-machine: a second content load in the same process registers a fresh
	// registry, and inheriting the first machine's number would size every buffer wrong.
	s_size = 0;
	s_mame_size = 0;
	s_size_known = false;
	s_update_count = 0;
	s_buffer.clear();
	s_buffer.shrink_to_fit();
}

} // namespace m2vk
