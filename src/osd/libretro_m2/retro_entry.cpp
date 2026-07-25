// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the libretro.h entry points.

    MAME's frontend blocks: emulator_info::start_frontend() runs until the machine exits. libretro's
    contract is the opposite — retro_run() must advance exactly one frame and return. So emulation
    runs on its own thread and the two sides pass a baton:

        retro_run()                            emulation thread, in osd->update()
        ----------------------------------     -----------------------------------------
        publish input                          ... one emulated frame ...
        release_frame()   ------------------>  (wakes, runs the frame)
        wait_for_frame()  <------------------  capture frame + audio, post, park
        video_cb / audio_batch_cb

    Two failure modes this has to survive, both of which would otherwise hang the frontend:
      - the machine never starts (missing ROM, fatalerror). The emulation thread calls
        signal_died() as it unwinds, which releases any waiter.
      - the machine exits on its own (the user closes it, -str elapses). Same path.

    retro_load_game() blocks until the first frame, because retro_get_system_av_info() is called
    immediately afterwards and needs real geometry.

*********************************************************************************************************************************/

#include "libretro.h"

#include "libretro_m2_input.h"
#include "libretro_m2_osd.h"
#include "retro_options.h"

#include "renderer_vk/vk_context.h"
#include "renderer_vk/vk_funcs.h"
#include "renderer_vk/vk_present.h"

#include "emu.h"
#include "emuopts.h"
#include "main.h"

#include "corestr.h"

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>


namespace {

// How many frames retro_load_game will pump waiting for the first real picture before deciding
// the machine is not going to produce one. Generous: it only bounds a failure path.
constexpr int MAX_STARTUP_FRAMES = 120;

//============================================================
//  frontend callbacks
//============================================================

retro_environment_t     s_environ_cb = nullptr;
retro_video_refresh_t   s_video_cb = nullptr;
retro_audio_sample_t    s_audio_cb = nullptr;
retro_audio_sample_batch_t s_audio_batch_cb = nullptr;
retro_input_poll_t      s_input_poll_cb = nullptr;
retro_input_state_t     s_input_state_cb = nullptr;

void fallback_log(enum retro_log_level level, const char *fmt, ...) { }
retro_log_printf_t s_log_cb = fallback_log;


//============================================================
//  emulation thread state
//============================================================

std::unique_ptr<libretro_m2_options>      s_options;
std::unique_ptr<libretro_m2_osd_interface> s_osd;
std::thread                                s_emu_thread;
bool                                       s_running = false;

// Set at load if the renderer option asked for Vulkan and the frontend accepted the declaration.
// It decides which of the two presentation paths retro_run takes, and it is deliberately not the
// same question as "is there a context right now" — a declared context can be absent for the first
// frames and can be destroyed mid-run.
bool                                       s_hw_render = false;

// Runs the whole MAME frontend. Everything after start_frontend() returns is teardown.
void emu_thread_main(std::vector<std::string> args)
{
	try
	{
		emulator_info::start_frontend(*s_options, *s_osd, args);
	}
	catch (...)
	{
		s_log_cb(RETRO_LOG_ERROR, "[model2] emulation thread terminated with an exception\n");
	}

	// Release anyone waiting on a frame that will now never arrive.
	s_osd->signal_died();
}


//============================================================
//  content path -> MAME command line
//============================================================

// The frontend hands us a path such as .../roms/vf2.zip. MAME wants the system name and a rompath.
std::string system_name_from_path(const std::string &path)
{
	std::string name(path);
	const std::string::size_type slash = name.find_last_of("/\\");
	if (slash != std::string::npos)
		name.erase(0, slash + 1);
	const std::string::size_type dot = name.find_last_of('.');
	if (dot != std::string::npos)
		name.erase(dot);
	return name;
}

std::string rompath_from_path(const std::string &path)
{
	std::string dir(path);
	const std::string::size_type slash = dir.find_last_of("/\\");
	if (slash == std::string::npos)
		return std::string(".");
	dir.erase(slash);
	return dir;
}

// One of the frontend's directories, or empty if it has none to offer. Both of the queries used
// here are allowed to succeed and still hand back a null pointer, which means "no such directory";
// the caller has to treat that the same as failure.
std::string frontend_directory(unsigned query)
{
	const char *dir = nullptr;
	if ((s_environ_cb != nullptr) && s_environ_cb(query, &dir) && (dir != nullptr) && (*dir != '\0'))
		return std::string(dir);
	return std::string();
}


//============================================================
//  input descriptors
//============================================================

// What the frontend shows in its remapping UI. Deliberately generic: the actual meaning of each
// button is whatever MAME's per-game input ports make of IPT_BUTTONn, and this core spans
// fighters, driving games, lightguns and twin sticks. The two axes are named for the driving
// games, which are the sets where a wrong guess is most obvious.
const struct retro_input_descriptor INPUT_DESCRIPTORS[] = {
#define M2_PORT_DESCRIPTORS(port) \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Button 1" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Button 2" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Button 3" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Button 4" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Button 5" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Button 6" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Brake / Button 7" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Accelerator / Button 8" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Steering / Stick X" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Stick Y" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
	M2_PORT_DESCRIPTORS(0)
	M2_PORT_DESCRIPTORS(1)
#undef M2_PORT_DESCRIPTORS
	{ 0, 0, 0, 0, nullptr } };

} // anonymous namespace


