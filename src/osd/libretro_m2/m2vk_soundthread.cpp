// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    M2VK_SOUND_THREAD — the SEGAM1AUDIO board on a worker thread. See m2vk_soundthread.h for the shape
    and the threading rules; this file is the whole of the implementation.

    Compiled into the mame_model2 driver project (modelizer.lua), where every MAME header this needs —
    the driver macros, segam1audio, i8251, running_machine, save_manager — is already on the include
    path. The OSD lib and model2.cpp see only the forward-declared header, and the symbols resolve at the
    final link, exactly as the render seams do.

    Three moving parts:

      * serial_line — the cross-thread replay engine. A producer thread pushes (absolute-emulated-time,
        line-state) transitions; the owning machine's thread pumps them onto its scheduler, one emu_timer
        chained through the queue, preserving inter-bit spacing. Used once per direction.
      * sound_host + sound_osd + m1snd_driver — the second, headless running_machine that hosts just the
        board, on its own thread, paced to stay ~1 frame behind the main machine. Its mixed stereo output
        lands in the audio ring.
      * the free functions the header exports, which are the only things anyone else calls.

*********************************************************************************************************************************/

#include "emu.h"

#include "m2vk_soundthread.h"
#include "m2vk_affinity.h"

#include "segam1audio.h"

#include "machine/i8251.h"

#include "m2vk_baud.h"

#include "emuopts.h"
#include "drivenum.h"
#include "main.h"
// osdepend.h forward-declares ui::menu_item and then declares
// `virtual std::vector<ui::menu_item> get_slider_list()`.  libstdc++ 16 instantiates that vector's
// defaulted constructor (and so its destructor) from the declaration alone, which an incomplete
// element type cannot satisfy -- so a translation unit that includes osdepend.h directly now needs
// the complete type.  Upstream's own OSD sources already pull menuitem.h in for the same reason
// (osdobj_common.cpp, osdwindow.h); this is that include, not a new dependency.
#include "../frontend/mame/ui/menuitem.h"
#include "osdepend.h"
#include "save.h"
#include "render.h"
#include "ui/uimain.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// Defined in src/frontend/mame/mame.cpp (always linked). Suppresses the frontend lua/UI hooks
// (emulator_info::periodic_check / frame_hook / sound_hook / draw_user_interface) on the calling
// thread, so the worker machine's own video/sound frame updates do not drive the PRIMARY machine's
// lua engine from this thread and corrupt its shared sol::state — an intermittent "call a nil value"
// PANIC before this was gated. Declared at global scope so it binds to the global definition.
void mame_suppress_frontend_hooks(bool suppress);

namespace m2vk_snd {

namespace {

//============================================================
//  the gate
//============================================================

// -1 unknown, 0 off, 1 on. The core option seeds it (set_option_enabled); the environment variable
// M2VK_SOUND_THREAD overrides, matching the M2VK_* wins-over-option rule elsewhere.
int  g_option = -1;
bool g_resolved = false;
bool g_enabled = false;
bool g_host_present = false;   // set by engage_host(); gates enabled() so only a worker-hosting OSD splits
bool g_board_split = false;    // set by note_board_split() when the model2o hook removes the board

bool env_on()
{
	char const *const v = std::getenv("M2VK_SOUND_THREAD");
	if ((v == nullptr) || (v[0] == '\0'))
		return false;
	return v[0] != '0';
}

bool env_present()
{
	char const *const v = std::getenv("M2VK_SOUND_THREAD");
	return (v != nullptr) && (v[0] != '\0');
}

//============================================================
//  the serial replay engine (one per direction)
//============================================================
//
// Both machines share the emulated-time origin (both start at t=0), so a transition tagged with the
// sender's absolute time can be scheduled on the receiver's own scheduler at that same absolute time —
// and when that time is already in the receiver's past (the sound->main direction, since the worker
// lags), the queue slides forward preserving the deltas between transitions, so byte framing survives
// and only the phase shifts by the lag. One rule covers both directions.
class serial_line
{
public:
	// Bind to the owning machine and the sink that applies a line transition on that machine's thread.
	// Called once, on the owning machine's thread, after it has started.
	void attach(running_machine &machine, std::function<void(int)> sink)
	{
		m_machine = &machine;
		m_sink = std::move(sink);
		m_timer = machine.scheduler().timer_alloc(
				timer_expired_delegate(&serial_line::tick, "m2vk_snd::serial_line::tick", this));
		m_have_last = false;
	}

