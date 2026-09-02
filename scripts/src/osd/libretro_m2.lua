-- license:BSD-3-Clause
-- copyright-holders:mcwild77

---------------------------------------------------------------------------
--
--   libretro_m2.lua
--
--   Build rules for the Model 2 libretro OSD.
--
--   Modeled on scripts/src/osd/sdl.lua, but with no SDL, X11, OpenGL or bgfx
--   linkage: the core is a self-contained shared library that talks to the
--   frontend over libretro.h and owns no window of its own.
--
--   Two things here are worth knowing before editing:
--
--   * maintargetosdoptions() re-issues kind "SharedLib".  mainProject() in
--     scripts/src/main.lua hard-codes kind "ConsoleApp" and then calls this
--     function from inside the same project scope, so the later call wins.  That
--     is how the core is built without patching main.lua.
--
--   * libretro_m2modulesbuild() compiles only the cross-platform "none" backends.
--     osd_common_t::register_options() names roughly fifty module symbols, most of
--     them unconditionally, so the ones not compiled here must still resolve at
--     link time — src/osd/libretro_m2/module_stubs.cpp supplies them.  A link error
--     naming a missing module symbol means one line needs adding there.
--
---------------------------------------------------------------------------

dofile("modules.lua")


---------------------------------------------------------------------------
--
--   Vulkan headers.
--
--   Headers only: the core links no Vulkan library and never will.  Every entry point is resolved
--   at run time from the vkGetInstanceProcAddr the frontend supplies, so there is nothing here to
--   put on a link line -- see src/osd/libretro_m2/renderer_vk/vk_funcs.h for the reasoning, and
--   devnotes/p2-vulkan-passthrough.md for the consequences.
--
--   An environment variable rather than a genie --option: options reach genie only through the
--   PARAMS list in the top-level makefile, and that is an upstream file this fork does not patch.
--
---------------------------------------------------------------------------

VULKAN_INCLUDEDIR = os.getenv("M2VK_VULKAN_INCLUDEDIR")
if (VULKAN_INCLUDEDIR == nil) or (VULKAN_INCLUDEDIR == "") then
	if _OPTIONS["targetos"]=="macosx" then
		VULKAN_INCLUDEDIR = "/opt/homebrew/include"
	else
		VULKAN_INCLUDEDIR = "/usr/include"
	end
end

if not os.isfile(path.join(VULKAN_INCLUDEDIR, "vulkan", "vulkan.h")) then
	error("\n"
		.. "vulkan/vulkan.h was not found under '" .. VULKAN_INCLUDEDIR .. "'.\n"
		.. "The Model 2 libretro core needs the Vulkan headers at build time (it links no Vulkan\n"
		.. "library).  On macOS: brew install vulkan-headers.  Otherwise install your distribution's\n"
		.. "vulkan-headers package, or set M2VK_VULKAN_INCLUDEDIR to the prefix that contains\n"
		.. "vulkan/vulkan.h and re-run with REGENIE=1.\n", 0)
end


