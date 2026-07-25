-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   model2.lua
--
--   Sega Model 2 subtarget: builds only the Model 2 driver and the devices
--   it needs.  Use make SUBTARGET=model2 to build.
--
--   Everything below the header is the verbatim output of
--
--       python3 scripts/build/makedep.py -r . sourcesproject -t model2 \
--           -l src/mame/mame.lst src/mame/sega/model2.cpp
--
--   plus two blocks marked HAND-ADDED: one defines M2VK for the driver
--   sources, the other makes sure the seam's sink is linked whatever OSD is
--   selected.  Regenerate with that command after an upstream merge and
--   re-apply the marked blocks; keeping the rest byte-identical to the
--   generator output keeps those diffs readable.
--
---------------------------------------------------------------------------

BUSES["HEATHZENITH_H19"] = true
BUSES["RS232"] = true
BUSES["SUNKBD"] = true
CPUS["I960"] = true
CPUS["IE15"] = true
CPUS["M6800"] = true
CPUS["M6800"] = true
CPUS["M680X0"] = true
CPUS["MB86233"] = true
CPUS["MB86235"] = true
CPUS["ADSP2106X"] = true
CPUS["Z80"] = true
CPUS["Z80"] = true
MACHINES["6821PIA"] = true
MACHINES["ACIA6850"] = true
MACHINES["CXD1095"] = true
MACHINES["EEPROMDEV"] = true
MACHINES["EEPROMDEV"] = true
MACHINES["GEN_FIFO"] = true
MACHINES["I8251"] = true
MACHINES["IE15"] = true
MACHINES["INPUT_MERGER"] = true
MACHINES["INS8250"] = true
MACHINES["MB3773"] = true
MACHINES["MB8421"] = true
MACHINES["MM5740"] = true
MACHINES["MSM6253"] = true
MACHINES["PCF8573"] = true
MACHINES["SWTPC8212"] = true
MACHINES["VOTRAXTNT"] = true
MACHINES["Z80CTC"] = true
MACHINES["Z80DAISY"] = true
MACHINES["Z80PIO"] = true
MACHINES["Z80SIO"] = true
SOUNDS["AY8910"] = true
SOUNDS["BEEP"] = true
SOUNDS["MPEG_AUDIO"] = true
SOUNDS["MULTIPCM"] = true
SOUNDS["SCSP"] = true
SOUNDS["VOTRAX_SC01"] = true
SOUNDS["VOTRAX_SC01A"] = true
SOUNDS["YM2203"] = true
SOUNDS["YM2608"] = true
SOUNDS["YM2610"] = true
SOUNDS["YM2612"] = true
VIDEOS["HD44780"] = true
VIDEOS["MC6845"] = true

function createProjects_mame_model2(_target, _subtarget)
    project ("mame_model2")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-model2"))
    addprojectflags()

    -- HAND-ADDED: scopes the hardware-renderer hooks in model2_v.cpp to this
    -- subtarget, so no other build sees them.
    defines {
        "M2VK",
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
        MAME_DIR .. "src/mame/sega/315-5838_317-0229_comp.cpp",
        MAME_DIR .. "src/mame/sega/315-5838_317-0229_comp.h",
        MAME_DIR .. "src/mame/sega/315-5881_crypt.cpp",
        MAME_DIR .. "src/mame/sega/315-5881_crypt.h",
        MAME_DIR .. "src/mame/sega/315_5296.cpp",
        MAME_DIR .. "src/mame/sega/315_5296.h",
        MAME_DIR .. "src/mame/sega/315_5338a.cpp",
        MAME_DIR .. "src/mame/sega/315_5338a.h",
        MAME_DIR .. "src/mame/sega/315_5649.cpp",
        MAME_DIR .. "src/mame/sega/315_5649.h",
        MAME_DIR .. "src/mame/sega/dsb2.cpp",
        MAME_DIR .. "src/mame/sega/dsb2.h",
        MAME_DIR .. "src/mame/sega/dsbz80.cpp",
        MAME_DIR .. "src/mame/sega/dsbz80.h",
        MAME_DIR .. "src/mame/sega/m2comm.cpp",
        MAME_DIR .. "src/mame/sega/m2comm.h",
        MAME_DIR .. "src/mame/sega/model1io.cpp",
        MAME_DIR .. "src/mame/sega/model1io.h",
        MAME_DIR .. "src/mame/sega/model1io2.cpp",
        MAME_DIR .. "src/mame/sega/model1io2.h",
        MAME_DIR .. "src/mame/sega/model2.cpp",
        MAME_DIR .. "src/mame/sega/model2.h",
        MAME_DIR .. "src/mame/sega/model2_m.cpp",
        MAME_DIR .. "src/mame/sega/model2_v.cpp",
        MAME_DIR .. "src/mame/sega/model2rd.ipp",
        MAME_DIR .. "src/mame/sega/segabill.cpp",
        MAME_DIR .. "src/mame/sega/segabill.h",
        MAME_DIR .. "src/mame/sega/segaic24.cpp",
        MAME_DIR .. "src/mame/sega/segaic24.h",
        MAME_DIR .. "src/mame/shared/segam1audio.cpp",
        MAME_DIR .. "src/mame/shared/segam1audio.h",
    }

    -- HAND-ADDED: the seam in model2_v.cpp calls into the sink in
    -- src/osd/libretro_m2, which the libretro OSD compiles and owns.  Any other
    -- OSD does not, so compile it here instead and the plain SUBTARGET=model2
    -- binary still links (and still carries the polygon tap).  Only ever one of
    -- the two, or the symbols are defined twice.
    if _OPTIONS["osd"] ~= "libretro_m2" then
        files {
            MAME_DIR .. "src/osd/libretro_m2/m2vk_sink.h",
            MAME_DIR .. "src/osd/libretro_m2/m2vk_sink.cpp",
            MAME_DIR .. "src/osd/libretro_m2/m2vk_polytap.h",
        }
    end
end

function linkProjects_mame_model2(_target, _subtarget)
    links {
        "mame_model2",
    }
end
