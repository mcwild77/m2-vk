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

#include "m2vk_frame.h"
#include "m2vk_reticle.h"
#include "m2vk_sink.h"

#include "renderer_vk/vk_context.h"
#include "renderer_vk/vk_funcs.h"
#include "renderer_vk/vk_present.h"

#include "emu.h"
#include "emuopts.h"
#include "main.h"

#include "corestr.h"

#include <cstdlib>
#include <cstring>
#include <iterator>
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
//
// One set of labels for both pad layouts, and they describe Classic. Descriptors are sent once when
// content is loaded and a layout is a per-port choice the player can change at any moment, so the
// alternative is not "labels that follow the layout" but "two arrays, one of them stale". The two
// that Modern moves are the shoulder pair.
//
// R3 is listed and L3 is not, for the same reason: R3 is daytona's fourth view button under every
// option value, and what L3 does depends on model2_diagnostic_input — a label that is wrong in the
// default configuration is worse than no label.
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
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "View / Button 9" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Steering / Stick X" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Stick Y" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" }, \
	{ port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
// The gun controls, on the two ports that can have a gun — see MAX_GUNS. The pad descriptors above
// still apply to a gun port: a port set to RETRO_DEVICE_LIGHTGUN keeps its RetroPad buttons here
// (libretro_m2_input.cpp gates the stick and nothing else), which is what keeps coin, start and the
// service switches reachable on a gun cabinet.
#define M2_GUN_DESCRIPTORS(port) \
	{ port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, "Trigger" }, \
	{ port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A,   "Button 2" }, \
	{ port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_B,   "Button 3" },

	M2_PORT_DESCRIPTORS(0)
	M2_GUN_DESCRIPTORS(0)
	M2_PORT_DESCRIPTORS(1)
	M2_GUN_DESCRIPTORS(1)
	// Ports 2 and 3 exist for airwlkrs, the one four-player cabinet — see MAX_PADS. Every other set
	// leaves them bound to input types it does not declare, which costs nothing.
	M2_PORT_DESCRIPTORS(2)
	M2_PORT_DESCRIPTORS(3)
#undef M2_GUN_DESCRIPTORS
#undef M2_PORT_DESCRIPTORS
	{ 0, 0, 0, 0, nullptr } };


//============================================================
//  controller types
//============================================================

// What a port can be set to. Every port offers the same list, because from the core's side every
// port is the same hardware and whether a gun means anything is a property of the loaded set rather
// than of the port — a gun on vf2 simply binds to types vf2 does not declare.
//
// The two pad entries are FBNeo's Classic and Modern layouts and differ only in where MAME buttons
// 5 and 6 sit; buttons 1-4, the d-pad, the sticks, coin and start are identical. Classic is plain
// RETRO_DEVICE_JOYPAD rather than a subclass of its own, so it is what a frontend that knows
// nothing about this list ends up with. 6-Panel is deliberately not offered: it exists for
// six-button fighters, and the whole platform's button histogram is 37/30/27/11/4/3/1/1 for
// IPT_BUTTON1..8 — vf2 is a three-button game. devnotes/lightgun.md §2.5.1.
const struct retro_controller_description PORT_DEVICES[] = {
	{ "RetroPad (Classic)", RETRO_DEVICE_JOYPAD },
	{ "RetroPad (Modern)",  RETRO_DEVICE_M2_PAD_MODERN },
	{ "Light Gun",          RETRO_DEVICE_LIGHTGUN } };

const struct retro_controller_info CONTROLLER_INFO[] = {
	{ PORT_DEVICES, unsigned(std::size(PORT_DEVICES)) },
	{ PORT_DEVICES, unsigned(std::size(PORT_DEVICES)) },
	{ PORT_DEVICES, unsigned(std::size(PORT_DEVICES)) },
	{ PORT_DEVICES, unsigned(std::size(PORT_DEVICES)) },
	{ nullptr, 0 } };

