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
#include "m2vk_steerbar.h"
#include "m2vk_sink.h"
#include "s22_seam.h"
#include "s21_seam.h"

#include "renderer_vk/vk_context.h"
#include "renderer_vk/vk_funcs.h"
#include "renderer_vk/vk_geom.h"
#include "renderer_vk/s22_geom.h"
#include "renderer_vk/vk_present.h"

#include "emu.h"
#include "emuopts.h"
#include "drivenum.h"
#include "main.h"

// after emu.h, which it reads MAME's ioport / running_machine types out of and which only a .cpp may
// include
#include "m2vk_analog.h"
#include "m2vk_steer.h"

#include "corestr.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
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

// Set as the emulation thread falls out of emu_thread_main, i.e. "join() will not block". Only the
// process-exit path needs it: everywhere else the libretro thread has already stopped releasing
// frames and can afford to wait.
std::atomic<bool>                          s_emu_finished{ false };

// How long that path waits for the machine to unwind before giving up on it. Generous — MAME's
// teardown is a few file writes — and bounded because a hang at exit is worse than the crash it
// replaces: the window is already gone, so there is nothing for the player to see or dismiss.
constexpr int SHUTDOWN_WAIT_MS = 2000;

// Set at load if the renderer option asked for Vulkan and the frontend accepted the declaration.
// It decides which of the two presentation paths retro_run takes, and it is deliberately not the
// same question as "is there a context right now" — a declared context can be absent for the first
// frames and can be destroyed mid-run.
bool                                       s_hw_render = false;

// Which of the three families the loaded set belongs to. Set from the driver source file in
// retro_load_game (see family_of()), cached here so retro_run's live-options handler — which has no
// `system` in scope — can gate the System 22 block without re-deriving it. model2 until a game loads.
enum class family { model2, system22, system21 };
family                                     s_family = family::model2;

// Per-game option visibility (RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY) is applied once, the first frame
// after the steering detector resolves — which needs a running machine, so it cannot be decided at
// declare() time. Reset on unload so the next game re-decides. See the block in retro_run().
bool                                       s_steer_display_applied = false;

// Same, for the two analog-stick options: shown only on the sets that declare an IPT_AD_STICK, hidden
// on the wheel/gun/fighter sets. Gated on the analog detector, reset on unload. See retro_run().
bool                                       s_analog_display_applied = false;

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

	s_emu_finished.store(true, std::memory_order_release);
}


//============================================================
//  process exit with the machine still running
//============================================================