-- Trimmed counterpart to osdmodulesbuild().  Deliberately excludes bgfx (drawbgfx.cpp
-- includes the OSD-specific window.h and drags in some eighty more files) and imgui,
-- which are the only two backends that do not guard their platform includes.
function libretro_m2modulesbuild()

	removeflags {
		"SingleOutputDir",
	}

	files {
		MAME_DIR .. "src/osd/watchdog.cpp",
		MAME_DIR .. "src/osd/watchdog.h",
		MAME_DIR .. "src/osd/interface/audio.cpp",
		MAME_DIR .. "src/osd/interface/audio.h",
		MAME_DIR .. "src/osd/interface/inputcode.h",
		MAME_DIR .. "src/osd/interface/inputdev.h",
		MAME_DIR .. "src/osd/interface/inputfwd.h",
		MAME_DIR .. "src/osd/interface/inputman.h",
		MAME_DIR .. "src/osd/interface/inputseq.cpp",
		MAME_DIR .. "src/osd/interface/inputseq.h",
		MAME_DIR .. "src/osd/interface/midiport.h",
		MAME_DIR .. "src/osd/interface/nethandler.cpp",
		MAME_DIR .. "src/osd/interface/nethandler.h",
		MAME_DIR .. "src/osd/interface/uievents.h",
		MAME_DIR .. "src/osd/modules/lib/osdobj_common.cpp",
		MAME_DIR .. "src/osd/modules/lib/osdobj_common.h",
		MAME_DIR .. "src/osd/modules/debugger/debug_module.h",
		MAME_DIR .. "src/osd/modules/debugger/none.cpp",
		MAME_DIR .. "src/osd/modules/debugger/xmlconfig.cpp",
		MAME_DIR .. "src/osd/modules/debugger/xmlconfig.h",
		MAME_DIR .. "src/osd/modules/diagnostics/diagnostics_module.h",
		MAME_DIR .. "src/osd/modules/diagnostics/none.cpp",
		MAME_DIR .. "src/osd/modules/font/font_module.h",
		MAME_DIR .. "src/osd/modules/font/font_none.cpp",
		MAME_DIR .. "src/osd/modules/input/assignmenthelper.cpp",
		MAME_DIR .. "src/osd/modules/input/assignmenthelper.h",
		MAME_DIR .. "src/osd/modules/input/input_common.cpp",
		MAME_DIR .. "src/osd/modules/input/input_common.h",
		MAME_DIR .. "src/osd/modules/input/input_module.h",
		MAME_DIR .. "src/osd/modules/input/input_none.cpp",
		MAME_DIR .. "src/osd/modules/midi/midi_module.h",
		MAME_DIR .. "src/osd/modules/midi/none.cpp",
		MAME_DIR .. "src/osd/modules/monitor/monitor_common.cpp",
		MAME_DIR .. "src/osd/modules/monitor/monitor_common.h",
		MAME_DIR .. "src/osd/modules/monitor/monitor_module.h",
		MAME_DIR .. "src/osd/modules/netdev/netdev_common.cpp",
		MAME_DIR .. "src/osd/modules/netdev/netdev_common.h",
		MAME_DIR .. "src/osd/modules/netdev/netdev_module.h",
		MAME_DIR .. "src/osd/modules/netdev/none.cpp",
		MAME_DIR .. "src/osd/modules/output/none.cpp",
		MAME_DIR .. "src/osd/modules/output/output_module.h",
		MAME_DIR .. "src/osd/modules/render/render_module.h",
		MAME_DIR .. "src/osd/modules/sound/none.cpp",
		MAME_DIR .. "src/osd/modules/sound/sound_module.cpp",
		MAME_DIR .. "src/osd/modules/sound/sound_module.h",
	}
	includedirs {
		MAME_DIR .. "src/osd",
		ext_includedir("asio"),
	}
	defines {
		"USE_OPENGL=0",
		"__STDC_LIMIT_MACROS",
		"__STDC_FORMAT_MACROS",
		"__STDC_CONSTANT_MACROS",
		"USE_QTDEBUG=0",
	}
end