	// Producer thread: enqueue a transition tagged with the sender's absolute emulated time (seconds).
	void push(double t_sec, int state)
	{
		std::lock_guard<std::mutex> lock(m_inbox_mutex);
		m_inbox.push_back({ t_sec, state });
	}

	// Owning machine's thread: move the inbox onto the scheduler. Call once per frame from the owning
	// machine's update(), before its scheduler advances the next frame.
	void pump()
	{
		if (m_machine == nullptr)
			return;

		std::deque<xfer> pending;
		{
			std::lock_guard<std::mutex> lock(m_inbox_mutex);
			pending.swap(m_inbox);
		}
		if (pending.empty())
			return;

		const attotime now = m_machine->time();
		for (xfer const &x : pending)
		{
			const attotime tag = attotime::from_double(x.t);
			attotime deliver;
			if (m_have_last)
				deliver = m_last_deliver + (tag - m_last_tag);   // preserve the inter-bit delta
			else
				deliver = tag;
			if (deliver < now)
				deliver = now;                                   // never schedule in the past

			m_fifo.push_back({ deliver, x.state });
			m_last_tag = tag;
			m_last_deliver = deliver;
			m_have_last = true;
		}

		arm();
	}

	// Owning machine's thread: drop any queued state (savestate load / reset). The scheduler timer is
	// disarmed by adjusting it to never on the next arm().
	void clear()
	{
		std::lock_guard<std::mutex> lock(m_inbox_mutex);
		m_inbox.clear();
		m_fifo.clear();
		m_have_last = false;
		if (m_timer != nullptr)
			m_timer->adjust(attotime::never);
	}

private:
	struct xfer { double t; int state; };
	struct pend { attotime deliver; int state; };

	void arm()
	{
		if (m_fifo.empty() || m_timer == nullptr)
			return;
		const attotime now = m_machine->time();
		attotime when = m_fifo.front().deliver - now;
		if (when < attotime::zero)
			when = attotime::zero;
		m_timer->adjust(when);
	}

	void tick(s32)
	{
		if (m_fifo.empty())
			return;
		const int state = m_fifo.front().state;
		m_fifo.pop_front();
		if (m_sink)
			m_sink(state);
		arm();
	}

	running_machine *m_machine = nullptr;
	std::function<void(int)> m_sink;
	emu_timer *m_timer = nullptr;

	std::mutex m_inbox_mutex;
	std::deque<xfer> m_inbox;   // cross-thread landing pad

