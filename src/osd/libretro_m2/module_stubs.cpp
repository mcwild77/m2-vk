// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro OSD — stand-ins for the backend modules this core does not build.

    osd_common_t::register_options() (src/osd/modules/lib/osdobj_common.cpp) names roughly fifty
    module symbols, and most of the references sit outside any #ifdef. That function is compiled
    into osdobj_common.o, which every OSD links, so all of those symbols must resolve even though
    this core never registers or selects a single one of them.

    Most backends solve that themselves: they guard their platform includes on OSD_SDL / SDLMAME_* /
    OSD_WINDOWS / USE_OPENGL and collapse to a MODULE_NOT_SUPPORTED stub when none are defined, so
    compiling them costs nothing and needs no external headers. This file covers the remainder —
    the backends that are not compiled at all because they do not self-stub (drawbgfx.cpp includes
    the OSD-specific window.h and pulls in the whole bgfx tree; debugimgui.cpp likewise) or because
    they belong to a platform this build does not target.

    Maintenance: a link error naming an undefined module symbol means upstream added a backend and
    it needs one more line here. That is deliberate — a loud, obvious failure in a file we own,
    rather than a patch to osdobj_common.cpp that would conflict on every merge.

    The counterpart list of what IS compiled lives in scripts/src/osd/libretro_m2.lua.

*********************************************************************************************************************************/

#include "modules/osdmodule.h"

#include "modules/debugger/debug_module.h"
#include "modules/font/font_module.h"
#include "modules/input/input_module.h"
#include "modules/midi/midi_module.h"
#include "modules/monitor/monitor_module.h"
#include "modules/netdev/netdev_module.h"
#include "modules/output/output_module.h"
#include "modules/render/render_module.h"
#include "modules/sound/sound_module.h"


namespace osd {

namespace {

// fonts — the platform providers
MODULE_NOT_SUPPORTED(font_osx_stub, OSD_FONT_PROVIDER, "osx")
MODULE_NOT_SUPPORTED(font_windows_stub, OSD_FONT_PROVIDER, "gdi")
MODULE_NOT_SUPPORTED(font_dwrite_stub, OSD_FONT_PROVIDER, "dwrite")
MODULE_NOT_SUPPORTED(font_sdl_stub, OSD_FONT_PROVIDER, "sdl")

// renderers — including "none", which despite the name is still a renderer *for a window*
// (drawnone.cpp calls osd_window::pixel_aspect()), and this core has no windows
MODULE_NOT_SUPPORTED(video_none_stub, OSD_RENDERER_PROVIDER, "none")
MODULE_NOT_SUPPORTED(video_bgfx_stub, OSD_RENDERER_PROVIDER, "bgfx")
MODULE_NOT_SUPPORTED(video_gdi_stub, OSD_RENDERER_PROVIDER, "gdi")
MODULE_NOT_SUPPORTED(video_opengl_stub, OSD_RENDERER_PROVIDER, "opengl")
MODULE_NOT_SUPPORTED(video_sdl2_stub, OSD_RENDERER_PROVIDER, "accel")
MODULE_NOT_SUPPORTED(video_sdl1_stub, OSD_RENDERER_PROVIDER, "soft")

// sound
MODULE_NOT_SUPPORTED(sound_wasapi_stub, OSD_SOUND_PROVIDER, "wasapi")
MODULE_NOT_SUPPORTED(sound_xaudio2_stub, OSD_SOUND_PROVIDER, "xaudio2")
MODULE_NOT_SUPPORTED(sound_coreaudio_stub, OSD_SOUND_PROVIDER, "coreaudio")
MODULE_NOT_SUPPORTED(sound_js_stub, OSD_SOUND_PROVIDER, "js")
MODULE_NOT_SUPPORTED(sound_sdl_stub, OSD_SOUND_PROVIDER, "sdl")

// monitors — none are compiled; this core never selects a monitor module
MODULE_NOT_SUPPORTED(monitor_sdl_stub, OSD_MONITOR_PROVIDER, "sdl")
MODULE_NOT_SUPPORTED(monitor_win32_stub, OSD_MONITOR_PROVIDER, "win32")
MODULE_NOT_SUPPORTED(monitor_dxgi_stub, OSD_MONITOR_PROVIDER, "dxgi")
MODULE_NOT_SUPPORTED(monitor_mac_stub, OSD_MONITOR_PROVIDER, "mac")

// debuggers
MODULE_NOT_SUPPORTED(debug_windows_stub, OSD_DEBUG_PROVIDER, "windows")
MODULE_NOT_SUPPORTED(debug_qt_stub, OSD_DEBUG_PROVIDER, "qt")
MODULE_NOT_SUPPORTED(debug_imgui_stub, OSD_DEBUG_PROVIDER, "imgui")
MODULE_NOT_SUPPORTED(debug_gdbstub_stub, OSD_DEBUG_PROVIDER, "gdbstub")

// network
MODULE_NOT_SUPPORTED(taptun_stub, OSD_NETDEV_PROVIDER, "taptun")
MODULE_NOT_SUPPORTED(pcap_stub, OSD_NETDEV_PROVIDER, "pcap")

// input — the platform providers; input_none.cpp supplies the "none" variants
MODULE_NOT_SUPPORTED(keyboard_rawinput_stub, OSD_KEYBOARDINPUT_PROVIDER, "rawinput")
MODULE_NOT_SUPPORTED(keyboard_dinput_stub, OSD_KEYBOARDINPUT_PROVIDER, "dinput")
MODULE_NOT_SUPPORTED(keyboard_win32_stub, OSD_KEYBOARDINPUT_PROVIDER, "win32")
MODULE_NOT_SUPPORTED(mouse_rawinput_stub, OSD_MOUSEINPUT_PROVIDER, "rawinput")
MODULE_NOT_SUPPORTED(mouse_dinput_stub, OSD_MOUSEINPUT_PROVIDER, "dinput")
MODULE_NOT_SUPPORTED(mouse_win32_stub, OSD_MOUSEINPUT_PROVIDER, "win32")
MODULE_NOT_SUPPORTED(lightgun_x11_stub, OSD_LIGHTGUNINPUT_PROVIDER, "x11")
MODULE_NOT_SUPPORTED(lightgun_rawinput_stub, OSD_LIGHTGUNINPUT_PROVIDER, "rawinput")
MODULE_NOT_SUPPORTED(lightgun_win32_stub, OSD_LIGHTGUNINPUT_PROVIDER, "win32")
MODULE_NOT_SUPPORTED(joystick_winhybrid_stub, OSD_JOYSTICKINPUT_PROVIDER, "winhybrid")
MODULE_NOT_SUPPORTED(joystick_dinput_stub, OSD_JOYSTICKINPUT_PROVIDER, "dinput")
MODULE_NOT_SUPPORTED(joystick_xinput_stub, OSD_JOYSTICKINPUT_PROVIDER, "xinput")

// output
MODULE_NOT_SUPPORTED(output_console_stub, OSD_OUTPUT_PROVIDER, "console")
MODULE_NOT_SUPPORTED(output_network_stub, OSD_OUTPUT_PROVIDER, "network")
MODULE_NOT_SUPPORTED(output_win32_stub, OSD_OUTPUT_PROVIDER, "windows")

} // anonymous namespace

} // namespace osd