//============================================================
//  libretro: callback registration
//============================================================

RETRO_API void retro_set_environment(retro_environment_t cb)
{
	s_environ_cb = cb;

	retro_log_callback log{};
	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log) && (log.log != nullptr))
		s_log_cb = log.log;

	bool no_content = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

	// Options are published here rather than in retro_init(): a frontend reads them before the
	// core is initialised, so that it can show them and restore the user's values first.
	m2opt::declare(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { s_video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { s_audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { s_audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { s_input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { s_input_state_cb = cb; }


//============================================================
//  libretro: identity
//============================================================

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name = "Model 2";
	info->library_version = emulator_info::get_bare_build_version();
	info->valid_extensions = "zip|7z";
	info->need_fullpath = true;
	info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	std::memset(info, 0, sizeof(*info));

	int width = 496, height = 384;
	if (s_osd)
		s_osd->framebuffer(width, height);
	if (width <= 0 || height <= 0)
	{
		width = 496;
		height = 384;
	}

	info->geometry.base_width = width;
	info->geometry.base_height = height;
	info->geometry.max_width = width;
	info->geometry.max_height = height;
	info->geometry.aspect_ratio = float(s_osd ? s_osd->aspect_ratio() : (4.0 / 3.0));

	info->timing.fps = s_osd ? s_osd->refresh_rate() : 60.0;
	info->timing.sample_rate = double(s_osd && s_osd->audio_rate() ? s_osd->audio_rate() : 48000);
}


//============================================================
//  libretro: lifecycle
//============================================================

RETRO_API void retro_init(void)
{
	// Says which headers the Vulkan side was built against, before any of it has run. What the
	// frontend's implementation actually is gets logged at context_reset, and the two are allowed
	// to differ — but when something goes wrong there, this is the other half of the comparison.
	s_log_cb(RETRO_LOG_INFO, "[model2] %s\n", m2vk::vk_build_info());
}

RETRO_API void retro_deinit(void) { }

// Every port is a RetroPad and nothing else. The analogue sticks and triggers are always read, so
// RETRO_DEVICE_ANALOG and RETRO_DEVICE_JOYPAD are the same device here and there is nothing to
// switch between.
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { }

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	if (s_running)
		return false;
	if ((game == nullptr) || (game->path == nullptr))
	{
		s_log_cb(RETRO_LOG_ERROR, "[model2] no content path supplied\n");
		return false;
	}

	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!s_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
	{
		s_log_cb(RETRO_LOG_ERROR, "[model2] frontend rejected XRGB8888\n");
		return false;
	}

	s_environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<struct retro_input_descriptor *>(INPUT_DESCRIPTORS));

	const std::string path(game->path);
	const std::string system = system_name_from_path(path);

	// Options are read once, here. Both of them are settled before the machine starts — the
	// renderer picks a draw path, the service buttons are baked into the input devices' default
	// assignments — so a change made later is reported by retro_run() and applied at the next load.
	const std::string renderer = m2opt::get(s_environ_cb, m2opt::KEY_RENDERER);
	const bool service_buttons = m2opt::get_bool(s_environ_cb, m2opt::KEY_SERVICE_BUTTONS);

	s_log_cb(RETRO_LOG_INFO, "[model2] options: %s=%s %s=%s\n",
			m2opt::KEY_RENDERER, renderer.c_str(),
			m2opt::KEY_SERVICE_BUTTONS, service_buttons ? "enabled" : "disabled");

	// Hardware render is declared here, before the machine starts, and only when the option asked
	// for it: with renderer=software the core must not declare it at all, so that the P1
	// presentation path stays byte-for-byte the path the software goldens were generated with.
	//
	// A refusal is not an error. retrohost is a frontend with no hardware render, and RetroArch with
	// a GL video driver is another; both keep running on the software path.
	s_hw_render = false;
	if (renderer != "software")
	{
		s_hw_render = m2vk::declare_hw_render(s_environ_cb, s_log_cb);
		if (!s_hw_render)
			s_log_cb(RETRO_LOG_WARN, "[model2] this frontend has no Vulkan context to offer; using the software renderer\n");
	}

	// The content's own directory, plus a place for sets the frontend keeps alongside the core.
	// The second entry is what makes a clone loadable when its parent set lives elsewhere.
	std::string rompath = rompath_from_path(path);
	const std::string systemdir = frontend_directory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
	if (!systemdir.empty())
		rompath += ";" + systemdir + "/model2";

	// MAME paces itself against a wall clock by default; here the frontend does the pacing, so
	// throttling and frame-skipping have to be off or the two clocks fight.
	//
	// -noreadconfig is about reproducibility as much as tidiness: MAME's default inipath includes
	// the working directory and $HOME/.mame, so without it a stray mame.ini belonging to someone's
	// standalone MAME silently changes how the core runs — and the A/B harness would never know.
	// It gates ini files only; the per-game cfg that carries input remaps still loads.
	std::vector<std::string> args{
		"model2",
		system,
		"-rompath", rompath,
		"-noreadconfig",
		"-video", "none",
		"-nothrottle",
		"-sound", "auto",
		"-samplerate", "48000",
		"-skip_gameinfo",
		"-nomouse",
	};

	// MAME writes NVRAM, per-game input remaps, snapshots and the rest to paths relative to the
	// process's working directory — which belongs to the frontend, not to us. Keep all of it inside
	// the frontend's save directory instead. MAME creates the directories on first write.
	//
	// This matters beyond tidiness: Model 2 sets keep credits and settings in battery RAM, so where
	// the NVRAM lands decides whether a given (rom, frame) fixture is reproducible at all.
	const std::string savedir = frontend_directory(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
	if (!savedir.empty())
	{
		const std::string base = savedir + "/model2";
		const std::pair<char const *, char const *> dirs[] = {
			{ "-nvram_directory",    "nvram" },
			{ "-cfg_directory",      "cfg" },
			{ "-input_directory",    "inp" },
			{ "-state_directory",    "sta" },
			{ "-diff_directory",     "diff" },
			{ "-snapshot_directory", "snap" } };
		for (auto [option, leaf] : dirs)
		{
			args.emplace_back(option);
			args.emplace_back(base + "/" + leaf);
		}
	}
	else
	{
		s_log_cb(RETRO_LOG_WARN, "[model2] the frontend offers no save directory; NVRAM and settings will be written relative to the working directory\n");
	}

	s_options = std::make_unique<libretro_m2_options>();
	s_osd = std::make_unique<libretro_m2_osd_interface>(*s_options);
	s_osd->set_service_buttons(service_buttons);
	s_running = true;
	s_emu_thread = std::thread(emu_thread_main, args);

	// Block until the machine has produced a frame with an actual picture in it:
	// retro_get_system_av_info() runs straight after this and needs real geometry and timing,
	// and the first frame or two arrive before the screen bitmap exists. A failure to start
	// unblocks us via signal_died() rather than hanging.
	bool have_picture = false;
	for (int i = 0; (i < MAX_STARTUP_FRAMES) && !have_picture; i++)
	{
		if (i != 0)
			s_osd->release_frame();
		if (!s_osd->wait_for_frame())
			break;
		int w = 0, h = 0;
		have_picture = (s_osd->framebuffer(w, h) != nullptr) && (w > 0) && (h > 0);
	}

	if (!have_picture)
	{
		s_log_cb(RETRO_LOG_ERROR, "[model2] '%s' failed to start\n", system.c_str());
		s_osd->request_exit();
		if (s_emu_thread.joinable())
			s_emu_thread.join();
		s_running = false;
		s_hw_render = false;
		m2vk::forget_hw_render();
		s_osd.reset();
		s_options.reset();
		return false;
	}

	s_log_cb(RETRO_LOG_INFO, "[model2] started '%s'\n", system.c_str());
	return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
	return false;
}

RETRO_API void retro_unload_game(void)
{
	if (!s_running)
		return;

	s_osd->request_exit();
	if (s_emu_thread.joinable())
		s_emu_thread.join();

	s_running = false;
	s_hw_render = false;

	// The frontend normally fires context_destroy before this; make the state safe whether or not it
	// did, and stop a context_reset arriving for a machine that no longer exists.
	m2vk::forget_hw_render();

	s_osd.reset();
	s_options.reset();
}

RETRO_API void retro_reset(void)
{
	if (s_running)
		s_osd->request_reset();
}


//============================================================
//  libretro: the frame
//============================================================

RETRO_API void retro_run(void)
{
	if (!s_running)
		return;

	// Neither option can be applied to a machine that is already running, so acknowledge the change
	// rather than half-honouring it. The frontend clears the flag as this reads it, so it is one
	// line per change, not one per frame.
	if (m2opt::updated(s_environ_cb))
		s_log_cb(RETRO_LOG_INFO, "[model2] core options changed; they take effect the next time content is loaded\n");

	if (s_input_poll_cb != nullptr)
		s_input_poll_cb();

	// Snapshot the pads into the input module before the emulation thread is let go. It is parked
	// on the baton right now, which is what makes writing its device state here safe — and it also
	// means a frame sees exactly one input sample, so a run stays reproducible.
	if (libretro_m2_input *const input = s_osd->input())
		input->poll_frontend(s_input_state_cb);

	// let the emulation thread run one frame, then wait for it to park again
	s_osd->release_frame();
	if (!s_osd->wait_for_frame())
	{
		// the machine exited on its own; tell the frontend to shut the core down
		s_environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
		s_running = false;
		return;
	}

	int width = 0, height = 0;
	const uint32_t *const pixels = s_osd->framebuffer(width, height);
	const bool have_picture = (pixels != nullptr) && (width > 0) && (height > 0);

	if (s_hw_render)
	{
		// With hardware render declared, libretro allows only RETRO_HW_FRAME_BUFFER_VALID or null
		// through video_cb (libretro.h:946) — a software pointer is not an option. So a dupe is the
		// only thing to say when the renderer has no image, which is the normal state of affairs for
		// the first frame or two of every run: context_reset does not fire until after
		// retro_load_game has returned.
		//
		// The renderer is handed the same buffer the software path passes to video_cb, and uploads it
		// as a texture. Until P3 that is the whole of the difference between the two paths.
		if (have_picture && m2vk::present_frame(pixels, unsigned(width), unsigned(height)))
			s_video_cb(RETRO_HW_FRAME_BUFFER_VALID, unsigned(width), unsigned(height), 0);
		else
			s_video_cb(nullptr, 0, 0, 0); // frame duped
	}
	else
	{
		if (have_picture)
			s_video_cb(pixels, unsigned(width), unsigned(height), size_t(width) * sizeof(uint32_t));
		else
			s_video_cb(nullptr, 0, 0, 0); // frame duped
	}

	int samples = 0;
	const int16_t *const audio = s_osd->frame_audio(samples);
	if ((audio != nullptr) && (samples > 0) && (s_audio_batch_cb != nullptr))
		s_audio_batch_cb(audio, size_t(samples));
}


//============================================================
//  libretro: savestates — deferred past P1
//
//  No Model 2 set in src/mame/sega/model2.cpp carries MACHINE_SUPPORTS_SAVE, so MAME does not
//  claim its state registration is complete for this driver. Rather than ship a savestate that
//  silently corrupts, the A/B harness keys fixtures on frame number, which P0 measured to be
//  bit-repeatable. See devnotes/p1-libretro-core.md.
//============================================================

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { return false; }


//============================================================
//  libretro: the rest of the ABI
//============================================================

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { }
