-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   namcos22.lua
--
--   Namco (Super) System 22 subtarget: builds only the namcos22 driver and
--   the devices it needs.  Use make SUBTARGET=namcos22 to build.
--
--   Everything below the header is the verbatim output of
--
--       python3 scripts/build/makedep.py -r . sourcesproject -t namcos22 \
--           -l src/mame/mame.lst src/mame/namco/namcos22.cpp
--
--   Carries three HAND-ADDED blocks: the GEN_FIFO line below, one that defines
--   S22VK for the driver sources (the seam's scope), and one that compiles the
--   seam's sink (s22_seam.cpp) into this driver project so the symbols resolve
--   whatever OSD is selected.  Unlike model2.lua the sink is compiled here for
--   BOTH OSDs: it is System-22-specific and the shared libretro_m2 OSD must not
--   carry it.  Regenerate the generator output with the command above after an
--   upstream merge and re-apply the marked blocks.
--
---------------------------------------------------------------------------

CPUS["M37710"] = true
CPUS["M680X0"] = true
CPUS["TMS320C2X"] = true
MACHINES["EEPROMDEV"] = true
MACHINES["EEPROMDEV"] = true
SOUNDS["C352"] = true
SOUNDS["MB87077"] = true

-- HAND-ADDED: the shared libretro_m2 OSD's savestate module (m2vk_savestate.cpp)
-- links generic_fifo_device_base unconditionally (the Model 2 gen_fifo trailer).
-- namcos22 has no GEN_FIFO of its own, so pull the device in to satisfy the symbol;
-- with no such device instantiated it is inert at runtime.
MACHINES["GEN_FIFO"] = true

function createProjects_mame_namcos22(_target, _subtarget)
    project ("mame_namcos22")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-namcos22"))
    addprojectflags()

    -- HAND-ADDED: scopes the S1 seam hooks in namcos22_v.cpp to this subtarget,
    -- so no other build sees them.
    defines {
        "S22VK",
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
        MAME_DIR .. "src/mame/namco/namco_dsp.cpp",
        MAME_DIR .. "src/mame/namco/namco_dsp.h",
        MAME_DIR .. "src/mame/namco/namcomcu.cpp",
        MAME_DIR .. "src/mame/namco/namcomcu.h",
        MAME_DIR .. "src/mame/namco/namcos22.cpp",
        MAME_DIR .. "src/mame/namco/namcos22.h",
        MAME_DIR .. "src/mame/namco/namcos22_v.cpp",
    }

    -- HAND-ADDED: the seam in namcos22_v.cpp calls into the System 22 sink and GPU pass, which live in
    -- the shared libretro_m2 OSD (s22_seam.cpp / renderer_vk/s22_geom.cpp) rather than here.  The OSD
    -- is linked into BOTH the model2 and namcos22 builds, so retro_entry.cpp and vk_present.cpp -- which
    -- both reference the s22:: symbols -- resolve in either build; compiling them here instead would
    -- leave the model2 build's references undefined.  In the model2 build the S22 code is inert:
    -- nothing ever turns capture on.  (S1 compiled the seam here; S2 moved it for that link reason.)
end

function linkProjects_mame_namcos22(_target, _subtarget)
    links {
        "mame_namcos22",
    }
end
