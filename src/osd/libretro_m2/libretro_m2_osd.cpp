// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "libretro_m2_osd.h"

#include "libretro_m2_input.h"
#include "m2vk_frame.h"
#include "m2vk_reticle.h"
#include "m2vk_steerbar.h"
#include "m2vk_sink.h"

#include "emu.h"
#include "emuopts.h"
#include "render.h"
#include "screen.h"
#include "dipalette.h"

// after emu.h, which they read MAME's ioport / running_machine types out of and which only a .cpp
// may include
#include "m2vk_analog.h"
#include "m2vk_gunlog.h"
#include "m2vk_inputdump.h"
#include "m2vk_savestate.h"
#include "m2vk_steer.h"
#include "m2vk_twinstick.h"

#include "modules/monitor/monitor_module.h"
#include "modules/output/output_module.h"
#include "modules/render/render_module.h"

#include "osdcore.h"

#include <cstring>


namespace {

// A font that reports nothing. MAME's UI asks the OSD for a font before it knows whether it
// will draw anything; returning a font that opens successfully but has no glyphs keeps the UI
// code happy without pulling in a real font backend. Nothing in a libretro core draws MAME's UI.
class null_font : public osd_font
{
public:
	virtual bool open(std::string const &font_path, std::string const &name, int &height) override
	{
		height = 16;
		return true;
	}

	virtual void close() override { }

	virtual bool get_bitmap(char32_t chnum, bitmap_argb32 &bitmap, std::int32_t &width, std::int32_t &xoffs, std::int32_t &yoffs) override
	{
		bitmap.reset();
		width = 0;
		xoffs = 0;
		yoffs = 0;
		return false;
	}
};

} // anonymous namespace


//============================================================
//  construction / destruction
//============================================================

libretro_m2_osd_interface::libretro_m2_osd_interface(libretro_m2_options &options)
	: osd_common_t(options)
	, m_options(options)
{
}

libretro_m2_osd_interface::~libretro_m2_osd_interface()
{
}


//============================================================
//  init
//============================================================

void libretro_m2_osd_interface::init(running_machine &machine)
{
	osd_common_t::init(machine);
	init_subsystems();

	// Brackets the polygon stream tapped in model2_v.cpp with this machine's run. Opening it here
	// rather than letting the first rendered frame do it is what lets a game that renders no 3D at
	// all be reported as such — the sink is there to say so either way.
	m2vk::sink_open();

	m_started.store(true, std::memory_order_release);
}

// Stands in for osd_common_t::init_subsystems(), which selects a monitor module and brings up a
// window. Neither exists here, and neither can: the frontend owns the display. Only the two
// modules osd_common_t actually dereferences are constructed, straight from their factories.
void libretro_m2_osd_interface::init_subsystems()
{
	// m_render stays null. Even the "none" renderer (drawnone.cpp) is a renderer *for a window*
	// — it calls osd_window::pixel_aspect() — and this core has no windows. Nothing in
	// osd_common_t dereferences m_render; it is only ever assigned, in the init_subsystems()
	// this replaces. Frames are read straight off the screen bitmap in capture_frame().

	{
		extern const module_type OUTPUT_NONE;
		m_output_module = OUTPUT_NONE();
		m_output_module->init(*this, options());
		m_output = dynamic_cast<output_module *>(m_output_module.get());
	}

	input_init();

	alloc_screen_target();

	// Sample the screen's timing here, not in capture_frame(): retro_get_system_av_info() runs
	// as soon as the first frame lands, and capture_frame() bails out early on the first frame
	// or two (the bitmap is not RGB32 yet), which would leave the frontend running the core at
	// a default 60 Hz instead of Model 2's 57.52.
	screen_device_enumerator screens(machine().root_device());
	screen_device *const screen = screens.first();
	if (screen != nullptr)
	{
		const attoseconds_t period = screen->frame_period().as_attoseconds();
		if (period > 0)
			m_refresh_rate = ATTOSECONDS_TO_HZ(period);
		const rectangle &vis = screen->visible_area();
		if ((vis.width() > 0) && (vis.height() > 0))
			m_aspect_ratio = double(vis.width()) / double(vis.height());
	}
}

