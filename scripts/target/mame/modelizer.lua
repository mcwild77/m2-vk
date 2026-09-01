-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   modelizer.lua
--
--   Modelizer subtarget: one core carrying all five hardware-accelerated
--   families -- Sega Model 1, Sega Model 2, Namco (Super) System 22, Namco
--   System 21, Namco System 23 -- behind the shared Vulkan seam.  Use make
--   SUBTARGET=modelizer to build.
--
--   This is the UNION of the five per-family driver projects (originally
--   model1.lua + model2.lua + namcos22.lua + namcos21.lua; namcos23 was added
--   directly here, no standalone namcos23.lua ever existed).  Each family
--   keeps its OWN driver project and its own scoping define (M1VK / M2VK /
--   S22VK / S21VK / S23VK), so the
--   _v.cpp seam hooks stay scoped exactly as they are in the split builds --
--   merging the three into one project would force all three defines onto
--   every driver source.  The five projects link together into one binary.
--
--   The driver LIST (which GAME()s register in driver_list) comes from the
--   sibling src/mame/modelizer.flt, not from here; this file only says which
--   source files compile and how.  Runtime family detection keys off the
--   loaded driver's source file (retro_entry.cpp family_of()), so all three
--   flagships being present in one table is fine.
--
--   Regenerate the three per-family scripts with makedep.py after an upstream
--   merge (see their headers) and re-fold the changes here; keep the marked
--   HAND-ADDED blocks.
--
--   DE-DUPLICATIONS vs a literal concatenation:
--    * namco/namco_dsp.cpp is in both the namcos22 and namcos21 file lists.
--      Compiling it into both driver libraries would define namco_dsp twice (a
--      duplicate device registration).  It is compiled once, in mame_namcos22;
--      mame_namcos21's references resolve from that library at the final link.
--    * Model 1 shares six source files with Model 2 (segaic24, segam1audio,
--      dsbz80, model1io, model1io2, 315_5338a).  They are compiled once, in
--      mame_model2; mame_model1 compiles only its four unique sources
--      (model1, model1_m, model1_v, m1comm) and resolves the shared devices
--      from mame_model2 at the final link.
--
---------------------------------------------------------------------------

-- Union of the five families' global feature flags (BUSES/CPUS/MACHINES/SOUNDS/VIDEOS).
-- Setting a flag true more than once is harmless; the union is de-duplicated here for readability.
BUSES["HEATHZENITH_H19"] = true
BUSES["JVS"] = true            -- namcos23 (namcoio TSS-I/O board)
BUSES["RS232"] = true
BUSES["SUNKBD"] = true

CPUS["I960"] = true
CPUS["IE15"] = true
CPUS["M6800"] = true
CPUS["M680X0"] = true
CPUS["MB86233"] = true
CPUS["MB86235"] = true
CPUS["ADSP2106X"] = true
CPUS["Z80"] = true
CPUS["M37710"] = true          -- namcos22
CPUS["TMS320C2X"] = true       -- namcos22 + namcos21
CPUS["M6502"] = true           -- namcos21
CPUS["M6805"] = true           -- namcos21
CPUS["M6809"] = true           -- namcos21
CPUS["V60"] = true             -- model1 (main CPU)
CPUS["I386"] = true            -- model1
CPUS["F2MC16"] = true          -- namcos23 (TSS-I/O board MCU, mb90570)
CPUS["H8"] = true              -- namcos23 (H8/3002 subcpu)
CPUS["MIPS3"] = true           -- namcos23 (R4650BE main CPU)
CPUS["SH"] = true              -- namcos23 (SH7604, firewire/video subsystem)
CPUS["MCS48"] = true           -- s97801 rs232 terminal dep (I8035 keyboard MCU; added mame0289)
CPUS["MCS51"] = true           -- s97801 rs232 terminal dep (I8031 main MCU; added mame0289)