-- Called from mainProject() in scripts/src/main.lua, inside the main project's scope.
function maintargetosdoptions(_target, _subtarget)
	osdmodulestargetconf()

	-- A libretro core is a shared library the frontend dlopen()s.  mainProject() has
	-- already said kind "ConsoleApp"; say otherwise now, while still in its scope.
	-- One unified subtarget (modelizer) carries all three families, so the core name is
	-- the constant "modelizer_libretro"; every launcher/symlink/harness path depends on
	-- that literal.  (The retired per-family subtargets used _subtarget here to keep
	-- their dylibs from clobbering each other -- no longer needed with a single build.)
	configuration { }
		kind "SharedLib"
		targetprefix ""
		targetname ("modelizer_libretro")

	configuration { "osx*" }
		targetextension ".dylib"
		-- osdlib_macosx.cpp's clipboard uses Carbon Pasteboard symbols
		links {
			"Carbon.framework",
		}

	configuration { "linux-* or freebsd" }
		targetextension ".so"

	-- RetroArch on Android looks for cores named <name>_libretro_android.so and strips the
	-- _android suffix again when it matches the core against its .info file, so the suffix is
	-- part of the ABI with the frontend rather than decoration.  targetprefix is restated
	-- because scripts/toolchain.lua's own android block may have had a say.
	configuration { "android-*" }
		targetextension ".so"
		targetprefix ""
		-- The "lib" prefix is deliberate: an APK only extracts/loads native libraries whose
		-- basename matches lib*.so (jniLibs / nativeLibraryDir), so a core meant to be bundled
		-- into a frontend APK must be named libmodelizer_libretro_android.so.  targetprefix stays
		-- "" and the prefix is carried in the name itself so both agree with the soname below.
		targetname ("libmodelizer_libretro_android")
		-- Reissued here because mainProject()'s own android block, which is where a genie build
		-- normally gets these, is skipped for this OSD -- it also links SDL2 and GLES.  The soname
		-- matters: Android's loader dedupes by soname, and mainProject()'s is the generic
		-- "libmain.so", which is a collision waiting to happen inside a frontend's process.
		linkoptions {
			"-shared",
			-- Matches the basename (libmodelizer_libretro_android.so); the soname and the
			-- basename must agree or the frontend's loader dedupe and .info matching break.
			"-Wl,-soname,libmodelizer_libretro_android.so",
			-- The NDK's clang links libc++_shared.so by default, which would make the core
			-- undlopenable in any frontend whose APK does not happen to ship that library --
			-- a runtime failure on the phone, discovered late, with nothing in the build to
			-- suggest it.  Static libc++ makes the core self-contained; --exclude-libs then
			-- keeps every symbol that came out of a static archive out of the dynamic symbol
			-- table, so the core's private libc++ cannot bind against the frontend's.  The
			-- retro_* entry points are unaffected: retro_entry.cpp is a direct object in this
			-- link, not a member of an archive.
			"-static-libstdc++",
			"-Wl,--exclude-libs,ALL",
		}

	configuration { "mingw* or vs*" }
		targetextension ".dll"

	configuration { }

	-- retro_entry.cpp is compiled into the main target rather than into osd_libretro_m2.
	-- Nothing inside the core references the retro_* entry points — the frontend looks them
	-- up by name after dlopen() — so as members of a static archive they would never be
	-- pulled into the link and the library would export nothing at all.
	files {
		MAME_DIR .. "src/osd/libretro_m2/retro_entry.cpp",
	}
	includedirs {
		MAME_DIR .. "src/osd/libretro_m2",
		VULKAN_INCLUDEDIR,
	}

	-- Android is excluded deliberately: bionic has no libpthread to link (it lives in libc), so
	-- -lpthread is a hard link error there, and scripts/toolchain.lua's android block already
	-- supplies c/dl/m/android/log.
	if _OPTIONS["targetos"]~="windows" and _OPTIONS["targetos"]~="macosx" and _OPTIONS["targetos"]~="asmjs" and _OPTIONS["targetos"]~="android" then
		links {
			"m",
			"pthread",
		}
	end
end


BASE_TARGETOS       = "unix"
LIBRETRO_M2_TARGETOS = "unix"
if _OPTIONS["targetos"]=="windows" then
	BASE_TARGETOS       = "win32"
	LIBRETRO_M2_TARGETOS = "win32"
elseif _OPTIONS["targetos"]=="macosx" then
	LIBRETRO_M2_TARGETOS = "macosx"
end


-- qtdbg_libretro_m2: main.lua links qtdbg_<osd> unconditionally.  With USE_QTDEBUG=0
-- this is just the debugqt.cpp stub; mirror the sdl.lua project shell.
project ("qtdbg_" .. _OPTIONS["osd"])
	uuid (os.uuid("qtdbg_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	dofile("libretro_m2_cfg.lua")
	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "src/osd/modules/render",
		MAME_DIR .. "3rdparty",
	}
	configuration { "linux-* or freebsd" }
		buildoptions {
			"-fPIC",
		}
	configuration { }

	qtdebuggerbuild()


-- osd_libretro_m2: the OSD layer, the libretro ABI, and the backend modules.
project ("osd_" .. _OPTIONS["osd"])
	uuid (os.uuid("osd_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	dofile("libretro_m2_cfg.lua")
	libretro_m2modulesbuild()

	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "src/osd/modules/file",
		MAME_DIR .. "src/osd/modules/render",
		MAME_DIR .. "3rdparty",
		MAME_DIR .. "src/osd/libretro_m2",
		VULKAN_INCLUDEDIR,
	}

	configuration { "linux-* or freebsd" }
		buildoptions {
			"-fPIC",
		}
	configuration { }

	-- Expose the M2VK-guarded i8251 baud accessors (i8251.h) and m2vk_sink.h paths
	-- to the OSD sources; the driver project (mame_model2) already defines this.
	defines {
		"M2VK",
	}

	files {
		MAME_DIR .. "src/osd/osdepend.h",
		MAME_DIR .. "src/osd/libretro_m2/libretro.h",
		MAME_DIR .. "src/osd/libretro_m2/libretro_vulkan.h",
		MAME_DIR .. "src/osd/libretro_m2/libretro_m2_osd.h",
		MAME_DIR .. "src/osd/libretro_m2/libretro_m2_osd.cpp",
		MAME_DIR .. "src/osd/libretro_m2/libretro_m2_input.h",
		MAME_DIR .. "src/osd/libretro_m2/libretro_m2_input.cpp",
		MAME_DIR .. "src/osd/libretro_m2/retro_options.h",
		MAME_DIR .. "src/osd/libretro_m2/retro_options.cpp",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_soundthread.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_soundthread.cpp",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_baud.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_baud.cpp",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_sink.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_sink.cpp",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_polytap.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_frame.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_frame.cpp",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_reticle.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_reticle.cpp",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_steerbar.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_steerbar.cpp",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_savestate.h",
		MAME_DIR .. "src/osd/libretro_m2/m2vk_savestate.cpp",
		MAME_DIR .. "src/osd/libretro_m2/module_stubs.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_funcs.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_funcs.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_context.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_context.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_present.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_present.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_geom.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/vk_geom.cpp",
		-- The System 22 seam and untextured GPU pass (S2).  Compiled into the shared OSD for both builds
		-- so retro_entry.cpp and vk_present.cpp resolve the s22:: symbols in the model2 build too, where
		-- the code is inert (nothing turns capture on).  The seam header is driver-type-free; the driver
		-- includes it under S22VK and calls submit_quad, which the namcos22.lua block scopes.
		MAME_DIR .. "src/osd/libretro_m2/s22_seam.h",
		MAME_DIR .. "src/osd/libretro_m2/s22_seam.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/s22_geom.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/s22_geom.cpp",
		-- The System 21 seam and flat untextured GPU pass (T2), same arrangement as System 22: compiled
		-- into the shared OSD for every build so retro_entry.cpp and vk_present.cpp resolve the s21::
		-- symbols in the model2 / namcos22 builds too, where the code is inert (nothing turns capture on).
		-- The seam header is driver-type-free; namcos21_3d.cpp / namcos21_c67.cpp include it under S21VK,
		-- which the namcos21.lua block scopes.
		MAME_DIR .. "src/osd/libretro_m2/s21_seam.h",
		MAME_DIR .. "src/osd/libretro_m2/s21_seam.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/s21_geom.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/s21_geom.cpp",
		-- The Sega Model 1 seam (M1-1), same arrangement as System 21/22: compiled into the shared OSD for
		-- every build so retro_entry.cpp resolves the m1:: symbols everywhere, inert until a Model 1 driver
		-- arms it. The seam header is driver-type-free; model1_v.cpp includes it under M1VK, which the
		-- modelizer.lua mame_model1 block scopes. (renderer_vk/m1_geom is added at M1-2.)
		MAME_DIR .. "src/osd/libretro_m2/m1_seam.h",
		MAME_DIR .. "src/osd/libretro_m2/m1_seam.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/m1_geom.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/m1_geom.cpp",
		-- The System 23 seam (23-1), same arrangement as System 21/22/Model 1: compiled into the shared
		-- OSD for every build so its symbols resolve everywhere, inert until a System 23 driver arms it.
		-- The seam header is driver-type-free; namcos23.cpp includes it under S23VK, which the
		-- modelizer.lua mame_namcos23 block scopes.
		MAME_DIR .. "src/osd/libretro_m2/s23_seam.h",
		MAME_DIR .. "src/osd/libretro_m2/s23_seam.cpp",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/s23_geom.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/s23_geom.cpp",
		-- Generated by renderer_vk/shaders/build_shaders.sh and committed; the GLSL sources and the
		-- script beside them are not listed because genie has no rule for them and none is wanted --
		-- see devnotes/p2-vulkan-passthrough.md for why the shaders are compiled offline.
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/fullscreen_vert_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/passthrough_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/overlay_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/reticle_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/steerbar_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/poly_vert_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/poly_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/s22_vert_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/s22_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/s21_vert_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/s21_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/m1_vert_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/m1_frag_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/s23_vert_spv.h",
		MAME_DIR .. "src/osd/libretro_m2/renderer_vk/shaders/s23_frag_spv.h",
	}


-- ocore_libretro_m2: the OS-core (file/sync/strconv) layer.  Same set as sdl's ocore.
project ("ocore_" .. _OPTIONS["osd"])
	uuid (os.uuid("ocore_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	removeflags {
		"SingleOutputDir",
	}

	dofile("libretro_m2_cfg.lua")

	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "src/osd/libretro_m2",
	}

	configuration { "linux-* or freebsd" }
		buildoptions {
			"-fPIC",
		}
	configuration { }

	files {
		MAME_DIR .. "src/osd/osdcore.cpp",
		MAME_DIR .. "src/osd/osdcore.h",
		MAME_DIR .. "src/osd/osdfile.h",
		MAME_DIR .. "src/osd/strconv.cpp",
		MAME_DIR .. "src/osd/strconv.h",
		MAME_DIR .. "src/osd/osdsync.cpp",
		MAME_DIR .. "src/osd/osdsync.h",
		MAME_DIR .. "src/osd/modules/osdmodule.cpp",
		MAME_DIR .. "src/osd/modules/osdmodule.h",
		MAME_DIR .. "src/osd/modules/lib/osdlib_" .. LIBRETRO_M2_TARGETOS .. ".cpp",
		MAME_DIR .. "src/osd/modules/lib/osdlib.h",
	}

	if BASE_TARGETOS=="unix" then
		files {
			MAME_DIR .. "src/osd/modules/file/posixdir.cpp",
			MAME_DIR .. "src/osd/modules/file/posixfile.cpp",
			MAME_DIR .. "src/osd/modules/file/posixfile.h",
			MAME_DIR .. "src/osd/modules/file/posixptty.cpp",
			MAME_DIR .. "src/osd/modules/file/posixsocket.cpp",
		}
	elseif BASE_TARGETOS=="win32" then
		includedirs {
			MAME_DIR .. "src/osd/windows",
		}
		files {
			MAME_DIR .. "src/osd/modules/file/windir.cpp",
			MAME_DIR .. "src/osd/modules/file/winfile.cpp",
			MAME_DIR .. "src/osd/modules/file/winfile.h",
			MAME_DIR .. "src/osd/modules/file/winptty.cpp",
			MAME_DIR .. "src/osd/modules/file/winsocket.cpp",
			MAME_DIR .. "src/osd/windows/winutil.cpp",
			MAME_DIR .. "src/osd/windows/winutil.h",
		}
	else
		files {
			MAME_DIR .. "src/osd/modules/file/stdfile.cpp",
		}
	end
