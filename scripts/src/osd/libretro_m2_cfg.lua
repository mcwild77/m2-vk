-- license:BSD-3-Clause
-- copyright-holders:mcwild77

---------------------------------------------------------------------------
--
--   libretro_m2_cfg.lua
--
--   Per-project configuration for the Model 2 libretro OSD.
--
--   The counterpart to sdl_cfg.lua, and it deliberately defines none of
--   OSD_SDL / SDLMAME_* / OSD_WINDOWS / USE_OPENGL.  That omission is what makes
--   every SDL, OpenGL and platform backend collapse to its MODULE_NOT_SUPPORTED
--   stub, so this build needs no SDL, X11 or OpenGL headers at all.  The handful
--   of backends that do not self-stub are supplied by module_stubs.cpp instead.
--
---------------------------------------------------------------------------

dofile('modules.lua')

defines {
	"OSD_LIBRETRO_M2",
	"NO_USE_MIDI",
	"NO_USE_PORTAUDIO",
	"NO_USE_PULSEAUDIO",
	"NO_USE_PIPEWIRE",
}

-- Note for macOS: SDLMAME_MACOSX is intentionally NOT defined.  osdlib_macosx.cpp
-- needs no SDL and no such define, and leaving it undefined keeps DEBUG_OSX out of
-- osd_common_t::register_options() (osdobj_common.cpp:277) — one less stub to carry.

if _OPTIONS["targetos"]=="windows" then
	configuration { "mingw* or vs*" }
		defines {
			"UNICODE",
			"_UNICODE",
			"_WIN32_WINNT=0x0600",
			"WIN32_LEAN_AND_MEAN",
			"NOMINMAX",
		}
	configuration { }
end

configuration { "osx*" }
	includedirs {
		MAME_DIR .. "3rdparty/bx/include/compat/osx",
	}
configuration { }