// The frontend is not obliged to unload the content before the process ends, and on macOS the
// ordinary way to quit does not: closing RetroArch's window takes AppKit's
// -[NSApplication terminate:] path, which calls exit() directly, so neither retro_unload_game nor
// retro_deinit is ever reached. exit() then runs this image's destructors — and destroying a
// joinable std::thread calls std::terminate(), so the core aborted every time the player closed
// the window. The crash report blames __cxa_finalize_ranges with the emulation thread still parked
// on the baton in libretro_m2_osd_interface::update().
//
// The abort is the loud half. The quiet half is that the machine never exits, so MAME writes
// neither NVRAM nor cfg: a quit taken this way silently discarded the game's settings and its high
// scores. Bringing the thread down properly fixes both, which is why this joins rather than simply
// detaching to dodge the terminate().
//
// Registered from retro_init, i.e. after this image's static constructors have run. Destructors and
// atexit handlers share one list and run last-registered-first, so registering later puts this
// ahead of them: everything the teardown touches — s_osd, s_options, MAME's own statics — is still
// alive when it runs. Registering it any earlier would invert that and is the thing not to "tidy".
void shutdown_at_exit()
{
	if (!s_running || !s_emu_thread.joinable() || !s_osd)
		return;

	s_osd->request_exit();

	for (int i = 0; (i < SHUTDOWN_WAIT_MS) && !s_emu_finished.load(std::memory_order_acquire); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	// Either way the thread must not still be joinable when its destructor runs, which is the whole
	// point of being here. Detaching is the giving-up branch: the process is going down regardless,
	// and a thread parked in the emulator is less harmful than a guaranteed abort.
	if (s_emu_finished.load(std::memory_order_acquire))
		s_emu_thread.join();
	else
		s_emu_thread.detach();

	s_running = false;
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
//  controller types
//============================================================

// What a port can be set to. Every port offers the same list, because from the core's side every
// port is the same hardware and whether a gun means anything is a property of the loaded set rather
// than of the port — a gun on vf2 simply binds to types vf2 does not declare.
//
// 🛑 THERE IS ONE PAD ENTRY AND THERE USED TO BE THREE. "RetroPad (Classic)", "RetroPad (Modern)" and
// "RetroPad (Cabinet)" are all gone, and so is the second retro_controller_info array that existed only
// to offer Cabinet to the sets that had a row. The reasoning is in libretro_m2_input.h; the part that
// belongs here is what it bought:
//
//   * the list is not per-game any more, so SET_CONTROLLER_INFO no longer needs the loaded set's name
//     to decide what to send;
//   * "RetroPad" means "this cabinet's controls", on every set, with no menu step;
//   * and a player who wants something else uses the frontend's own remap UI, which is now worth
//     using — the descriptors name what each control actually does on the loaded game.
//
// ⚠️ A config remembering a retired subclass id still plays: the pad treats any unrecognised device
// value as "use this machine's row". Nothing has to be migrated.
const struct retro_controller_description PORT_DEVICES[] = {
	{ "RetroPad",  RETRO_DEVICE_JOYPAD },
	{ "Light Gun", RETRO_DEVICE_LIGHTGUN } };

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


//============================================================
//  driver family
//============================================================

// Which of the three families a loaded set belongs to. Keyed on the running driver's source file, not
// on which flagship happens to be compiled into the table: driver_list::find("ridgerac")>=0 answers
// "is System 22 in my table", which is true for every game the moment all three families share one
// core. driver().type.source() is the __FILE__ of the driver's cpp — sega/model2.cpp,
// namco/namcos22.cpp, namco/namcos21.cpp or namco/namcos21_c67.cpp — so it names the family the set
// actually belongs to regardless of what else the table holds. The set names do not collide across the
// four cpps (checked), so a name lookup is unambiguous. (enum family is declared with the state block above.)

family family_of(const std::string &system)
{
	int const driver = driver_list::find(system.c_str());
	if (driver < 0)
		return family::model2;   // not one of ours; the machine fails to start regardless

	// The raw __FILE__ carries whatever build-relative prefix the compiler was handed, so match on the
	// distinguishing token rather than the whole path (this is how MAME's own info_xml_creator finds it).
	// namcos21_c67.cpp contains "namcos21"; the two Namco tokens do not overlap.
	const std::string src = driver_list::driver(driver).type.source();
	if (src.find("namcos22") != std::string::npos)
		return family::system22;
	if (src.find("namcos21") != std::string::npos)
		return family::system21;
	return family::model2;
}

// Gate the option set to one family: point Internal Resolution at that family's native size and hide the
// options the other families own. Called at LOAD time (from retro_load_game, once family_of(system) is
// known), because retro_set_environment declares the full union before any set is chosen — in the merged
// core all three flagships are present there, so it cannot pick a family. The caller does
//   clear_hidden(); apply_family_cascade(fam); redeclare(cb);
// so each load starts from the full set and the frontend's menu is re-published for the loaded family.
// This is the same hide_option()/set_native_resolution() cascade that used to live in
// retro_set_environment, keyed on the family rather than on which flagship is compiled in.
void apply_family_cascade(family fam)
{
	if (fam == family::system22)
	{
		m2opt::set_native_resolution("640x480");
		// System 22's menu carries No Lighting (model2_flat_luma, wired into the S22 shade tail) but not
		// Model 2's Flat Shading — the S22 untextured look is its own option (system22_no_textures, a
		// greyscale view) rather than the base-colour draw Flat Shading gives, so Flat Shading would be a
		// dead entry here.
		m2opt::hide_option(m2opt::KEY_FLAT_SHADING);
		// Transparency (stipple vs blended) is a Model 2-only fix: it drives s_option_blend in vk_geom.cpp,
		// the Model 2 polygon pass. System 22's pipeline hardcodes blendEnable=VK_TRUE (s22_geom.cpp) — it
		// does real hardware transparency unconditionally — so the option would be a dead menu entry.
		m2opt::hide_option(m2opt::KEY_TRANSPARENCY);
	}
	else if (fam == family::system21)
	{
		// System 21 native is 496x480 (the polygonizer's framebuffer) — a listed value, so this retargets
		// the default and the "(Native)" label onto it. Its menu wants none of the System 22-only options
		// either — S21 is always z-buffered, has no texture filter, no fog/untextured toggles.
		m2opt::set_native_resolution("496x480");
		m2opt::hide_option(m2opt::KEY_S22_TEXTURE_FILTER);
		m2opt::hide_option(m2opt::KEY_S22_FOG);
		m2opt::hide_option(m2opt::KEY_S22_NO_TEXTURES);
		m2opt::hide_option(m2opt::KEY_S22_2D_OVERLAY);
		// And three Model 2 render options the S21 path never reads: it is always untextured (so Flat
		// Shading has nothing to remove), has no per-poly luma hook (No Lighting), and hardcodes
		// blendEnable=VK_FALSE (Transparency). Left visible they would be dead menu entries, so hide them
		// for the same reason the S22-only options are hidden from Model 2 below.
		m2opt::hide_option(m2opt::KEY_FLAT_SHADING);
		m2opt::hide_option(m2opt::KEY_FLAT_LUMA);
		m2opt::hide_option(m2opt::KEY_TRANSPARENCY);
	}
	else
	{
		// Model 2: keep its authored 496x384 native (set_native_resolution not needed — clear of the other
		// families' retargeting is done by the caller's clear path plus set_native_resolution resetting the
		// default itself, but be explicit so a load after an S22/S21 set restores it).
		m2opt::set_native_resolution("496x384");
		// System 22-only options do not belong on the Model 2 menu (its renderer never reads them).
		m2opt::hide_option(m2opt::KEY_S22_TEXTURE_FILTER);
		m2opt::hide_option(m2opt::KEY_S22_FOG);
		m2opt::hide_option(m2opt::KEY_S22_NO_TEXTURES);
		m2opt::hide_option(m2opt::KEY_S22_2D_OVERLAY);
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

	// Options are NOT declared here. In the merged core all three families' drivers are in the table, so
	// retro_set_environment — which runs before any set is chosen — cannot know which family's menu to
	// publish or what the Internal Resolution native size is (496x384 Model 2, 640x480 System 22, 496x480
	// System 21). Declaring a family-neutral guess here would be the value a frontend caches: a frontend
	// keeps the FIRST declaration's values, so a later redeclare() would not move the resolution the
	// renderer reads (measured — this exact bug rendered System 22 at Model 2's 496x384). So the one
	// declaration is deferred to retro_load_game, where family_of(system) is known: clear_hidden() +
	// apply_family_cascade(fam) sets the native size and the family's option subset, then declare() (via
	// redeclare()) publishes it. A frontend that instead reads an option before declaration falls back to
	// the core's default_value(), which apply_family_cascade() has patched to the same family native, so
	// both paths agree. RetroArch populates its Core Options menu from the load-time declaration.
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
	// The core's name, not the system's — RetroArch shows it as "Core name" and builds the per-core
	// config directory from it (config/m2-vk/m2-vk.opt). Deliberately carries no MAME wordmark; see
	// devnotes/legalstuff.md. Renaming this orphans a player's existing options file, so it is not a
	// string to adjust casually.
	info->library_name = "m2-vk";
	info->library_version = emulator_info::get_bare_build_version();
	info->valid_extensions = "zip|7z";
	info->need_fullpath = true;
	info->block_extract = true;
}

// The geometry as the frontend was last told it. base is MAME's own picture and never moves; max is a
// high-water mark that only ever grows, because shrinking it would invalidate a frontend allocation
// that a frame still in flight may be using.
//
// This is Flycast's arrangement (shell/libretro/libretro.cpp, setGameGeometry/retro_resize_renderer)
// and it is copied deliberately: SET_GEOMETRY for a size inside the max, which is free, and
// SET_SYSTEM_AV_INFO only when the max has to grow, which costs a video-driver reinit.
namespace {

unsigned s_fb_width = 0;
unsigned s_fb_height = 0;
unsigned s_max_width = 0;
unsigned s_max_height = 0;

void picture_size(unsigned &width, unsigned &height)
{
	int w = 496, h = 384;
	if (s_osd)
		s_osd->framebuffer(w, h);
	if ((w <= 0) || (h <= 0))
	{
		w = 496;
		h = 384;
	}
	width = unsigned(w);
	height = unsigned(h);
}

void fill_geometry(struct retro_game_geometry &geometry)
{
	unsigned width = 0, height = 0;
	picture_size(width, height);

	// base is the PICTURE, whatever the internal resolution is — it is what a frontend sizes its
	// window from at startup, and opening 2848 pixels wide because a menu setting says so is Flycast's
	// "avoid gigantic window size at startup" problem exactly. The frame's real size travels with each
	// video_refresh instead.
	geometry.base_width = width;
	geometry.base_height = height;

	if (s_max_width < width)
		s_max_width = width;
	if (s_max_height < height)
		s_max_height = height;
	geometry.max_width = s_max_width;
	geometry.max_height = s_max_height;

	// Unchanged by the internal resolution, and that is the whole reason a 4:3 internal resolution is
	// allowed to serve a 1.2917 picture: this says what SHAPE to draw the image, so a bigger buffer is
	// a denser sample grid rather than a wider picture.
	geometry.aspect_ratio = float(s_osd ? s_osd->aspect_ratio() : (4.0 / 3.0));
}

// Tell the frontend the frame has changed size, by whichever of the two calls fits. Called from
// retro_run() with the extent the renderer actually presented, and only when it has moved.
void announce_geometry(unsigned width, unsigned height)
{
	if ((width == s_fb_width) && (height == s_fb_height))
		return;

	s_fb_width = width;
	s_fb_height = height;

	// The max has to cover the frame before the frame is announced, or the frontend is entitled to
	// refuse it. Growing it is the expensive branch: SET_SYSTEM_AV_INFO may reinitialise the video
	// driver, which for us means a context_destroy/context_reset pair and a ring rebuild — survivable,
	// exercised, and still worth avoiding on every step of a resolution slider.
	const bool grow = (width > s_max_width) || (height > s_max_height);
	if (grow)
	{
		if (s_max_width < width)
			s_max_width = width;
		if (s_max_height < height)
			s_max_height = height;

		struct retro_system_av_info av;
		std::memset(&av, 0, sizeof(av));
		fill_geometry(av.geometry);
		av.timing.fps = s_osd ? s_osd->refresh_rate() : 60.0;
		av.timing.sample_rate = double(s_osd && s_osd->audio_rate() ? s_osd->audio_rate() : 48000);
		s_environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
	}
	else
	{
		struct retro_game_geometry geometry;
		std::memset(&geometry, 0, sizeof(geometry));
		fill_geometry(geometry);
		s_environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
	}

	s_log_cb(RETRO_LOG_INFO, "[model2] presenting %ux%u (max %ux%u) via %s\n",
			width, height, s_max_width, s_max_height,
			grow ? "SET_SYSTEM_AV_INFO" : "SET_GEOMETRY");
}

} // anonymous namespace

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	std::memset(info, 0, sizeof(*info));

	// Seeded from the resolution the option asks for, so that a frontend which read the option before
	// content loaded does not need a SET_SYSTEM_AV_INFO on the very first frame to be told about it.
	unsigned res_width = 0, res_height = 0;
	m2opt::get_internal_size(s_environ_cb, res_width, res_height);
	if (s_max_width < res_width)
		s_max_width = res_width;
	if (s_max_height < res_height)
		s_max_height = res_height;

	fill_geometry(info->geometry);

	info->timing.fps = s_osd ? s_osd->refresh_rate() : 60.0;
	info->timing.sample_rate = double(s_osd && s_osd->audio_rate() ? s_osd->audio_rate() : 48000);
}


//============================================================
//  libretro: lifecycle
//============================================================

RETRO_API void retro_init(void)
{
	// Bring the emulation thread down if the process exits with content still loaded — see
	// shutdown_at_exit() for why that is an ordinary quit rather than an edge case, and for why it
	// has to be registered here rather than at load or at static-init time. Once, because a
	// frontend may init the core more than once and the handler list keeps every registration.
	static const bool registered = (std::atexit(shutdown_at_exit) == 0);
	(void)registered;

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

	const std::string path(game->path);
	const std::string system = system_name_from_path(path);

	// What a port may be set to. No longer per-game — there is one pad type and it is whatever this
	// cabinet's controls are — so this is sent unconditionally and needs nothing about the set.
	s_environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<struct retro_controller_info *>(CONTROLLER_INFO));

	// The set's parent, for the layout lookup below: one row covers a set and all of its clones.
	//
	// driver_list::find() is a name-only lookup in the subtarget's own compiled-in driver table, so a
	// content file whose basename is not a Model 2 set simply yields no parent and no row; the machine
	// will fail to start a few lines further down for the same reason.
	int const driver = driver_list::find(system.c_str());
	char const *const parent = (driver >= 0) ? driver_list::driver(driver).parent : nullptr;

	// Which family this set belongs to, from its driver source file. Every per-family decision below
	// keys off this rather than off driver_list::find(<flagship>), which only asks whether a family is
	// compiled in — true for all three once the cores merge.
	const family fam = family_of(system);
	s_family = fam;   // cached for retro_run's live-options handler, which has no `system`

	// Re-gate the menu for this family now that it is known. retro_set_environment declared the full union
	// (it runs before any set is chosen); start from that full set and hide the options the other families
	// own, then re-point Internal Resolution's native size and re-publish. Runs BEFORE the option reads
	// below so the native-res default the renderer picks up is this family's, not the authored Model 2 one.
	m2opt::clear_hidden();
	apply_family_cascade(fam);
	m2opt::redeclare(s_environ_cb);

	// Options are read once, here. Both of them are settled before the machine starts — the
	// renderer picks a draw path, the diagnostic combo is baked into the input devices' default
	// assignments — so a change made later is reported by retro_run() and applied at the next load.
	//
	// The diagnostic option is logged as the value it *resolved to*, not as the frontend's string:
	// an unrecognised one silently becomes None, and a log line agreeing with the options menu while
	// the combo does nothing is the shape of bug that costs an afternoon.
	const std::string renderer = m2opt::get(s_environ_cb, m2opt::KEY_RENDERER);
	const unsigned diagnostic = m2opt::get_diagnostic(s_environ_cb);
	unsigned res_width = 0, res_height = 0;
	m2opt::get_internal_size(s_environ_cb, res_width, res_height);
	const unsigned flat_shading = m2opt::get_flat_shading(s_environ_cb);
	const bool flat_luma = m2opt::get_flat_luma(s_environ_cb);
	const unsigned transparency = m2opt::get_transparency(s_environ_cb);
	const unsigned steer_response = m2opt::get_steering_response(s_environ_cb);
	const float steer_deadzone = m2opt::get_steering_deadzone(s_environ_cb);
	const float steer_range = m2opt::get_steering_range(s_environ_cb);
	const unsigned steer_damp_drive = m2opt::get_steering_damp_drive(s_environ_cb);
	const unsigned steer_damp_return = m2opt::get_steering_damp_return(s_environ_cb);
	const bool steer_display = m2opt::get_steering_display(s_environ_cb);
	const float analog_deadzone = m2opt::get_analog_deadzone(s_environ_cb);
	const float analog_reach = m2opt::get_analog_reach(s_environ_cb);

	// The resolution is logged as "native" rather than as the 0x0 the parser produces for a value it
	// did not recognise: "model2_internal_res=0x0" reads as a bug in the option, when what it means is
	// that the frontend's value was one we do not declare and the hardware's own resolution is what
	// the run will use.
	char res_text[32];
	if ((res_width == 0) || (res_height == 0))
		std::snprintf(res_text, sizeof(res_text), "native");
	else
		std::snprintf(res_text, sizeof(res_text), "%ux%u", res_width, res_height);

	// Frame counts formatted as "off" or "<n>f", so the options line reads them the way the menu does.
	char damp_drive_text[8], damp_return_text[8];
	if (steer_damp_drive == 0)  std::snprintf(damp_drive_text,  sizeof(damp_drive_text),  "off");
	else                        std::snprintf(damp_drive_text,  sizeof(damp_drive_text),  "%uf", steer_damp_drive);
	if (steer_damp_return == 0) std::snprintf(damp_return_text, sizeof(damp_return_text), "off");
	else                        std::snprintf(damp_return_text, sizeof(damp_return_text), "%uf", steer_damp_return);

	s_log_cb(RETRO_LOG_INFO, "[model2] options: %s=%s %s=%s %s=%s %s=%s %s=%s %s=%s %s=%s %s=%.0f%% %s=%.0f%% %s=%s/%s %s=%s\n",
			m2opt::KEY_RENDERER, renderer.c_str(),
			m2opt::KEY_DIAGNOSTIC_INPUT, m2opt::DIAGNOSTIC_VALUES[diagnostic],
			m2opt::KEY_INTERNAL_RES, res_text,
			m2opt::KEY_FLAT_SHADING, (flat_shading != 0) ? "flat" : "off",
			m2opt::KEY_FLAT_LUMA, flat_luma ? "on" : "off",
			m2opt::KEY_TRANSPARENCY, (transparency != 0) ? "blended" : "stipple",
			m2opt::KEY_STEERING_RESPONSE, m2opt::STEERING_RESPONSE_VALUES[steer_response],
			m2opt::KEY_STEERING_DEADZONE, double(steer_deadzone) * 100.0,
			m2opt::KEY_STEERING_RANGE, double(steer_range) * 100.0,
			"model2_steering_damp", damp_drive_text, damp_return_text,
			m2opt::KEY_STEERING_DISPLAY, steer_display ? "on" : "off");

	s_log_cb(RETRO_LOG_INFO, "[model2] analog: %s=%.0f%% %s=%.0f%%\n",
			m2opt::KEY_ANALOG_DEADZONE, double(analog_deadzone) * 100.0,
			m2opt::KEY_ANALOG_REACH, double(analog_reach) * 100.0);

	// The frontend's remap labels, per game, from the same layout row the pad reads. This is what makes
	// the Controls menu say "GEAR 1" and "VR1 (Red)" instead of "Button 2" and "Button 6", and it is the
	// user-facing half of the per-game layout work: a default nobody has to change, and a remap screen
	// worth opening if they want to anyway.
	//
	// 🚨 It sits BELOW the options read rather than beside SET_CONTROLLER_INFO above, and the ordering is
	// load-bearing: L3 is IPT_SERVICE1 only while model2_diagnostic_input names a combo, and is an inert
	// IPT_UI_MENU otherwise, so its label cannot be decided before `diagnostic` has been read. Sent from
	// where the descriptors used to be sent, it would have labelled a dead control on every default run.
	s_environ_cb(
			RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
			const_cast<struct retro_input_descriptor *>(
					libretro_m2_input::descriptors(system.c_str(), parent, diagnostic != m2opt::DIAG_NONE)));

	if (libretro_m2_input::has_layout(system.c_str(), parent))
		s_log_cb(RETRO_LOG_INFO, "[model2] '%s' has its own control layout; it is what a RetroPad plays as\n", system.c_str());
	else
		s_log_cb(RETRO_LOG_INFO, "[model2] '%s' has no layout row; using the generic one\n", system.c_str());

	// 🚨 The corresponding M2VK_* switch overrides each of the two options below, and the harness
	// depends on that: ab.sh's MODE= and res.sh's scale arrive in the environment, and a .opt file left
	// in a non-default state by an interactive session must not be able to rewrite a baseline. Which
	// means the line above can disagree with what the run actually does — and "read [model2] options:
	// before believing a result" is the standing rule the whole harness rests on, so the disagreement
	// has to announce itself.
	//
	// Presence, not value: M2VK_SS=99 is refused back to 1x by its reader, and a line saying the switch
	// was set is still the right thing to have printed.
	for (char const *const sw : { "M2VK_RES", "M2VK_SS", "M2VK_FORCE_SOLID", "M2VK_FLAT_LUMA", "M2VK_BLEND",
			"M2VK_STEER_LINEAR", "M2VK_STEER_DEADZONE", "M2VK_STEER_GAMMA", "M2VK_STEER_RANGE",
			"M2VK_STEER_DAMP_DRIVE", "M2VK_STEER_DAMP_RETURN", "M2VK_STEERBAR",
			"M2VK_ANALOG_LINEAR", "M2VK_ANALOG_DEADZONE", "M2VK_ANALOG_REACH", "M2VK_S22_FILTER",
			"M2VK_S22_DEPTH", "M2VK_S22_FOG", "M2VK_S22_NOTEX", "M2VK_S22_HUD", "M2VK_POLYCOUNT" })
	{
		if (std::getenv(sw) != nullptr)
			s_log_cb(RETRO_LOG_INFO, "[model2] %s is set; it overrides the matching core option\n", sw);
	}

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

	// Parked now and read later — the internal resolution at context_reset, which sizes every ring
	// slot's attachments, and the flat-shading mode at sink_open(). Neither has a reader yet at this
	// point in the load, which is why they are setters rather than arguments to something.
	//
	// ⚠️ These sit BELOW declare_hw_render deliberately: it is what installs the renderer's log
	// callback (vk_context.cpp calls set_log() as its first act), and vk_log() is a silent no-op until
	// then. Called any earlier, set_option_resolution's out-of-range complaint would be swallowed —
	// which is the one thing it exists to do.
	//
	// The env override lives at each reader rather than here, because the standalone OSD=sdl3 build has
	// no core options at all and must keep the switches working.
	m2vk::set_option_resolution(res_width, res_height);
	m2vk::set_option_force_solid(flat_shading);
	m2vk::set_option_flat_luma(flat_luma);
	m2vk::set_option_blend(transparency);

	// input_init() recomposes these against the M2VK_STEER_* switches once the machine exists.
	m2vk::set_option_steering(steer_deadzone, m2opt::STEERING_RESPONSE_GAMMA[steer_response], steer_range);
	m2vk::set_option_steer_damping(steer_damp_drive, steer_damp_return);
	m2vk::set_option_steerbar(steer_display);

	// analog_config() recomposes this against the M2VK_ANALOG_* switches once the machine exists.
	m2vk::set_option_analog(analog_deadzone, analog_reach);

	// Polygon counter — a HUD read-out, all three families. Vulkan-path only (it counts GPU primitives),
	// a harmless no-op on the software renderer.
	m2vk::set_option_counter(m2opt::get_poly_counter(s_environ_cb));

	// System 22's 3D texture filter — its own option, declared only on the S22 build (hidden on Model 2
	// above). Parked in the S22 polygon pass; a harmless no-op on Model 2, whose seam never draws. The
	// option is hidden there, so it always reads its "off" default, but the read is gated on family
	// anyway so the S22-only log line stays off the Model 2 console.
	if (fam == family::system22)
	{
		const bool s22_filter = m2opt::get_s22_texture_filter(s_environ_cb);
		s22::set_option_filter(s22_filter);
		// The Depth Buffer option was removed before release (shelved; see devnotes/zfighting.md). The
		// renderer code is dormant — s22::depth_enabled() is forced off — so nothing parks it here.
		const bool s22_fog = m2opt::get_s22_fog(s_environ_cb);
		s22::set_option_fog(s22_fog);
		const bool s22_notex = m2opt::get_s22_no_textures(s_environ_cb);
		s22::set_option_no_textures(s22_notex);
		// No Lighting is the shared model2_flat_luma option; on the S22 path it skips the shade tail. The
		// value was read as `flat_luma` above.
		s22::set_option_no_lighting(flat_luma);
		const bool s22_overlay = m2opt::get_s22_2d_overlay(s_environ_cb);
		s22::set_option_hud(s22_overlay);
		s_log_cb(RETRO_LOG_INFO, "[system22] options: %s=%s %s=%s %s=%s %s=%s %s=%s\n",
				m2opt::KEY_S22_TEXTURE_FILTER, s22_filter ? "on" : "off",
				m2opt::KEY_S22_FOG, s22_fog ? "on" : "off",
				m2opt::KEY_S22_NO_TEXTURES, s22_notex ? "on" : "off",
				m2opt::KEY_FLAT_LUMA, flat_luma ? "on" : "off",
				m2opt::KEY_S22_2D_OVERLAY, s22_overlay ? "on" : "off");
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

	// The System 22 GPU pass (S2) owns the 3D under the same condition the Model 2 hardware path does:
	// a Vulkan context is up and neither M2VK_NO_3D nor M2VK_SW_3D has taken it back. set_gpu(true) both
	// attaches the record consumer and stops the driver's render_triangle_fan, so software draws no 3D.
	// M2VK_NO_3D is the "neither draws" background reference: set_no_3d() also hands sw_owns_3d() back
	// false, so — unlike set_gpu(false) — the software rasteriser is suppressed too and the picture is
	// just the 2D layers (what the coverage/exact harness differences against). In the Model 2 build
	// these are harmless flag writes — no S22 seam site ever calls into them.
	if (no_3d)
		s22::set_no_3d();
	else
		s22::set_gpu(s_hw_render && (std::getenv("M2VK_SW_3D") == nullptr));

	// The System 21 GPU pass (T2) owns the 3D under the same condition, gated on its own family so a
	// namcos22 / Model 2 build never turns S21 capture on (harmless if it did — no S21 seam fires — but
	// kept safe-by-default). Only the namcos21 build compiles in starblad.
	if (fam == family::system21)
	{
		if (no_3d)
			s21::set_no_3d();
		else
			s21::set_gpu(s_hw_render && (std::getenv("M2VK_SW_3D") == nullptr));
	}

	// The content's own directory, plus a place for sets the frontend keeps alongside the core.
	// The second entry is what makes a clone loadable when its parent set lives elsewhere. Which
	// leaf that is depends on the driver family compiled into this dylib (see the driver_list::find
	// note in retro_set_environment above) — a System 22 build must not go looking in ".../model2".
	const std::string family_dir = (fam == family::system22) ? "system22"
			: (fam == family::system21) ? "system21" : "model2";
	std::string rompath = rompath_from_path(path);
	const std::string systemdir = frontend_directory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
	if (!systemdir.empty())
		rompath += ";" + systemdir + "/" + family_dir;

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
		const std::string base = savedir + "/" + family_dir;
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
	s_emu_finished.store(false, std::memory_order_release);
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

	// 🚨 The savestate size must be answerable before this returns, because the frontend asks
	// straight afterwards and a frontend that is told 0 disables savestates for the session. It is
	// not automatic: MAME closes the save registry in allow_registration(false) (machine.cpp:306),
	// which is AFTER start_all_devices(), while update() — and therefore the baton — is reached
	// twice before that point. Measured on vf2: the registry passes through 15 and 26 entries before
	// settling at 4294. So spin frames until the registry is final rather than assuming a picture
	// implies it.
	for (int i = 0; (i < MAX_STARTUP_FRAMES) && (s_osd->state_size() == 0); i++)
	{
		s_osd->release_frame();
		if (!s_osd->wait_for_frame())
			break;
	}

	const size_t state_bytes = s_osd->state_size();
	if (state_bytes == 0)
	{
		s_log_cb(RETRO_LOG_WARN, "[model2] the save registry never closed; savestates are unavailable this session\n");
	}
	else
	{
		// libretro.h:2702 says retro_init or retro_load_game, not both; this is the one that knows.
		//
		// PLATFORM_DEPENDENT only, and the three we deliberately do NOT declare are the interesting
		// part:
		//   ENDIAN_DEPENDENT   — MAME records the writer's endianness in the state header and flips
		//                        every entry on read when it disagrees (save.cpp:472, flip_data),
		//                        so a cross-endian load is genuinely supported. Declaring it would
		//                        be a lie that costs netplay.
		//   MUST_INITIALIZE    — would be true if the size were not ready when this returns. It is:
		//                        the loop above spins until the save registry closes.
		//   INCOMPLETE         — means "do not rely on this for netplay or rerecording". Not
		//                        claimed, because devnotes/state.sh measures the opposite: a state
		//                        taken from one machine history and loaded into another reproduces
		//                        the first history's future byte-exactly.
		uint64_t quirks = RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT;
		s_environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
	}

	s_log_cb(RETRO_LOG_INFO, "[model2] started '%s'; savestate %zu bytes\n", system.c_str(), state_bytes);
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
	// Next game re-runs the steering-visibility decision (its wheel status may differ).
	s_steer_display_applied = false;
	s_analog_display_applied = false;
	s_family = family::model2;   // next game re-derives from its driver source

	// The frontend normally fires context_destroy before this; make the state safe whether or not it
	// did, and stop a context_reset arriving for a machine that no longer exists.
	m2vk::forget_hw_render();
	m2vk::frame_end_run();

	// The port selection survives an unload by design (it is the player's choice, not the machine's),
	// so the aim must not: a second game would otherwise open with the first one's crosshair on it
	// until the frontend next moved the pointer.
	m2vk::reticle_end_run();
	m2vk::steerbar_end_run();

	// What was announced, not the high-water mark: the next content load gets a fresh
	// retro_get_system_av_info and must announce its first frame's size rather than assume the last
	// game's still holds. s_max_* deliberately survives — it only ever grows, and a frontend allocation
	// sized by it may outlive the unload.
	s_fb_width = 0;
	s_fb_height = 0;

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

	// Per-game menu hygiene: the six steering options only mean anything on a machine with a wheel (one
	// that declares IPT_PADDLE), so hide them everywhere else — the gun games (Time Crisis, Virtua Cop),
	// the fighters (VF2), and everything on System 21. The steering EFFECT is already gated on the same
	// detector; this hides the dead menu entries to match. It runs once, the first frame after the
	// detector resolves (m2vk::steer_frame() sets .resolved on the first safe emulated frame), and shows
	// or hides by .active so a reload from a wheel game to a gun game — and back — settles correctly.
	// Visibility only: a hidden option still reads its declared default, so no harness pin is disturbed.
	if (!s_steer_display_applied && m2vk::steer().resolved)
	{
		const bool wheel = m2vk::steer().active;
		for (char const *const key : { m2opt::KEY_STEERING_RESPONSE, m2opt::KEY_STEERING_DEADZONE,
				m2opt::KEY_STEERING_RANGE, m2opt::KEY_STEERING_DAMP_DRIVE, m2opt::KEY_STEERING_DAMP_RETURN,
				m2opt::KEY_STEERING_DISPLAY })
			m2opt::set_option_display(s_environ_cb, key, wheel);
		s_steer_display_applied = true;
		if (s_log_cb != nullptr)
			s_log_cb(RETRO_LOG_INFO, "[model2] steering options %s (machine %s)\n",
					wheel ? "shown" : "hidden", wheel ? "has a wheel" : "has no wheel");
	}

	// The same, for the two analog-stick options: shown only on the sets that declare an IPT_AD_STICK
	// (Star Blade, the twin-stick and flight sets), hidden on the wheel/gun/fighter sets. Gated on the
	// analog detector, not the driver family, so Star Blade (System 21) shows them. Runs once, the first
	// frame after m2vk::analog_frame() resolves. Visibility only — a hidden option still reads its
	// declared default, so no harness pin is disturbed.
	if (!s_analog_display_applied && m2vk::analog().resolved)
	{
		const bool stick = m2vk::analog().active;
		for (char const *const key : { m2opt::KEY_ANALOG_DEADZONE, m2opt::KEY_ANALOG_REACH })
			m2opt::set_option_display(s_environ_cb, key, stick);
		s_analog_display_applied = true;
		if (s_log_cb != nullptr)
			s_log_cb(RETRO_LOG_INFO, "[model2] analog-stick options %s (machine %s)\n",
					stick ? "shown" : "hidden", stick ? "has an analog stick" : "has no analog stick");
	}

	// Four of the six options apply live; two cannot. The frontend clears the flag as this reads it,
	// so this runs once per change rather than once per frame.
	//
	// 🚨 "Applied when content is loaded" is the wrong answer for anything a player is meant to *play*
	// with — they change it in the menu, nothing happens, and the reasonable conclusion is that the
	// option is broken. model2_internal_res and model2_flat_shading are both reachable from a running
	// machine, so they are applied here:
	//
	//   flat shading    — g_force_solid is a plain global that submit() reads per polygon, and this is
	//                     the same point at which input is published, i.e. with the emulation thread
	//                     parked on the baton. Takes effect on the next frame.
	//   no lighting     — g_flat_luma, the same global in the same place, resolved per polygon as it
	//                     crosses. Takes effect on the next frame.
	//   internal res    — parked in the renderer; present_frame() compares it against the ring it
	//                     built and rebuilds when they differ, exactly as it does for a sync-mask
	//                     change. Takes effect on the next presented frame.
	//   transparency    — parked in the polygon pass, which latches it at the top of each upload so
	//                     that one frame's deferral decision and its push constant agree. Takes effect
	//                     on the next uploaded frame. Nothing to rebuild: all three pipelines were
	//                     created at context_reset.
	//   steering        — parked in m2vk::steer(), read by the pad below. Takes effect next frame.
	//   steering display — bool in m2vk_steerbar.cpp, read by the emulation thread next frame.
	//
	// model2_renderer and model2_diagnostic_input genuinely cannot: one decides whether hardware
	// render was declared at all, before the machine started, and the other is baked into the input
	// assignments when the devices were configured. Those two still want a reload, and say so.
	if (m2opt::updated(s_environ_cb))
	{
		unsigned res_width = 0, res_height = 0;
		m2opt::get_internal_size(s_environ_cb, res_width, res_height);
		const unsigned flat_shading = m2opt::get_flat_shading(s_environ_cb);
		const bool flat_luma = m2opt::get_flat_luma(s_environ_cb);
		const unsigned transparency = m2opt::get_transparency(s_environ_cb);
		const unsigned steer_response = m2opt::get_steering_response(s_environ_cb);
		const float steer_deadzone = m2opt::get_steering_deadzone(s_environ_cb);
		const float steer_range = m2opt::get_steering_range(s_environ_cb);
		const unsigned steer_damp_drive = m2opt::get_steering_damp_drive(s_environ_cb);
		const unsigned steer_damp_return = m2opt::get_steering_damp_return(s_environ_cb);
		const bool steer_display = m2opt::get_steering_display(s_environ_cb);

		m2vk::set_option_resolution(res_width, res_height);
		m2vk::set_option_force_solid(flat_shading);
		m2vk::set_option_flat_luma(flat_luma);
		m2vk::set_option_blend(transparency);

		// Recomposes against the switches, so a run pinned by M2VK_STEER_GAMMA stays pinned when the
		// player touches an unrelated option.
		m2vk::set_option_steering(steer_deadzone, m2opt::STEERING_RESPONSE_GAMMA[steer_response], steer_range);
		m2vk::set_option_steer_damping(steer_damp_drive, steer_damp_return);
		m2vk::set_option_steerbar(steer_display);
		m2vk::set_option_analog(m2opt::get_analog_deadzone(s_environ_cb), m2opt::get_analog_reach(s_environ_cb));
		m2vk::set_option_counter(m2opt::get_poly_counter(s_environ_cb));

		// System 22 texture filter, fog, no-textures and No Lighting all apply live — push-constant bits
		// read at the next draw, nothing to rebuild.
		if (s_family == family::system22)
		{
			s22::set_option_filter(m2opt::get_s22_texture_filter(s_environ_cb));
			s22::set_option_fog(m2opt::get_s22_fog(s_environ_cb));
			s22::set_option_no_textures(m2opt::get_s22_no_textures(s_environ_cb));
			s22::set_option_no_lighting(flat_luma);
			s22::set_option_hud(m2opt::get_s22_2d_overlay(s_environ_cb));
		}

		char res_text[32];
		if ((res_width == 0) || (res_height == 0))
			std::snprintf(res_text, sizeof(res_text), "native");
		else
			std::snprintf(res_text, sizeof(res_text), "%ux%u", res_width, res_height);

		s_log_cb(RETRO_LOG_INFO,
				"[model2] core options changed: %s=%s %s=%s %s=%s %s=%s %s=%s %s=%.0f%% %s=%.0f%% applied now; %s and %s need a reload\n",
				m2opt::KEY_INTERNAL_RES, res_text,
				m2opt::KEY_FLAT_SHADING, (flat_shading != 0) ? "flat" : "off",
				m2opt::KEY_FLAT_LUMA, flat_luma ? "on" : "off",
				m2opt::KEY_TRANSPARENCY, (transparency != 0) ? "blended" : "stipple",
				m2opt::KEY_STEERING_RESPONSE, m2opt::STEERING_RESPONSE_VALUES[steer_response],
				m2opt::KEY_STEERING_DEADZONE, double(steer_deadzone) * 100.0,
				m2opt::KEY_STEERING_RANGE, double(steer_range) * 100.0,
				m2opt::KEY_RENDERER, m2opt::KEY_DIAGNOSTIC_INPUT);
	}

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
		//
		// 🚨 The size passed on is the RENDERER's, not MAME's: with the internal resolution above native
		// the image the frontend was just given is bigger than the picture, and handing over the
		// picture's numbers here would have the frontend read a corner of it. present_extent() is the
		// only authority on that — it is the extent the ring was actually built at, clamps included.
		unsigned out_width = 0, out_height = 0;
		if (have_picture && m2vk::present_frame(pixels, unsigned(width), unsigned(height))
				&& m2vk::present_extent(out_width, out_height))
		{
			announce_geometry(out_width, out_height);
			s_video_cb(RETRO_HW_FRAME_BUFFER_VALID, out_width, out_height, 0);
		}
		else
		{
			s_video_cb(nullptr, 0, 0, 0); // frame duped
		}
	}
	else
	{
		// Always the picture's own size: MAME's rasteriser draws at one resolution and the internal
		// resolution option says so.
		if (have_picture)
		{
			announce_geometry(unsigned(width), unsigned(height));
			s_video_cb(pixels, unsigned(width), unsigned(height), size_t(width) * sizeof(uint32_t));
		}
		else
		{
			s_video_cb(nullptr, 0, 0, 0); // frame duped
		}
	}

	int samples = 0;
	const int16_t *const audio = s_osd->frame_audio(samples);
	if ((audio != nullptr) && (samples > 0) && (s_audio_batch_cb != nullptr))
		s_audio_batch_cb(audio, size_t(samples));
}


//============================================================
//  libretro: savestates
//
//  P1 deferred these and the reason is worth reading before touching them
//  (devnotes/p1-libretro-core.md): no Model 2 set carries MACHINE_SUPPORTS_SAVE. That is still true,
//  and it turned out not to gate what it was thought to gate — the flag drives a UI warning, the
//  -autosave load and a fatalerror on devices that register nothing, while save_manager::do_write and
//  do_read have no supported() check at all. devnotes/savestates.md is the audit that replaced the
//  inference, and m2vk_savestate.h carries the three properties this rests on.
//
//  🚨 All three run on the FRONTEND thread with the emulation thread parked on the baton in
//  osd->update(). That is what makes them safe, and it is also what makes the state small: the
//  several-megabyte raster->poly_list, and poly_sorted_list's array of raw pointers, are rebuilt from
//  m_bufferram at every vblank and are therefore not live at this one point in the frame.
//============================================================

RETRO_API size_t retro_serialize_size(void)
{
	if (!s_running || !s_osd)
		return 0;
	return s_osd->state_size();
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
	if (!s_running || !s_osd)
		return false;
	return s_osd->state_save(data, size);
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
	if (!s_running || !s_osd)
		return false;
	if (!s_osd->state_load(data, size))
		return false;

	// 🚨 MAME's state is not the whole core's state. The frame record still holds the polygon list
	// from before the load, and the renderer redraws the last list whenever a frame carries no new
	// geometry — which is exactly what P3 step 8 fixed for the empty-display-list case, and a load
	// lands in the same shape. Without this the first post-load frame can composite the OLD scene
	// under the new one. geometry_none() marks the record valid with poly_count = 0 and bumps the
	// serial: "a new frame that is empty", not "no news".
	m2vk::geometry_none();
	return true;
}


//============================================================
//  libretro: the rest of the ABI
//============================================================

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { }
