-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   namcos21.lua
--
--   Namco System 21 subtarget: builds the System 21 "C67" polygonizer driver
--   (Star Blade and siblings) AND the original Winning Run driver (namcos21.cpp:
--   Winning Run / Suzuka GP / '91).  Both front-ends draw through the same
--   namcos21_3d_device, so one seam covers the whole family.  Use
--   make SUBTARGET=namcos21 to build.
--
--   Everything in createProjects below is the verbatim output of
--
--       python3 scripts/build/makedep.py -r . sourcesproject -t namcos21 \
--           -l src/mame/mame.lst src/mame/namco/namcos21_c67.cpp \
--           src/mame/namco/namcos21.cpp
--
--   Carries two HAND-ADDED pieces: the GEN_FIFO MACHINES line (see below) and
--   the S21VK defines block that scopes the seam hooks to this subtarget.
--   Regenerate the generator output with the command above after an upstream
--   merge and re-apply both marked pieces.
--
---------------------------------------------------------------------------

CPUS["M6502"] = true
CPUS["M6502"] = true
CPUS["M6502"] = true
CPUS["M680X0"] = true
CPUS["M6805"] = true
CPUS["M6809"] = true
CPUS["TMS320C2X"] = true
SOUNDS["C140"] = true
SOUNDS["MB87077"] = true
SOUNDS["YM2151"] = true

-- HAND-ADDED: the shared libretro_m2 OSD's savestate module (m2vk_savestate.cpp)
-- links generic_fifo_device_base unconditionally (the Model 2 gen_fifo trailer).
-- System 21 has no GEN_FIFO of its own, so pull the device in to satisfy the
-- symbol; with no such device instantiated it is inert at runtime.
MACHINES["GEN_FIFO"] = true

function createProjects_mame_namcos21(_target, _subtarget)
    project ("mame_namcos21")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-namcos21"))
    addprojectflags()

    -- HAND-ADDED: scopes the T1 seam hooks in namcos21_3d.cpp to this subtarget, so no
    -- other build sees them.
    defines {
        "S21VK",
    }

    includedirs {
        MAME_DIR .. "src/osd",
        MAME_DIR .. "src/emu",
        MAME_DIR .. "src/devices",
        MAME_DIR .. "src/mame/shared",
        MAME_DIR .. "src/lib",
        MAME_DIR .. "src/lib/util",
        MAME_DIR .. "src/lib/netlist",
        MAME_DIR .. "3rdparty",
        GEN_DIR  .. "mame/layout",
        ext_includedir("asio"),
        ext_includedir("flac"),
        ext_includedir("glm"),
        ext_includedir("jpeg"),
        ext_includedir("rapidjson"),
        ext_includedir("zlib"),
    }

    files{
        MAME_DIR .. "src/mame/namco/namco65.cpp",
        MAME_DIR .. "src/mame/namco/namco65.h",
        MAME_DIR .. "src/mame/namco/namco68.cpp",
        MAME_DIR .. "src/mame/namco/namco68.h",
        MAME_DIR .. "src/mame/namco/namco_c139.cpp",
        MAME_DIR .. "src/mame/namco/namco_c139.h",
        MAME_DIR .. "src/mame/namco/namco_c148.cpp",
        MAME_DIR .. "src/mame/namco/namco_c148.h",
        MAME_DIR .. "src/mame/namco/namco_dsp.cpp",
        MAME_DIR .. "src/mame/namco/namco_dsp.h",
        MAME_DIR .. "src/mame/namco/namcoio_gearbox.cpp",
        MAME_DIR .. "src/mame/namco/namcoio_gearbox.h",
        MAME_DIR .. "src/mame/namco/namcos21.cpp",
        MAME_DIR .. "src/mame/namco/namcos21_3d.cpp",
        MAME_DIR .. "src/mame/namco/namcos21_3d.h",
        MAME_DIR .. "src/mame/namco/namcos21_c67.cpp",
        MAME_DIR .. "src/mame/namco/namcos21_dsp.cpp",
        MAME_DIR .. "src/mame/namco/namcos21_dsp.h",
        MAME_DIR .. "src/mame/namco/namcos21_dsp_c67.cpp",
        MAME_DIR .. "src/mame/namco/namcos21_dsp_c67.h",
        MAME_DIR .. "src/mame/shared/namco_c355spr.cpp",
        MAME_DIR .. "src/mame/shared/namco_c355spr.h",
    }

    -- The T2 seam and GPU pass (s21_seam.cpp / renderer_vk/s21_geom.cpp) live in the shared libretro_m2
    -- OSD, not here: retro_entry.cpp and vk_present.cpp (OSD) reference the s21:: symbols and must resolve
    -- in every build. (T1 compiled the seam into this driver project; T2 moved it for that link reason,
    -- exactly as System 22 did at S1->S2.) Only the S21VK-scoped hook calls stay in the driver sources.
end

function linkProjects_mame_namcos21(_target, _subtarget)
    links {
        "mame_namcos21",
    }
end