	std::deque<pend> m_fifo;    // owning-thread schedule
	attotime m_last_tag = attotime::zero;
	attotime m_last_deliver = attotime::zero;
	bool m_have_last = false;
};

//============================================================
//  shared bridge state
//============================================================

serial_line g_to_sound;    // main -> sound (replayed on the worker's scheduler)
serial_line g_to_main;     // sound -> main (replayed on the main's scheduler)

// The main machine's i8251, so g_to_main can drive its RXD. Set on the main thread in set_main_uart(),
// read on the main thread in g_to_main's sink. A plain pointer: the main machine outlives the bridge.
i8251_device *g_main_uart = nullptr;

// The sound board in the worker machine, so g_to_sound can drive its TXD (== the board UART's RXD). Set
// on the worker thread in the driver's machine_start, read on the worker thread in g_to_sound's sink.
segam1audio_device *g_board = nullptr;

// The main UART's demand-gated baud clock (m2vk_baud.h), when it is in use: its RxD has to arrive
// through the generator so a sleeping receiver is woken. Null when M2VK_LAZY_BAUD=0, in which case the
// line goes straight at the UART as before. The sound board routes its own RxD (segam1audio.cpp).
m2vk_baud_device *g_main_baud = nullptr;

// The main machine, so the worker's machine_start can attach g_to_main to it. Set on the main thread in
// start() before the worker launches (happens-before via thread start); read once on the worker thread.
running_machine *g_main_machine = nullptr;

// Pacing: the main machine's current emulated time in seconds, published each frame by pump_main. The
// worker blocks in its update() until this is at least a frame ahead of the worker, so the worker never
// runs ahead of the main (which would make main->sound transitions land in its past).
std::atomic<double> g_main_time{ 0.0 };
std::mutex g_pace_mutex;
std::condition_variable g_pace_cv;

constexpr double LAG_SEC = 0.0174;   // ~1 frame at Daytona's ~57.5 Hz; matches the Stage-0 bracket

std::atomic<bool> g_stop{ false };
std::atomic<bool> g_running{ false };

// The captured main-machine ROM regions, filled into the worker's identical regions before its CPU
// runs. Set on the main thread in start() before the worker launches (happens-before via thread start);
// read on the worker thread in the driver's machine_start.
struct rom_src { uint8_t const *base; size_t bytes; };
rom_src g_rom[3];   // sndcpu, pcm1, pcm2
char const *const k_region_tag[3] = { "m1audio:sndcpu", "m1audio:pcm1", "m1audio:pcm2" };

//============================================================
//  the audio ring
//============================================================
//
// The worker's mixed stereo output (interleaved int16) lands here; the frontend pulls it in place of
// the main machine's now-silent mix. Mutex-guarded because the worker writes while the frontend reads.

std::mutex g_ring_mutex;
std::deque<int16_t> g_ring;                 // interleaved L,R,L,R...
std::vector<int16_t> g_pull_buf;            // what pull_audio hands back, stable until the next pull
size_t g_pull_frames = 0;                   // stereo frames currently in g_pull_buf, pending consume

// Keep the ring from growing without bound if the frontend stops pulling (paused frontend, or the
// worker briefly outrunning it). Two frames at 48 kHz stereo is plenty of slack.
constexpr size_t RING_CAP_SAMPLES = 48000 / 20 * 2 * 2;

void ring_push(const int16_t *buf, int frames)
{
	std::lock_guard<std::mutex> lock(g_ring_mutex);
	g_ring.insert(g_ring.end(), buf, buf + (size_t(frames) * 2));
	while (g_ring.size() > RING_CAP_SAMPLES)
		g_ring.pop_front();
}

//============================================================
//  savestate park handshake
//============================================================
//
// The worker machine holds sound state the main machine no longer does. To (de)serialise it the worker
// is parked at its per-frame safe point (update(), between frames — the same rule the main OSD's
// savestate obeys), the frontend does the buffer I/O, then the worker is released.

std::atomic<bool> g_park_request{ false };
std::atomic<bool> g_parked{ false };
std::mutex g_park_mutex;
std::condition_variable g_park_cv;
running_machine *g_worker_machine = nullptr;   // set by the worker in sound_osd::init, read while parked

//============================================================
//  the stub OSD for the worker machine
//============================================================
//
// A direct osd_interface: no modules, no window, nothing shared with the main OSD. Everything is a
// no-op except the audio sink (capture the board's mix) and update() (pace the worker, pump the
// main->sound serial, service savestate parks).

class sound_osd : public osd_interface
{
public:
	// core
	virtual void init(running_machine &machine) override
	{
		m_machine = &machine;
		g_worker_machine = &machine;

		// Allocate one (non-hidden) render target so render_manager::m_ui_target is set. Nothing here
		// presents it, but running_machine's exit-time config save dereferences m_ui_target
		// unconditionally, and a headless machine has none otherwise. Freed with the machine.
		machine.render().target_alloc();

		// The board and its serial sink are wired in the driver's machine_start (below), which runs
		// after every device has started; nothing else to do here.
	}

	virtual void update(bool /*skip_redraw*/) override
	{
		if (m_machine == nullptr)
			return;

		// Keep the worker on the big cluster too — worker_main pins it once, but Android wipes
		// thread affinity on app-state transitions (see m2vk_affinity.h).
		static unsigned s_worker_repin = 0;
		m2vk_repin_self(s_worker_repin);

		// Deliver any pending main->sound transitions onto this machine's scheduler.
		g_to_sound.pump();

		// Park and pace share one loop so a park request is honoured even when we are blocked in the
		// pace wait. 🚨 That coupling is the whole point: a savestate parks the MAIN emulation, which
		// freezes g_main_time, so a worker sitting in the pace wait would never see its predicate
		// (g_main_time >= self + LAG) become true and would never reach a standalone park check above.
		// The park request therefore has to be a wake condition of the pace wait too, and on waking for
		// it we loop back to park rather than running a frame ahead. Whether the worker is mid-frame or
		// pace-blocked when the request lands is a race; before this loop it decided the savestate.
		for (;;)
		{
			// Savestate: if the frontend has asked to park, announce parked and hold until released,
			// then re-evaluate (it may still be parked, or fall through to pace). The announce (store +
			// notify) is done UNDER g_park_mutex so with_worker_parked, which blocks on g_park_cv for
			// g_parked, cannot miss it — the old code busy-spun on the flag and lost the race under load.
			if (g_park_request.load(std::memory_order_acquire))
			{
				std::unique_lock<std::mutex> lock(g_park_mutex);
				g_parked.store(true, std::memory_order_release);
				g_park_cv.notify_all();
				g_park_cv.wait(lock, [] { return !g_park_request.load(std::memory_order_acquire) || g_stop.load(); });
				g_parked.store(false, std::memory_order_release);
				continue;
			}

			// Pace: never run ahead of the main machine. Block until the main is at least a frame ahead,
			// but also wake to service a park request (see the note above).
			const double self = m_machine->time().as_double();
			std::unique_lock<std::mutex> lock(g_pace_mutex);
			g_pace_cv.wait(lock, [self] {
				return g_stop.load()
					|| g_park_request.load(std::memory_order_acquire)
					|| (g_main_time.load(std::memory_order_acquire) >= self + LAG_SEC);
			});
			if (g_park_request.load(std::memory_order_acquire))
				continue;   // go park rather than running a frame
			break;
		}

		if (g_stop.load())
			m_machine->schedule_exit();
	}

	virtual void input_update(bool /*relative_reset*/) override { }
	virtual void check_osd_inputs() override { }
	virtual void set_verbose(bool /*print_verbose*/) override { }

	// debugger
	virtual void init_debugger() override { }
	virtual void wait_for_debugger(device_t & /*device*/, bool /*firststop*/) override { }

	// audio
	virtual bool no_sound() override { return false; }
	virtual bool sound_external_per_channel_volume() override { return false; }
	virtual bool sound_split_streams_per_source() override { return false; }
	virtual uint32_t sound_get_generation() override { return 1; }

	virtual osd::audio_info sound_get_information() override
	{
		osd::audio_info result;
		result.m_generation = 1;
		result.m_default_sink = 1;
		result.m_default_source = 0;
		result.m_nodes.resize(1);
		result.m_nodes[0].m_name = "m2vk-sound-thread";
		result.m_nodes[0].m_display_name = "sound thread";
		result.m_nodes[0].m_id = 1;
		result.m_nodes[0].m_rate.m_default_rate = 0;
		result.m_nodes[0].m_rate.m_min_rate = 0;
		result.m_nodes[0].m_rate.m_max_rate = 0;
		result.m_nodes[0].m_sinks = 2;
		result.m_nodes[0].m_sources = 0;
		result.m_nodes[0].m_port_names.emplace_back("L");
		result.m_nodes[0].m_port_names.emplace_back("R");
		result.m_nodes[0].m_port_positions.emplace_back(osd::channel_position::FL());
		result.m_nodes[0].m_port_positions.emplace_back(osd::channel_position::FR());
		if (m_stream_id != 0)
		{
			result.m_streams.resize(1);
			result.m_streams[0].m_id = m_stream_id;
			result.m_streams[0].m_node = 1;
		}
		return result;
	}

	virtual uint32_t sound_stream_sink_open(uint32_t /*node*/, std::string /*name*/, uint32_t /*rate*/) override
	{
		if (m_stream_id != 0)
			return 0;
		m_stream_id = 1;
		return m_stream_id;
	}
	virtual uint32_t sound_stream_source_open(uint32_t /*node*/, std::string /*name*/, uint32_t /*rate*/) override { return 0; }
	virtual void sound_stream_close(uint32_t id) override { if (id == m_stream_id) m_stream_id = 0; }
	virtual void sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame) override
	{
		if (id != m_stream_id || buffer == nullptr || samples_this_frame <= 0)
			return;
		ring_push(buffer, samples_this_frame);
	}
	virtual void sound_stream_source_update(uint32_t /*id*/, int16_t * /*buffer*/, int /*samples_this_frame*/) override { }
	virtual void sound_stream_set_volumes(uint32_t /*id*/, const std::vector<float> & /*db*/) override { }
	virtual void sound_begin_update() override { }
	virtual void sound_end_update() override { }

	// input types / UI / misc — all inert for a headless sound board
	virtual void customize_input_type_list(std::vector<input_type_entry> & /*typelist*/) override { }
	virtual void add_audio_to_recording(const int16_t * /*buffer*/, int /*samples_this_frame*/) override { }
	virtual std::vector<ui::menu_item> get_slider_list() override { return {}; }