static_assert(std::size(CONTROLLER_INFO) == libretro_m2_input::MAX_PADS + 1,
		"one entry per port, plus the terminator the frontend scans for");

// Which libretro device each port is set to. Read every frame by retro_run and written by
// retro_set_controller_port_device, both on the libretro thread, so there is nothing to synchronise.
//
// It lives here rather than in the input module because the frontend owns the ordering: the call is
// allowed to arrive before content is loaded, when there is no module to tell, and it must survive
// retro_unload_game so that a second load keeps the player's choice.
unsigned s_port_device[libretro_m2_input::MAX_PADS] = {
	RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD };

static_assert(std::size(s_port_device) == libretro_m2_input::MAX_PADS,
		"the initialiser above has one entry per port and has to keep up with MAX_PADS");

static_assert(m2vk::RETICLE_MAX == libretro_m2_input::MAX_GUNS,
		"one reticle per gun; m2vk_reticle.h keeps its own copy so that it needs no MAME headers");


//============================================================
//  the lightgun reticle
//============================================================

// Where each gun is pointing, published once a frame for whichever renderer is presenting. This is
// the only place that reads the frontend's pointer for a reason other than driving MAME: the input
// module's gun device turns the same two axes into ioport values and deliberately keeps no state a
// renderer could reach, since it lives behind the OSD and the Vulkan side is on the other side of it.
//
// Reading the pointer twice rather than plumbing one read through is the cheap side of the trade —
// two state_cb calls per gun port per frame — and it keeps the drawn position honest: it is the
// frontend's coordinate, not something derived from what MAME made of it. The port value has already
// been through PORT_MINMAX by then, so working back from it would put a calibration of ours between
// the pointer and the cross, which is exactly what devnotes/lightgun.md §5 forbids.
//
// Offscreen counts as no reticle. RELOAD is offscreen too: the gun device pins both axes to
// ABSOLUTE_MIN for it, so the shot really is going into the corner, and drawing a cross where the
// player is pointing would say otherwise.
void publish_reticles()
{
	if (s_input_state_cb == nullptr)
		return;

	for (unsigned port = 0; port < m2vk::RETICLE_MAX; port++)
	{
		if ((s_port_device[port] & RETRO_DEVICE_MASK) != RETRO_DEVICE_LIGHTGUN)
		{
			m2vk::reticle_publish(port, false, 0.0f, 0.0f);
			continue;
		}

		const bool off = (s_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) != 0)
				|| (s_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) != 0);
		if (off)
		{
			m2vk::reticle_publish(port, false, 0.0f, 0.0f);
			continue;
		}

		// SCREEN_X/Y are -0x8000..0x7fff across the viewport; the reticle wants 0..1 across the
		// picture, and the picture's size is not this function's business.
		const int16_t x = s_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
		const int16_t y = s_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
		m2vk::reticle_publish(port, true,
				(float(x) + 32768.0f) / 65536.0f,
				(float(y) + 32768.0f) / 65536.0f);
	}
}

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