MODULE_DEFINITION(FONT_OSX, osd::font_osx_stub)
MODULE_DEFINITION(FONT_WINDOWS, osd::font_windows_stub)
MODULE_DEFINITION(FONT_DWRITE, osd::font_dwrite_stub)
MODULE_DEFINITION(FONT_SDL, osd::font_sdl_stub)

MODULE_DEFINITION(RENDERER_NONE, osd::video_none_stub)
MODULE_DEFINITION(RENDERER_BGFX, osd::video_bgfx_stub)
MODULE_DEFINITION(RENDERER_GDI, osd::video_gdi_stub)
MODULE_DEFINITION(RENDERER_OPENGL, osd::video_opengl_stub)
MODULE_DEFINITION(RENDERER_SDL2, osd::video_sdl2_stub)
MODULE_DEFINITION(RENDERER_SDL1, osd::video_sdl1_stub)

MODULE_DEFINITION(SOUND_WASAPI, osd::sound_wasapi_stub)
MODULE_DEFINITION(SOUND_XAUDIO2, osd::sound_xaudio2_stub)
MODULE_DEFINITION(SOUND_COREAUDIO, osd::sound_coreaudio_stub)
MODULE_DEFINITION(SOUND_JS, osd::sound_js_stub)
MODULE_DEFINITION(SOUND_SDL, osd::sound_sdl_stub)

MODULE_DEFINITION(MONITOR_SDL, osd::monitor_sdl_stub)
MODULE_DEFINITION(MONITOR_WIN32, osd::monitor_win32_stub)
MODULE_DEFINITION(MONITOR_DXGI, osd::monitor_dxgi_stub)
MODULE_DEFINITION(MONITOR_MAC, osd::monitor_mac_stub)

MODULE_DEFINITION(DEBUG_WINDOWS, osd::debug_windows_stub)
MODULE_DEFINITION(DEBUG_QT, osd::debug_qt_stub)
MODULE_DEFINITION(DEBUG_IMGUI, osd::debug_imgui_stub)
MODULE_DEFINITION(DEBUG_GDBSTUB, osd::debug_gdbstub_stub)

MODULE_DEFINITION(NETDEV_TAPTUN, osd::taptun_stub)
MODULE_DEFINITION(NETDEV_PCAP, osd::pcap_stub)

MODULE_DEFINITION(KEYBOARDINPUT_RAWINPUT, osd::keyboard_rawinput_stub)
MODULE_DEFINITION(KEYBOARDINPUT_DINPUT, osd::keyboard_dinput_stub)
MODULE_DEFINITION(KEYBOARDINPUT_WIN32, osd::keyboard_win32_stub)
MODULE_DEFINITION(MOUSEINPUT_RAWINPUT, osd::mouse_rawinput_stub)
MODULE_DEFINITION(MOUSEINPUT_DINPUT, osd::mouse_dinput_stub)
MODULE_DEFINITION(MOUSEINPUT_WIN32, osd::mouse_win32_stub)
MODULE_DEFINITION(LIGHTGUN_X11, osd::lightgun_x11_stub)
MODULE_DEFINITION(LIGHTGUNINPUT_RAWINPUT, osd::lightgun_rawinput_stub)
MODULE_DEFINITION(LIGHTGUNINPUT_WIN32, osd::lightgun_win32_stub)
MODULE_DEFINITION(JOYSTICKINPUT_WINHYBRID, osd::joystick_winhybrid_stub)
MODULE_DEFINITION(JOYSTICKINPUT_DINPUT, osd::joystick_dinput_stub)
MODULE_DEFINITION(JOYSTICKINPUT_XINPUT, osd::joystick_xinput_stub)

MODULE_DEFINITION(OUTPUT_CONSOLE, osd::output_console_stub)
MODULE_DEFINITION(OUTPUT_NETWORK, osd::output_network_stub)
MODULE_DEFINITION(OUTPUT_WIN32, osd::output_win32_stub)