	virtual osd_font::ptr font_alloc() override { return nullptr; }
	virtual bool get_font_families(std::string const & /*font_path*/, std::vector<std::pair<std::string, std::string> > & /*result*/) override { return false; }
	virtual bool execute_command(const char * /*command*/) override { return false; }

	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view /*name*/) override { return nullptr; }
	virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view /*name*/) override { return nullptr; }
	virtual std::vector<osd::midi_port_info> list_midi_ports() override { return {}; }
	virtual std::unique_ptr<osd::network_device> open_network_device(int /*id*/, osd::network_handler & /*handler*/) override { return nullptr; }
	virtual std::vector<osd::network_device_info> list_network_devices() override { return {}; }

private:
	running_machine *m_machine = nullptr;
	uint32_t m_stream_id = 0;
};

//============================================================
//  the synthetic driver — root of the worker machine
//============================================================
//
// One segam1audio board, tagged "m1audio" so its regions are "m1audio:sndcpu" / "pcm1" / "pcm2",
// matching the tags captured from the main machine. The regions are declared with no ROM_LOAD, so
// rom_load allocates them zeroed; machine_start fills them from the captured main-machine bytes before
// the 68000's first fetch, and wires the two serial sinks.

class m1snd_driver : public driver_device
{
public:
	m1snd_driver(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_board(*this, "m1audio")
	{
	}

	// MACHINE constructor (referenced by the GAME() below): one board, wired for sound->main replies.
	void m1snd(machine_config &config)
	{
		segam1audio_device &board = SEGAM1AUDIO(config, "m1audio");
		// sound -> main: the board's UART TXD, tagged with this (worker) machine's time and queued for
		// the main machine to replay on its UART RXD.
		board.rxd_handler().set(*this, FUNC(m1snd_driver::sound_reply));
	}

	void sound_reply(int state)
	{
		g_to_main.push(machine().time().as_double(), state);
	}

protected:
	virtual void machine_start() override
	{
		g_board = m_board;

		// Fill the ROM regions from the main machine's already-loaded bytes, before the 68000 runs.
		for (int i = 0; i < 3; i++)
		{
			memory_region *const region = memregion(k_region_tag[i]);
			if ((region == nullptr) || (g_rom[i].base == nullptr))
				continue;
			const size_t n = std::min<size_t>(region->bytes(), g_rom[i].bytes);
			std::memcpy(region->base(), g_rom[i].base, n);
		}

		// main -> sound: apply a queued transition onto the board's UART RXD (write_txd feeds it). This
		// scheduler belongs to THIS (worker) machine and thread, so attach it here.
		//
		// The mirror direction (g_to_main) is attached in start() on the main thread — its timer lives on
		// the main machine's scheduler, and allocating it from here would race the main thread.
		g_to_sound.attach(machine(), [](int state) { if (g_board) g_board->write_txd(state); });
	}

private:
	required_device<segam1audio_device> m_board;
};

// Empty input ports and a ROM map that only reserves the three regions (no files — filled from the main
// machine in machine_start).
INPUT_PORTS_START(m1snd)
INPUT_PORTS_END

ROM_START(m1snd)
	ROM_REGION(0xc0000, "m1audio:sndcpu", ROMREGION_BE | ROMREGION_16BIT)
	ROM_REGION(0x400000, "m1audio:pcm1", 0)
	ROM_REGION(0x400000, "m1audio:pcm2", ROMREGION_ERASE00)
ROM_END

// The game_driver + device_type for the synthetic system, minted by the macro. Defined inside this
// anonymous namespace (internal linkage) so it is never a selectable system and never reaches the
// generated driver list — it is referenced by C++ name only, in worker_main below. MACHINE_SUPPORTS_SAVE
// is load-bearing: without it the type carries SAVE_UNSUPPORTED and the worker's savestate would be
// refused. empty_init is driver_device's do-nothing DRIVER_INIT.
GAME(2026, m1snd, 0, m1snd, m1snd, m1snd_driver, empty_init, ROT0, "m2vk", "M2VK sound thread", MACHINE_SUPPORTS_SAVE)

//============================================================
//  the worker host
//============================================================

std::thread g_worker;
std::unique_ptr<emu_options> g_worker_opts;

// The name the worker's copy of the m1snd descriptor is given — see the comment in worker_main().
// Set by start() from the main machine, on the main thread, before the worker is spawned.
char g_worker_driver_name[MAX_DRIVER_NAME_CHARS + 1] = { 0 };

// A minimal machine_manager: no UI, no cheats, the stub OSD. Everything the base leaves virtual is a
// no-op, which is exactly right for a headless sound board.
class sound_manager_shim : public machine_manager
{
public:
	sound_manager_shim(emu_options &options, osd_interface &osd) : machine_manager(options, osd) { }