// This was a no-op for a reason, and the reason no longer holds. It used to be that every port was
// a RetroPad and nothing else: the analogue sticks and triggers are always read, so
// RETRO_DEVICE_ANALOG and RETRO_DEVICE_JOYPAD were the same device here and there was nothing to
// switch between.
//
// There is now, because a port can be set to RETRO_DEVICE_LIGHTGUN. That is a real choice rather
// than a superset — MAME ORs the pad's stick and the gun into IPT_LIGHTGUN_X, and OR'd absolute
// axes are summed and then saturated, so the two have to take turns. All this records is what the
// port is set to; the gate itself is in libretro_m2_input.cpp. See devnotes/lightgun.md §1.2, §2.2.
//
// Nothing here needs a reload, and that is by design: -lightgun is passed unconditionally and both
// kinds of device are created for every set, so a change mid-run only alters which of them moves.
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
	if (port >= std::size(s_port_device))
		return;

	if (s_port_device[port] != device)
		s_log_cb(RETRO_LOG_INFO, "[model2] port %u set to device 0x%x\n", port, device);
	s_port_device[port] = device;
}

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
	s_environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<struct retro_controller_info *>(CONTROLLER_INFO));

	const std::string path(game->path);
	const std::string system = system_name_from_path(path);

	// Options are read once, here. Both of them are settled before the machine starts — the
	// renderer picks a draw path, the diagnostic combo is baked into the input devices' default
	// assignments — so a change made later is reported by retro_run() and applied at the next load.
	//
	// The diagnostic option is logged as the value it *resolved to*, not as the frontend's string:
	// an unrecognised one silently becomes None, and a log line agreeing with the options menu while
	// the combo does nothing is the shape of bug that costs an afternoon.
	const std::string renderer = m2opt::get(s_environ_cb, m2opt::KEY_RENDERER);
	const unsigned diagnostic = m2opt::get_diagnostic(s_environ_cb);

	s_log_cb(RETRO_LOG_INFO, "[model2] options: %s=%s %s=%s\n",
			m2opt::KEY_RENDERER, renderer.c_str(),
			m2opt::KEY_DIAGNOSTIC_INPUT, m2opt::DIAGNOSTIC_VALUES[diagnostic]);

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

	// The 2D tilemap layers that sandwich the 3D are captured only for the Vulkan path, which
	// composites them itself (m2vk_frame.h). On the software path the two hooks in screen_update()
	// cost a predicate each and nothing more, which is what keeps renderer=software the reference.
	m2vk::frame_enable(s_hw_render);

	// The hardware renderer owns the 3D layer from P3 step 3 on, so MAME's scanline rasteriser stops
	// drawing it — which is also where nearly all of the emulator's CPU time was going. Two overrides,
	// both of which act on the software renderer as well so that the two stay comparable:
	//
	//   M2VK_SW_3D=1  puts MAME's rasteriser back in charge and takes the GPU out of it. That is the
	//                 step-1/2 arrangement, still bit-exact against renderer=software, and it is the
	//                 isolation tool for "is this a rendering problem or a timing one" — skipping the
	//                 rasteriser changes the emulation thread's load substantially.
	//   M2VK_NO_3D=1  neither draws. The picture is the two tilemap layers with a hole between them,
	//                 identical under both renderers because neither touches those pixels, which is
	//                 what the coverage comparison differences against.
	const bool no_3d = std::getenv("M2VK_NO_3D") != nullptr;
	m2vk::set_rasterize(!no_3d && (!s_hw_render || (std::getenv("M2VK_SW_3D") != nullptr)));

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

		// The lightgun class is enabled for every set, not just the six with a gun, and that is what
		// makes retro_set_controller_port_device free to honour at any moment: MAME's options are
		// fixed when the machine is built, so a conditional -lightgun would mean a device change
		// needed a reload. It costs nothing to leave on — a device reporting 0 contributes 0 to the
		// port it is OR'd into.
		//
		// -lightgun_device is deliberately absent rather than forgotten: init_autoselect_devices
		// returns early once the class is enabled, so it would have nothing left to do.
		// devnotes/lightgun.md §1.3, §2.1.
		"-lightgun",
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
	s_osd->set_diagnostic_input(diagnostic);
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
	m2vk::frame_end_run();

	// The port selection survives an unload by design (it is the player's choice, not the machine's),
	// so the aim must not: a second game would otherwise open with the first one's crosshair on it
	// until the frontend next moved the pointer.
	m2vk::reticle_end_run();

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
		input->poll_frontend(s_input_state_cb, s_port_device);

	// Same snapshot, same reason, and it has to be on this side of release_frame(): the software
	// path's blit happens on the emulation thread, in capture_frame(), so the reticle it draws is the
	// aim the frame was emulated from rather than the one after it.
	publish_reticles();

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