// Replaces osd_common_t::input_init(), which calls input_init() on four separately selected
// modules. There is only one here — the RetroPad module — so it is constructed, initialised and
// aimed at all four pointers. The pointers matter only for completeness: input_update() below
// polls the module directly rather than going through poll_input_modules().
bool libretro_m2_osd_interface::input_init()
{
	m_input = std::make_unique<libretro_m2_input>(m_diagnostic);
	if (m_input->init(*this, options()) != 0)
	{
		osd_printf_error("libretro input module failed to initialise\n");
		m_input.reset();
		return false;
	}

	m_keyboard_input = m_input.get();
	m_mouse_input = m_input.get();
	m_lightgun_input = m_input.get();
	m_joystick_input = m_input.get();

	m_input->input_init(machine());
	return true;
}

// Called from video_manager once per frame (video.cpp:222,251). The module's poll() is a no-op —
// the item pointers address its state directly and retro_run() writes that state — but going
// through poll_if_necessary() keeps the module's own bookkeeping honest.
void libretro_m2_osd_interface::input_update(bool relative_reset)
{
	if (m_input)
		m_input->poll_if_necessary(relative_reset);
}

void libretro_m2_osd_interface::check_osd_inputs()
{
	// MAME's own UI hotkeys (fullscreen, throttle, snapshot...) belong to the frontend here
}

// MACHINE_NOTIFY_EXIT, registered by osd_common_t::init(). Everything torn down here holds a
// reference into the machine that is going away: the input devices are registered with
// machine().input(), and the render_target belongs to machine().render().
void libretro_m2_osd_interface::osd_exit()
{
	// last point at which the run's polygon stream is complete
	m2vk::sink_close();

	// the cached savestate size belongs to the machine going away
	m2vk::state_close();

	// the gun read-out holds ioport pointers into the machine being torn down
	m2vk::gun_log_close();

	// same for the steering detector's paddle fields — a second load re-decides
	m2vk::steer_close();

	// same for the analog-stick detector — a second load re-decides against the new set's fields
	m2vk::analog_close();

	// same for the twin-AD-stick binding — it holds no pointers, but a second load must re-decide
	// against the new set's fields
	m2vk::twin_stick_close();

	if (m_input)
	{
		m_input->exit();
		m_input.reset();
	}
	m_keyboard_input = nullptr;
	m_mouse_input = nullptr;
	m_lightgun_input = nullptr;
	m_joystick_input = nullptr;

	free_screen_target();

	osd_common_t::osd_exit();
}


//============================================================
//  the screen render_target
//============================================================

// A render_target with the default view marks the emulated screen visible, so video_manager keeps
// invoking its SCREEN_UPDATE callback even though nothing is drawn through the target. Without it
// the bitmap read in capture_frame() would stay blank.
void libretro_m2_osd_interface::alloc_screen_target()
{
	if (m_target != nullptr)
		return;

	m_target = machine().render().target_alloc(nullptr, 0);
	if (m_target != nullptr)
	{
		screen_device_enumerator screens(machine().root_device());
		screen_device *const screen = screens.first();
		if (screen != nullptr)
			m_target->set_bounds(screen->visible_area().width(), screen->visible_area().height());
		else
			m_target->set_bounds(496, 384);
	}
}

void libretro_m2_osd_interface::free_screen_target()
{
	if (m_target != nullptr)
	{
		machine().render().target_free(m_target);
		m_target = nullptr;
	}
}


//============================================================
//  update — one emulated frame has finished; hand it over
//============================================================