	// running_machine::start() dereferences the ui_manager unconditionally (set_startup_text). The base
	// ui_manager is concrete and entirely no-op, which is exactly what a headless machine wants.
	virtual ui_manager *create_ui(running_machine &machine) override { return new ui_manager(machine); }
};

void worker_main()
{
	// Big-core pin (see m2vk_affinity.h) — the mask holds the whole big cluster, so the scheduler
	// still balances this worker and the emulation thread onto separate big cores.
	if (int const pinned = m2vk_pin_self_to_big_cores())
		osd_printf_verbose("[m2vk_snd] sound worker pinned to the %d big cores\n", pinned);

	// This whole thread hosts the secondary (sound-board) machine; keep it out of the frontend lua/UI.
	::mame_suppress_frontend_hooks(true);

	try
	{
		g_worker_opts = std::make_unique<emu_options>();
		g_worker_opts->set_value(OPTION_READCONFIG, false, OPTION_PRIORITY_MAXIMUM);
		g_worker_opts->set_value(OPTION_WRITECONFIG, false, OPTION_PRIORITY_MAXIMUM);
		g_worker_opts->set_value(OPTION_THROTTLE, false, OPTION_PRIORITY_MAXIMUM);
		g_worker_opts->set_value(OPTION_NVRAM_SAVE, false, OPTION_PRIORITY_MAXIMUM);
		g_worker_opts->set_value(OPTION_SAMPLERATE, 48000, OPTION_PRIORITY_MAXIMUM);
		// Deliberately NOT set_system_name("m1snd"): that setter validates against the generated driver
		// list, and this synthetic system is not in it ("Unknown system"). machine_config takes the
		// game_driver by reference below, so it needs no options.system() to find it.

		sound_osd osd;
		sound_manager_shim manager(*g_worker_opts, osd);
		// running_machine::run() unconditionally calls manager().http()->clear(); the frontend normally
		// creates the http_manager. Ours is inactive (the http option defaults off) but must exist.
		manager.start_http_server();
		// 🚨 The worker machine is built from a COPY of the m1snd descriptor, renamed to the main
		// system's short name, and that is a correctness fix rather than cosmetics.
		//
		// driver_device's constructor caches a search path by walking its own clone chain:
		//     driver_list::clone(m_system) -> { index = find(m_system); assert(index >= 0); ... }
		// m1snd is deliberately absent from the generated driver list (that is the point of the
		// anonymous namespace), so find() returns -1, the assert is compiled out of a release build,
		// and the call proceeds to driver(std::size_t(-1)) — an out-of-bounds index into
		// s_drivers_sorted. Undefined behaviour, and it behaves like it: on Windows it segfaults
		// inside driver_device's constructor before the worker machine exists, killing every model2o
		// set (daytona, desert, vcop) the moment the sound thread is on. macOS and Android happened to
		// read a survivable value off the end and carried on, which is why this shipped.
		//
		// The main system's name is in the list by construction, so find() succeeds and the walk is
		// over the real set's ancestry. The path it caches is then the one this board's ROMs would
		// actually live under, which is more right than "m1snd" ever was. game_driver::name is an
		// inline char array, so the copy owns its string; `worker_driver` is declared before the
		// config and the machine and so outlives both.
		game_driver worker_driver = GAME_NAME(m1snd);
		std::snprintf(worker_driver.name, sizeof(worker_driver.name), "%s", g_worker_driver_name);

		machine_config config(worker_driver, *g_worker_opts);
		running_machine machine(config, manager);
		manager.set_machine(&machine);

		machine.run(true);   // blocks; unwinds when update() schedules exit on g_stop
	}
	catch (std::exception const &ex)
	{
		osd_printf_error("[m2vk_snd] worker machine failed: %s\n", ex.what());
	}
	catch (...)
	{
		osd_printf_error("[m2vk_snd] worker machine failed with an unknown exception\n");
	}

	g_running.store(false, std::memory_order_release);
}

} // anonymous namespace

//============================================================
//  the exported API
//============================================================

void set_option_enabled(bool on)
{
	g_option = on ? 1 : 0;
	g_resolved = false;   // re-resolve on next enabled()
}

void engage_host()
{
	g_host_present = true;
}

bool enabled()
{
	if (!g_host_present)
		return false;   // no worker host in this build: never remove the board from the main machine
	if (!g_resolved)
	{
		if (env_present())
			g_enabled = env_on();
		else
			g_enabled = (g_option == 1);
		g_resolved = true;
	}
	return g_enabled;
}