MACHINES["6821PIA"] = true
MACHINES["ACIA6850"] = true
MACHINES["AM9517A"] = true      -- model1
MACHINES["MB89374"] = true      -- model1
MACHINES["CXD1095"] = true
MACHINES["EEPROMDEV"] = true
MACHINES["GEN_FIFO"] = true    -- native to model2; also satisfies the m2vk_savestate gen_fifo trailer
MACHINES["I2CHLE"] = true      -- namcos23 (vpx3220a video decoder)
MACHINES["I8251"] = true
MACHINES["INTELFLASH"] = true  -- pulled in unconditionally by BUSES["JVS"]'s cyberlead.cpp (unused
                                -- LED-sign JVS peripheral, not a namcos23 dependency, but the JVS bus
                                -- flag compiles all of bus/jvs/* into the shared optional lib)
MACHINES["IE15"] = true
MACHINES["INPUT_MERGER"] = true
MACHINES["INS8250"] = true
MACHINES["MB3773"] = true
MACHINES["MB8421"] = true
MACHINES["MM5740"] = true
MACHINES["MSM6253"] = true
MACHINES["PCF8573"] = true
MACHINES["PS2INTC"] = true     -- pulled in unconditionally by VIDEOS["PS2GS"] (see PS2GIF/PS2GS above)
MACHINES["RTC4543"] = true     -- namcos23
MACHINES["S97801"] = true      -- backs BUSES["RS232"]'s s97801 terminal (added mame0289), like IE15/SWTPC8212
MACHINES["SCN_PCI"] = true      -- s97801 terminal dep (SCN2661B UART)
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
SOUNDS["C352"] = true          -- namcos22 + namcos23
SOUNDS["MB87077"] = true       -- namcos22 + namcos21
SOUNDS["C140"] = true          -- namcos21
SOUNDS["YM2151"] = true        -- namcos21

VIDEOS["HD44780"] = true
VIDEOS["SCN2674"] = true        -- s97801 rs232 terminal dep (SCN2672/2674 CRTC; added mame0289)
VIDEOS["MC6845"] = true
VIDEOS["PS2GIF"] = true        -- pulled in unconditionally by CPUS["MIPS3"]'s ps2vu.cpp/ps2vif1.cpp
VIDEOS["PS2GS"] = true         -- (PS2 vector-unit support code, not a namcos23 dependency either)


function createProjects_mame_modelizer(_target, _subtarget)

    --=====================================================================
    --  Sega Model 1  (from model1.lua)
    --=====================================================================
    project ("mame_model1")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-model1"))
    addprojectflags()

    -- HAND-ADDED: scopes the M1 seam hooks in model1_v.cpp to this driver
    -- project (no hooks yet at M1-0; the define is in place for M1-1).
    defines {
        "M1VK",
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
        MAME_DIR .. "src/mame/sega/m1comm.cpp",
        MAME_DIR .. "src/mame/sega/m1comm.h",
        MAME_DIR .. "src/mame/sega/model1.cpp",
        MAME_DIR .. "src/mame/sega/model1.h",
        MAME_DIR .. "src/mame/sega/model1_m.cpp",
        MAME_DIR .. "src/mame/sega/model1_v.cpp",
        -- segaic24 / segam1audio / dsbz80 / model1io / model1io2 / 315_5338a
        -- intentionally omitted: compiled once in mame_model2 (see header);
        -- the references resolve from that library at the final link.
    }

    --=====================================================================
    --  Sega Model 2  (from model2.lua)
    --=====================================================================
    project ("mame_model2")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-model2"))
    addprojectflags()

    -- HAND-ADDED: scopes the hardware-renderer hooks in model2_v.cpp to this
    -- driver project, so no other driver source sees them.
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

        -- HAND-ADDED: M2VK_SOUND_THREAD — the sound board on a worker thread. Compiled here (not in the
        -- OSD lib) because it builds a second running_machine and needs the full MAME/devices headers,
        -- which this project has. The OSD lib and model2.cpp see only the forward-declared header; the
        -- symbols resolve at the final link, as the render seams do. Inert unless the flag is set.
        MAME_DIR .. "src/osd/libretro_m2/m2vk_soundthread.h",
        MAME_DIR .. "src/osd/libretro_m2/m2vk_soundthread.cpp",

        -- HAND-ADDED: M2VK_LAZY_BAUD — the demand-gated i8251 baud clock. Same reason as above: it is
        -- a device_t and needs the full MAME/devices headers this project has, and both model2.cpp and
        -- segam1audio.cpp (compiled here) reference it. Inert when M2VK_LAZY_BAUD=0.
        MAME_DIR .. "src/osd/libretro_m2/m2vk_baud.h",
        MAME_DIR .. "src/osd/libretro_m2/m2vk_baud.cpp",
    }

    -- HAND-ADDED: the seam in model2_v.cpp calls into the sink in
    -- src/osd/libretro_m2, which the libretro OSD compiles and owns.  Any other
    -- OSD does not, so compile it here instead and the plain SUBTARGET=... binary
    -- still links (and still carries the polygon tap).  Only ever one of the two,
    -- or the symbols are defined twice.
    if _OPTIONS["osd"] ~= "libretro_m2" then
        files {
            MAME_DIR .. "src/osd/libretro_m2/m2vk_sink.h",
            MAME_DIR .. "src/osd/libretro_m2/m2vk_sink.cpp",
            MAME_DIR .. "src/osd/libretro_m2/m2vk_polytap.h",
            MAME_DIR .. "src/osd/libretro_m2/m2vk_frame.h",
            MAME_DIR .. "src/osd/libretro_m2/m2vk_frame.cpp",
        }
    end

    --=====================================================================
    --  Namco (Super) System 22  (from namcos22.lua)
    --=====================================================================
    project ("mame_namcos22")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-namcos22"))
    addprojectflags()

    -- HAND-ADDED: scopes the S1 seam hooks in namcos22_v.cpp to this driver project.
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
        MAME_DIR .. "src/mame/namco/namco_dsp.cpp",   -- compiled here, shared with mame_namcos21
        MAME_DIR .. "src/mame/namco/namco_dsp.h",
        MAME_DIR .. "src/mame/namco/namcomcu.cpp",
        MAME_DIR .. "src/mame/namco/namcomcu.h",
        MAME_DIR .. "src/mame/namco/namcos22.cpp",
        MAME_DIR .. "src/mame/namco/namcos22.h",
        MAME_DIR .. "src/mame/namco/namcos22_v.cpp",
    }

    -- The S22 seam/sink/GPU pass (s22_seam.cpp / renderer_vk/s22_geom.cpp) live in the shared libretro_m2
    -- OSD, not here; retro_entry.cpp and vk_present.cpp reference the s22:: symbols and resolve against
    -- the OSD in every build.  Inert unless a namcos22 driver arms capture.

    --=====================================================================
    --  Namco System 21  (from namcos21.lua)
    --=====================================================================
    project ("mame_namcos21")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-namcos21"))
    addprojectflags()

    -- HAND-ADDED: scopes the T1 seam hooks in namcos21_3d.cpp to this driver project.
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
        -- namco_dsp.cpp/.h intentionally omitted: compiled once in mame_namcos22 (see header).
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

    -- The T2 seam/GPU pass (s21_seam.cpp / renderer_vk/s21_geom.cpp) live in the shared libretro_m2 OSD,
    -- not here; retro_entry.cpp and vk_present.cpp reference the s21:: symbols and resolve against the OSD
    -- in every build.  Inert unless a namcos21 driver arms capture.

    --=====================================================================
    --  Namco System 23 / Super System 23  (new; no standalone namcos23.lua
    --  ever existed -- this project was hand-derived directly against
    --  modelizer, per devnotes/plan_system23.md phase 23-0)
    --=====================================================================
    project ("mame_namcos23")
    targetsubdir(_target .."_" .. _subtarget)
    kind (LIBTYPE)
    uuid (os.uuid("drv-mame-namcos23"))
    addprojectflags()

    -- HAND-ADDED: scopes the (future) seam hooks in namcos23.cpp to this
    -- driver project (no hooks yet at 23-0; the define is in place for 23-1).
    defines {
        "S23VK",
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
        MAME_DIR .. "src/mame/namco/md8412b_s23.cpp",
        MAME_DIR .. "src/mame/namco/md8412b_s23.h",
        MAME_DIR .. "src/mame/namco/namco_settings.cpp",
        MAME_DIR .. "src/mame/namco/namco_settings.h",
        MAME_DIR .. "src/mame/namco/namcos23.cpp",
        MAME_DIR .. "src/mame/namco/vpx3220a.cpp",
        MAME_DIR .. "src/mame/namco/vpx3220a.h",
    }

    -- A future S23 seam/GPU pass would live in the shared libretro_m2 OSD like the
    -- other three, not here; inert until 23-1 taps render_flush/the four producers.
end

function linkProjects_mame_modelizer(_target, _subtarget)
    links {
        "mame_model1",
        "mame_model2",
        "mame_namcos22",
        "mame_namcos21",
        "mame_namcos23",
    }
end