void libretro_m2_osd_interface::update(bool skip_redraw)
{
	osd_common_t::update(skip_redraw); // watchdog reset

	// Honor a cross-thread exit request here, on the emulation thread. m_exiting is sticky:
	// schedule_exit() only takes effect at the end of the timeslice, so update() is called
	// several more times on the way out. Those calls must NOT park on the baton — retro_unload_game
	// has already stopped releasing frames and gone to join(), so parking would deadlock. That is
	// exactly what the first version of this did.
	if (!m_exiting && m_exit_requested.load(std::memory_order_acquire))
	{
		machine().schedule_exit();
		m_exiting = true;
	}

	// Same story for retro_reset(): schedule_soft_reset() is only safe to call from the emulation
	// thread, so the libretro side just sets a flag and this picks it up at the frame boundary.
	if (!m_exiting && m_reset_requested.exchange(false, std::memory_order_acq_rel))
		machine().schedule_soft_reset();

	// 🚨 An update() reached before the machine is RUNNING must NOT cost the frontend a frame, and
	// this is not tidiness — it is the fix for the run-to-run nondeterminism that made five fixtures
	// "bistable".
	//
	// MAME pumps video_manager::frame_update() while it is still loading ROMs:
	// romload.cpp:649 calls set_startup_text(..., force=false), which (ui.cpp:916) forwards to
	// frame_update() whenever more than a TENTH OF A WALL-CLOCK SECOND has passed since the last
	// one. So the number of those pumps is floor(rom_load_seconds * 10) — a host-speed measurement,
	// not an emulated quantity. Each one used to park on the baton, i.e. cost retro_run() a whole
	// frame, so the frontend was handed 5 OR 6 duplicate startup frames depending on how the disk
	// felt, and every host frame for the rest of the session then mapped to a DIFFERENT emulated
	// frame. Measured on vcop2: `frame - update_count` settles at -6 or -7, deterministic within a
	// run and a coin flip between runs.
	//
	// That is what every frame-indexed comparison in devnotes/ was silently assuming could not
	// happen, and it is why two runs of one command could produce two stable digests: not frame
	// parity in draw_framebuffer (which only exists in render test mode and cannot explain waverunr)
	// but a ±1 shift in which emulated frame each host frame lands on.
	//
	// Returning early is safe with retro_load_game()'s startup loops: they drive the machine by
	// release_frame()/wait_for_frame(), and an early release is discarded because the parking branch
	// below clears m_go itself, under the lock, before it waits. So the first frame the frontend
	// ever sees is the first RUNNING one — which is also after save_manager::allow_registration(false),
	// so the save registry is already closed when retro_load_game asks for the state size.
	//
	// ⚠️ It cannot stall retro_run() on a reset either: soft_reset() (machine.cpp:967-979) sets RESET
	// and RUNNING inside one call and pumps no video between them, so no update() is ever reached at
	// that phase. EXIT is above RUNNING, so the shutdown path — which must not park, see m_exiting
	// above — is unaffected by this test.
	if (machine().phase() < machine_phase::RUNNING)
		return;

	// The lightgun read-out (M2VK_GUN_LOG), taken here so that it reports the state the frame being
	// handed over was drawn from. Off unless the variable is set, and it reads ports rather than
	// writing anything, so a run without it is unchanged.
	m2vk::gun_log_frame(machine());

	// Steering detector + M2VK_STEER_LOG read-out. Here rather than in input_init() because the port
	// list is empty there — same trap as the gun read-out above.
	m2vk::steer_frame(machine());

	// Analog-stick detector (IPT_AD_STICK). Here rather than in input_init() for the same empty-port-list
	// reason as the steering detector above.
	m2vk::analog_frame(machine());

	// Single-pad twin-AD-stick binding (cybsled's right tread is player 2's IPT_AD_STICK). Same
	// once-from-update() reason: the port list is empty at input_init(). See m2vk_twinstick.h.
	m2vk::twin_stick_frame(machine());

	// The layout editor's data source (M2VK_INPUT_DUMP), one-shot. Here rather than in input_init() for
	// the same reason the read-out above resolves here: osd().init() runs before
	// ioport_manager::initialize(), so a dump taken there reports no fields at all on a set that has
	// twenty. See m2vk_inputdump.h.
	m2vk::input_dump_frame(machine());

	// M2VK_SAVE_LOG's one-shot report. Here rather than in init() because the save registry is still
	// being filled while devices start, and osd->init() runs inside that window.
	m2vk::state_log(machine());

	if (!skip_redraw)
		capture_frame();

	// Hand the frame to retro_run() and park until it asks for the next one. Everything the
	// consumer reads (m_fb, m_audio) is written above and not touched again until it releases
	// us, so the baton is the only synchronisation needed.
	{
		std::unique_lock<std::mutex> lock(m_baton);
		m_frame_ready = true;
		m_cv.notify_all();
		if (!m_exiting)
		{
			m_go = false;
			m_cv.wait(lock, [this] { return m_go || m_died; });
		}
	}

	// the next frame's audio accumulates from empty
	m_audio.clear();
}

//============================================================
//  savestates — forwarded, with the one guard that matters
//============================================================

// m_started is set at the end of init() and the machine is torn down through osd_exit(), so this
// pair brackets every point at which machine() is valid. A frontend is entitled to call
// retro_serialize_size() early — RetroArch does, right after retro_load_game — and answering 0 is
// how a core says "not yet".
size_t libretro_m2_osd_interface::state_size()
{
	return machine_started() ? m2vk::state_size(machine()) : 0;
}

bool libretro_m2_osd_interface::state_save(void *data, size_t size)
{
	return machine_started() && m2vk::state_save(machine(), data, size);
}

bool libretro_m2_osd_interface::state_load(void const *data, size_t size)
{
	return machine_started() && m2vk::state_load(machine(), data, size);
}