void main_txd(const attotime &t, int state)
{
	if (!g_running.load(std::memory_order_acquire))
		return;
	g_to_sound.push(t.as_double(), state);
}

void prepare_main(running_machine &main)
{
	// Decide from the RUNNING machine, not from a flag set during config: the validity checker
	// (mame.cpp check_shared_source) builds every model2.cpp driver's config — daytona's included —
	// before the real machine runs, so a config-time flag leaks a model2o decision into the next game.
	// The threaded model2o branch adds a marker speaker "m2vk_snd_null"; its presence is the machine's
	// own, unambiguous answer. prepare_main() runs only for the real machine (validity never inits an OSD).
	g_board_split = (main.root_device().subdevice("m2vk_snd_null") != nullptr);
	if (!g_board_split)
		return;

	g_main_machine = &main;
	// The main UART, so sound->main replies can be delivered onto its RXD. Tag "uart" (model2.h). The
	// device object exists post-config even before its own device_start.
	g_main_uart = main.root_device().subdevice<i8251_device>("uart");
	g_main_baud = dynamic_cast<m2vk_baud_device *>(main.root_device().subdevice("uart_clock"));

	// sound -> main: allocate the reply line's timer on the MAIN machine NOW, while its save-state
	// registration is still open (this runs inside running_machine::start(), from osd().init()).
	// Allocating it later is refused by MAME. It is pumped and fires only on this (main) thread.
	g_to_main.attach(main, [](int state)
		{
			if (g_main_baud)
				g_main_baud->rxd_w(state);
			else if (g_main_uart)
				g_main_uart->write_rxd(state);
		});
}

void start(running_machine &main)
{
	if (!g_board_split)
		return;
	if (g_running.load(std::memory_order_acquire) || g_worker.joinable())
		return;

	// The name the worker's driver_device will be constructed under. It MUST resolve in the driver
	// list — see the long comment in worker_main() for what happens when it does not — so this is
	// checked here, on the main thread, where refusing to split the board is still an option.
	std::snprintf(g_worker_driver_name, sizeof(g_worker_driver_name), "%s", main.system().name);
	if (driver_list::find(g_worker_driver_name) < 0)
	{
		// Cannot happen — the main machine came out of the driver list — but the consequence of being
		// wrong is a segfault rather than a bad frame, so it is checked. The board has already been
		// removed from the main machine by the config hook at this point, so this arm is silent sound,
		// not unthreaded sound. Silent beats crashing.
		osd_printf_error("[m2vk_snd] '%s' is not in the driver list; the sound board will not run\n",
				g_worker_driver_name);
		return;
	}

	// Capture the main machine's already-loaded ROM regions; the worker fills its identical regions
	// from these before its CPU runs.
	for (int i = 0; i < 3; i++)
	{
		memory_region *const region = main.root_device().memregion(k_region_tag[i]);
		if (region != nullptr)
			g_rom[i] = { region->base(), region->bytes() };
		else
			g_rom[i] = { nullptr, 0 };
	}

	g_stop.store(false);
	g_main_time.store(0.0, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(g_ring_mutex);
		g_ring.clear();
	}
	g_running.store(true, std::memory_order_release);
	g_worker = std::thread(worker_main);

	osd_printf_verbose("[m2vk_snd] sound board threaded (M2VK_SOUND_THREAD)\n");
}

void stop()
{
	// Join the worker if one is running.
	if (g_worker.joinable())
	{
		g_stop.store(true);
		// Wake the worker wherever it is parked.
		{
			std::lock_guard<std::mutex> lock(g_pace_mutex);
			g_pace_cv.notify_all();
		}
		{
			std::lock_guard<std::mutex> lock(g_park_mutex);
			g_park_request.store(false);
			g_park_cv.notify_all();
		}
		g_worker.join();
	}

	// Reset per-machine state so the next game re-decides. Runs in osd_exit while the main machine is
	// still alive, so disarming g_to_main's timer on it is valid. Unconditional (even if no worker ran,
	// e.g. a split machine whose worker failed) so g_board_split never leaks into the next game.
	if (g_main_machine != nullptr)
		g_to_main.clear();
	g_board = nullptr;
	g_worker_machine = nullptr;
	g_main_uart = nullptr;
	g_main_baud = nullptr;
	g_main_machine = nullptr;
	g_board_split = false;
	g_running.store(false, std::memory_order_release);
}

bool running()
{
	return g_running.load(std::memory_order_acquire);
}

void pump_main(const attotime &main_time)
{
	if (!g_running.load(std::memory_order_acquire))
		return;

	// Deliver queued sound->main replies onto the main UART (main thread).
	g_to_main.pump();

	// Publish our time so the worker can pace itself against it, and wake it.
	g_main_time.store(main_time.as_double(), std::memory_order_release);
	std::lock_guard<std::mutex> lock(g_pace_mutex);
	g_pace_cv.notify_all();
}

const int16_t *pull_audio(int &sample_frames)
{
	sample_frames = 0;
	std::lock_guard<std::mutex> lock(g_ring_mutex);
	if (g_ring.empty())
		return nullptr;

	// Hand back up to a bounded chunk, copied so the pointer is stable while the worker keeps writing.
	const size_t frames = std::min<size_t>(g_ring.size() / 2, 48000 / 20);   // <= ~2 frames' worth
	g_pull_buf.assign(g_ring.begin(), g_ring.begin() + (frames * 2));
	g_pull_frames = frames;
	sample_frames = int(frames);
	return g_pull_buf.data();
}

void audio_consumed()
{
	std::lock_guard<std::mutex> lock(g_ring_mutex);
	const size_t n = std::min<size_t>(g_pull_frames * 2, g_ring.size());
	g_ring.erase(g_ring.begin(), g_ring.begin() + n);
	g_pull_frames = 0;
}

//============================================================
//  savestate (frontend thread, main emulation parked)
//============================================================

namespace {

// Park the worker at its safe point, run `fn` against the worker machine, release. Returns false if the
// worker never parked (not running, or shutting down).
template <typename Fn>
bool with_worker_parked(Fn &&fn)
{
	if (!g_running.load(std::memory_order_acquire))
		return false;

	g_park_request.store(true, std::memory_order_release);
	// Nudge the pacing wait so update() breaks out of it and reaches the park block. The main emulation
	// is parked while this runs, so g_main_time is frozen and the pace predicate would otherwise never
	// fire — the pace wait services g_park_request for exactly this reason (see update()).
	{
		std::lock_guard<std::mutex> lock(g_pace_mutex);
		g_pace_cv.notify_all();
	}

	// Block on g_park_cv until the worker announces parked (a real wait, not a spin: the old busy-spin
	// was a wall-clock race that lost when the worker thread was not scheduled within its 100k yields).
	// The 2 s cap is a safety net against a wedged worker, not the expected path — it parks within a
	// frame. g_parked is set by the worker under g_park_mutex, so checking it under the same lock here
	// cannot miss the announce.
	{
		std::unique_lock<std::mutex> lock(g_park_mutex);
		g_park_cv.wait_for(lock, std::chrono::seconds(2), [] {
			return g_parked.load(std::memory_order_acquire) || !g_running.load(std::memory_order_acquire);
		});
	}
	if (!g_parked.load(std::memory_order_acquire) || !g_running.load(std::memory_order_acquire))
	{
		// Never parked (timed out, or the worker is shutting down). Clear the request and wake the worker
		// in case it parks late, so it does not sit parked forever.
		g_park_request.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(g_park_mutex);
			g_park_cv.notify_all();
		}
		return false;
	}

	bool ok = false;
	if (g_worker_machine != nullptr)
		ok = fn(*g_worker_machine);

	// Release: clear the request and wake the worker out of its hold, under the lock so the store is
	// visible before the worker re-tests the predicate.
	{
		std::lock_guard<std::mutex> lock(g_park_mutex);
		g_park_request.store(false, std::memory_order_release);
		g_park_cv.notify_all();
	}
	return ok;
}

} // anonymous namespace

size_t state_size()
{
	size_t out = 0;
	with_worker_parked([&out](running_machine &m) {
		if (!m.save().registration_allowed() && m.save().registration_count() != 0)
			out = ram_state::get_size(m.save());
		return true;
	});
	return out;
}

bool state_save(void *dst, size_t size)
{
	return with_worker_parked([dst, size](running_machine &m) {
		return m.save().write_buffer(dst, size) == STATERR_NONE;
	});
}

bool state_load(const void *src, size_t size)
{
	return with_worker_parked([src, size](running_machine &m) {
		const bool ok = (m.save().read_buffer(src, size) == STATERR_NONE);
		// The in-flight serial queues are receiver residue, not part of the saved machine; clear them so
		// a load doesn't replay stale transitions.
		g_to_sound.clear();
		g_to_main.clear();
		return ok;
	});
}

} // namespace m2vk_snd