void libretro_m2_osd_interface::capture_frame()
{
	screen_device_enumerator screens(machine().root_device());
	screen_device *const screen = screens.first();
	if (screen == nullptr)
		return;

	// Sample the refresh rate here rather than at init(): the screen's frame period is not
	// configured from the driver's set_raw() until the screen device has started, which is after
	// osd->init() runs. Sampling too early reports a default 60 Hz instead of Model 2's 57.52.
	const attoseconds_t period = screen->frame_period().as_attoseconds();
	if (period > 0)
		m_refresh_rate = ATTOSECONDS_TO_HZ(period);

	screen_bitmap &sb = screen->curbitmap();
	const bitmap_format fmt = sb.format();
	// Model 2 and namcos22 present RGB32; namcos21 (Star Blade et al.) draws a palettized IND16.
	if (fmt != BITMAP_FORMAT_RGB32 && fmt != BITMAP_FORMAT_IND16)
		return;

	// Copy the VISIBLE area, not the whole bitmap. curbitmap() is the full raster — 656x424 for
	// Model 2 — while the picture is the 496x384 visible rectangle. Handing the frontend the full
	// bitmap while advertising the visible geometry in av_info renders as garbage.
	const rectangle &vis = screen->visible_area();
	const int w = vis.width();
	const int h = vis.height();
	const int bw = (fmt == BITMAP_FORMAT_RGB32) ? sb.as_rgb32().width()  : sb.as_ind16().width();
	const int bh = (fmt == BITMAP_FORMAT_RGB32) ? sb.as_rgb32().height() : sb.as_ind16().height();
	if ((w <= 0) || (h <= 0) || (vis.max_x >= bw) || (vis.max_y >= bh))
		return;

	m_aspect_ratio = double(w) / double(h);

	if (m_fb_w != w || m_fb_h != h)
	{
		m_fb_w = w;
		m_fb_h = h;
		m_fb.resize(size_t(w) * size_t(h));
	}

	if (fmt == BITMAP_FORMAT_RGB32)
	{
		bitmap_rgb32 &bm = sb.as_rgb32();
		for (int y = 0; y < h; y++)
			std::memcpy(&m_fb[size_t(y) * w], &bm.pix(vis.min_y + y, vis.min_x), size_t(w) * sizeof(uint32_t));
	}
	else
	{
		// Palettized (namcos21): resolve each 16-bit index through the screen's own palette to the
		// XRGB8888 the frontend expects. pen_t is already 0xAARRGGBB with alpha forced opaque.
		//
		// NOT screen->palette().pens(): screen.set_palette() stamps the device_palette_interface's
		// m_format to the screen's own bitmap format (IND16 here), and for BITMAP_FORMAT_IND16
		// allocate_color_tables() replaces pens() with a dummy 1:1 index identity table (dipalette.cpp) —
		// it is meant for callers that go on to do their own indirection, not as a resolved colour array.
		// The real per-pen RGB, for either bitmap format, is palette()->entry_list_adjusted().
		if (!screen->has_palette())
			return;
		const pen_t *const pens = reinterpret_cast<const pen_t *>(screen->palette().palette()->entry_list_adjusted());
		bitmap_ind16 &bm = sb.as_ind16();
		for (int y = 0; y < h; y++)
		{
			const uint16_t *const src = &bm.pix(vis.min_y + y, vis.min_x);
			uint32_t *const dst = &m_fb[size_t(y) * w];
			for (int x = 0; x < w; x++)
				dst[x] = pens[src[x]];
		}
	}

	// The lightgun reticle, for the software path only — MAME draws no crosshair this OSD can see
	// (m2vk_reticle.h), and this buffer is what renderer=software hands the frontend.
	//
	// m2vk::capturing() is precisely "the Vulkan path is compositing", which is the question being
	// asked: that path never presents this buffer except for the frame or two before the 2D layers
	// exist, and it draws the same cross itself, after the foreground layer where it belongs. Blitting
	// here as well would put one into MAME's frame, where the hardware renderer's 3D would then be
	// composited over the top of it.
	if (!m2vk::capturing())
	{
		m2vk::reticle_blit(m_fb.data(), w, h);
		// Steering bar over the reticle, matching the Vulkan draw order.
		m2vk::steerbar_blit(m_fb.data(), w, h);
	}
}


//============================================================
//  the baton
//============================================================

bool libretro_m2_osd_interface::wait_for_frame()
{
	std::unique_lock<std::mutex> lock(m_baton);
	m_cv.wait(lock, [this] { return m_frame_ready || m_died; });
	if (m_died)
		return false;
	m_frame_ready = false;
	return true;
}

void libretro_m2_osd_interface::release_frame()
{
	std::lock_guard<std::mutex> lock(m_baton);
	m_go = true;
	m_cv.notify_all();
}

void libretro_m2_osd_interface::request_reset()
{
	m_reset_requested.store(true, std::memory_order_release);
}

void libretro_m2_osd_interface::request_exit()
{
	m_exit_requested.store(true, std::memory_order_release);
	release_frame();
}

// Called as the emulation thread unwinds — including the failure paths, where the machine never
// started and no frame was ever produced. Without this a missing ROM would leave the libretro
// thread blocked in wait_for_frame() forever.
void libretro_m2_osd_interface::signal_died()
{
	std::lock_guard<std::mutex> lock(m_baton);
	m_died = true;
	m_cv.notify_all();
}

const uint32_t *libretro_m2_osd_interface::framebuffer(int &width, int &height) const
{
	width = m_fb_w;
	height = m_fb_h;
	return m_fb.empty() ? nullptr : m_fb.data();
}

const int16_t *libretro_m2_osd_interface::frame_audio(int &samples_this_frame) const
{
	// m_audio holds interleaved stereo, so two int16 per sample frame
	samples_this_frame = int(m_audio.size() / 2);
	return m_audio.empty() ? nullptr : m_audio.data();
}


//============================================================
//  audio
//============================================================

// One node, one stereo sink, rate 0 meaning "use the configured sample rate" — MAME then hands
// machine().sample_rate() to stream_sink_open. Mirrors what js_sound advertises.
osd::audio_info libretro_m2_osd_interface::sound_get_information()
{
	osd::audio_info result;
	result.m_generation = 1;
	result.m_default_sink = 1;
	result.m_default_source = 0;
	result.m_nodes.resize(1);
	result.m_nodes[0].m_name = "libretro";
	result.m_nodes[0].m_display_name = "libretro frontend";
	result.m_nodes[0].m_id = 1;
	result.m_nodes[0].m_rate.m_default_rate = 0;
	result.m_nodes[0].m_rate.m_min_rate = 0;
	result.m_nodes[0].m_rate.m_max_rate = 0;
	result.m_nodes[0].m_sinks = 2;
	result.m_nodes[0].m_sources = 0;
	result.m_nodes[0].m_port_names.reserve(2);
	result.m_nodes[0].m_port_names.emplace_back("L");
	result.m_nodes[0].m_port_names.emplace_back("R");
	result.m_nodes[0].m_port_positions.reserve(2);
	result.m_nodes[0].m_port_positions.emplace_back(osd::channel_position::FL());
	result.m_nodes[0].m_port_positions.emplace_back(osd::channel_position::FR());
	if (m_audio_stream_id != 0)
	{
		result.m_streams.resize(1);
		result.m_streams[0].m_id = m_audio_stream_id;
		result.m_streams[0].m_node = 1;
	}
	return result;
}

uint32_t libretro_m2_osd_interface::sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate)
{
	if (m_audio_stream_id != 0)
		return 0; // a single stream only

	m_audio_rate = rate;
	m_audio_stream_id = m_audio_next_id++;
	return m_audio_stream_id;
}

void libretro_m2_osd_interface::sound_stream_close(uint32_t id)
{
	if (id == m_audio_stream_id)
		m_audio_stream_id = 0;
}

void libretro_m2_osd_interface::sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame)
{
	if (id != m_audio_stream_id || buffer == nullptr || samples_this_frame <= 0)
		return;

	// The sink is stereo, so the buffer holds samples_this_frame frames of interleaved int16.
	// This runs on the emulation thread before update() hands the frame over, and m_audio is
	// cleared once the consumer has released us, so appending here needs no lock.
	m_audio.insert(m_audio.end(), buffer, buffer + (size_t(samples_this_frame) * 2));
}


//============================================================
//  font
//============================================================

//============================================================
//  free functions every OSD has to provide
//
//  Declared in emuopts.h and osdcore.h, referenced from the frontend and the Lua engine, so
//  they must exist even though neither means anything without a window.
//============================================================

void osd_setup_osd_specific_emu_options(emu_options &opts)
{
	opts.add_entries(osd_options::s_option_entries);
}

void osd_set_aggressive_input_focus(bool aggressive_focus)
{
	// no window to grab focus for
}


osd_font::ptr libretro_m2_osd_interface::font_alloc()
{
	return std::make_unique<null_font>();
}

bool libretro_m2_osd_interface::get_font_families(std::string const &font_path, std::vector<std::pair<std::string, std::string> > &result)
{
	result.clear();
	return true;
}
