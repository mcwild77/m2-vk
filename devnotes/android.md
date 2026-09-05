# The Android core — cross-building for on-device testing

✅ **Status, 2026-08-07: IT RUNS ON HARDWARE.** `vf2` and `daytona` both boot, render and play on an
**AYN Odin 2 Portal — Snapdragon 8 Gen 2, `Adreno (TM) 740`, Android 13** — under RetroArch 1.22.2
with the Vulkan driver. Screenshots: `devnotes/screenshots/2026-08-07-android-odin-{vf2,daytona}.png`.
First light took no code change beyond the build glue in §4.

🚨 **The Adreno 740 is the Quest 3's GPU.** The XR2 Gen 2 is the same silicon family, so this device
is a far better proxy for the deployment target than the plan assumed was available — the phone was
picked for convenience and turns out to answer the Quest 3 questions. **§5a is the device read-out,
and it contradicts `vulkan-target.md` in several places** that were written from MoltenVK.

✅ **The Quest 3 port is no longer shelved** — the 2026-07-27 decision was overtaken by the Aug–Sep 2026
Quest work (profiling on Adreno silicon, `model2_lazy_baud` + `model2_sound_thread` landed, 57.5 fps
locked outside the heaviest scenes; worklog 2026-09-01, `retroarch-quest-perf.md`). The §4 performance
items now have an Adreno to run on, and several have.

⚠️ **Nothing about speed or accuracy has been MEASURED yet.** Two games reaching a playable screen is
first light, not a benchmark, and §6 still stands in full.

---

## 1. The headline finding: the core was already portable

This was expected to be the expensive part and it was free. Before writing any build glue, the tree
was checked rather than assumed:

- **Zero Apple-specific code in `src/osd/libretro_m2/`.** No `__APPLE__`, no CoreFoundation, no
  `mach_*`, no Objective-C. MoltenVK appears in comments only.
- **The core links no Vulkan library and never did** (`renderer_vk/vk_funcs.h`). Every entry point is
  resolved at run time from the frontend's `vkGetInstanceProcAddr`, so Android's `libvulkan.so` needs
  no build-time handling whatsoever. This is the single biggest reason the port is cheap, and it was
  a P2 decision taken for unrelated reasons.
- **The shaders are committed SPIR-V** compiled at `--target-env=vulkan1.0`. Nothing to recompile,
  no shaderc on the cross-build host.
- `--osd` is free-form in `scripts/genie.lua:139` and `android` is an allowed `targetos`, so
  `--osd=libretro_m2 --targetos=android` is a legal genie invocation with no patching.

**Every line of work below is build-system glue.** Not one line of renderer, OSD or emulation code
was changed to make an arm64 core exist.

---

## 2. Prerequisites (none of which the repo carries)

| | |
| --- | --- |
| NDK | **r27d**, at `~/Library/Android/sdk/ndk/android-ndk-r27d`. Override with `ANDROID_NDK_HOME`. |
| adb | `brew install --cask android-platform-tools` |

🚨 **The NDK was installed from Google's zip, not from `brew`.** The `android-ndk` cask is broken
(`Cask 'android-ndk' definition is invalid: undefined method 'command_wrapper'`), and the
`android-commandlinetools` route to `sdkmanager` needs a JDK this machine does not have. The zip
(`dl.google.com/android/repository/android-ndk-r27d-darwin.zip`, 839 MB) is self-contained and needs
no Java. `android-commandlinetools` **is** installed, but only `adb`/`fastboot` out of it are used.

⚠️ **The NDK's host directory is called `darwin-x86_64` on Apple Silicon too** — the binaries inside
are universal, `clang` is arm64-native. `scripts/toolchain.lua:63` hard-codes that name, so nothing
needs adjusting; it just looks wrong.

---

## 3. The two commands

```sh
./devnotes/build-android.sh              # incremental -> ./model2_libretro_android.so
REGENIE=1 ./devnotes/build-android.sh    # after any scripts/ change, same rule as the host build

./devnotes/deploy-android.sh vf2         # strip, adb push core + devnotes/roms/vf2.zip
```

**`build-android.sh` deliberately does not go through the makefile's `android-arm64` target.** That
rule hard-codes `--osd=sdl` and demands `SDL_INSTALL_ROOT` (`makefile:1195`), neither of which
applies here; calling genie directly is shorter and leaves the upstream makefile alone. The genie
`PARAMS` list is **harvested from the makefile at run time** rather than retyped, so an upstream
change to the flag set reaches this build the way it reaches every other one.

Object files go to `build/android/obj/arm64`; the host build's are in `build/osx_clang/obj`. **The
two builds do not share an object directory** — which is worth stating explicitly, because
`OSD=sdl3` and `OSD=libretro_m2` *do*, and that is the latent breakage CLAUDE.md records.

---

## 4. What had to change, and why — four edits, three files, no source change

### 4.1 `src/osd/modules/lib/osdlib_unix.cpp` — the SDL include (guarded)

`osdlib_unix.cpp` includes `<SDL2/SDL.h>` **unconditionally** (line 15), and needs it for the
clipboard and nothing else. The macOS build has never hit this because it uses `osdlib_macosx.cpp`;
`LIBRETRO_M2_TARGETOS` maps everything that is not Windows or macOS to `unix`, so Android is the
first target of this OSD to reach the file.

Upstream already stubs both clipboard functions out under `SDLMAME_ANDROID`. The fix is to let
`OSD_LIBRETRO_M2` join that condition, and to put the include behind the same test:

```c
#if !defined(SDLMAME_ANDROID) && !defined(OSD_LIBRETRO_M2)
  ... the SDL include ...
#endif
```

🚨 **This is the SECOND time the fork has touched upstream outside `src/mame/sega/` — the third
file, since the first episode was `scsp.cpp` *and* `scsp.h` — and unlike that one this edit IS
guarded** — it compiles
to exactly the previous bytes for every other OSD. `OSD_LIBRETRO_M2` is defined in
`libretro_m2_cfg.lua` and `osdlib_unix.cpp` is in the `ocore_libretro_m2` project, so the define
reaches it.

✅ **A side effect worth knowing: this is also what a Linux build of the core was blocked on.** Not
attempted, but the blocker is gone.

### 4.2 `scripts/src/main.lua` — two android branches that build the SDL app

`mainProject()` has two blocks that assume `targetos=android` means "the SDL `android-project` app":

- **line ~27**, a `configuration { "android*" }` that sets `targetprefix "lib"` / `targetname "main"`
  and links **`EGL`, `GLESv1_CM`, `GLESv2`, `SDL2`**;
- **line ~91**, which adds `src/osd/sdl/android_main.cpp`, points `targetdir` at
  `android-project/app/src/main/libs/…`, and **copies `libSDL2.so`** next to the output.

Both are now skipped when `osd == "libretro_m2"`.

🚨 **The first one had to be skipped rather than overridden, and that is the general lesson:
`links{}` accumulate and genie has no way to take one back.** `maintargetosdoptions()` runs later and
did successfully override `targetprefix`/`targetname`/`targetextension` — but `-lSDL2` stayed, and
the first successful compile of all 1095 objects ended in `ld.lld: error: unable to find library
-lSDL2`. A whole-tree build to find a link flag.

⚠️ **That block is also where a genie android build normally gets `-shared` and its soname**, so both
are reissued in `libretro_m2.lua`. Skipping the block without noticing that would have produced an
executable rather than a shared object.

### 4.3 `scripts/src/osd/libretro_m2.lua` — the android configuration

```lua
configuration { "android-*" }
    targetextension ".so"
    targetprefix ""
    targetname "model2_libretro_android"
    linkoptions { "-shared", "-Wl,-soname,model2_libretro_android.so",
                  "-static-libstdc++", "-Wl,--exclude-libs,ALL" }
```

and Android is excluded from the `links { "m", "pthread" }` block, because **bionic has no
libpthread** — it lives in libc, there is no `libpthread.so` or `.a` in the NDK sysroot at all, and
`-lpthread` is a hard link error.

Four things in that block are load-bearing:

- **`_android` in the name is ABI, not decoration.** RetroArch on Android looks for
  `<name>_libretro_android.so` and strips the suffix again to find `<name>_libretro.info`.
- **The soname.** `mainProject()`'s is the generic `libmain.so`, and **Android's loader dedupes by
  soname** — a core calling itself `libmain.so` inside a frontend's process is a collision waiting
  to happen.
- 🚨 **`-static-libstdc++` is a runtime-failure fix made at build time.** The NDK's clang links
  `libc++_shared.so` by default, and the first successful link had it as a `NEEDED` entry. A frontend
  whose APK does not ship that library cannot `dlopen` the core — a failure that would have shown up
  on the phone, late, with nothing in the build to suggest it. Measured both ways: `NEEDED` went from
  six entries to five, and `libc++_shared` is gone.
- **`-Wl,--exclude-libs,ALL`** keeps every symbol that came out of a static archive out of the
  dynamic symbol table, so the core's now-private libc++ cannot bind against the frontend's. The
  `retro_*` entry points survive because `retro_entry.cpp` is a **direct object** in this link rather
  than an archive member — which is true for the reason the lua's own comment gives, and would
  otherwise have been a silent disaster.

### 4.4 `--NOASM=1`

Same as the upstream android rules pass. The x86 DRC back end is not buildable here and every Model 2
CPU (i960, MB86233/5, 68000, Z80) has a C interpreter. ⚠️ **Untested consequence: this may cost real
speed**, and speed is the entire question the port exists to answer. See §6.

---

## 5. What the artifact is

```
model2_libretro_android.so   ELF 64-bit LSB shared object, ARM aarch64, not stripped, 89.6 MB
  SONAME  model2_libretro_android.so
  NEEDED  libc.so libdl.so libm.so libandroid.so liblog.so
  exports 46 dynamic symbols, of which all 25 retro_* entry points
```

Unstripped on purpose — a first-generation port wants symbols when the phone produces a native
crash. `deploy-android.sh` pushes a stripped copy and leaves the debug info here for `ndk-stack`.

**The host build is untouched.** `make SUBTARGET=model2 OSD=libretro_m2 REGENIE=1` still links, and
`ab.sh vf2 2500` reproduces `ab-baselines.md` — see the worklog entry for the digests. That is the
only guard that matters here: every change above is either android-config-only or guarded by
`OSD_LIBRETRO_M2`, so a moved digest would mean one of them was not.

---

## 5a. The device, as the core's own probe reported it (Odin 2 Portal, 2026-08-07)

🚨 **`vulkan-target.md` is a MoltenVK document and several of its facts are Apple's, not Vulkan's.**
It is not wrong — it says what it measured — but do not carry its constraints onto Adreno:

| | Apple / MoltenVK (`vulkan-target.md`) | Adreno 740 (measured here) |
| --- | --- | --- |
| instance API | 1.1 ceiling (MoltenVK clamps) | **1.3.0**, device 1.3.128 |
| `D24_UNORM_S8_UINT` | **does not exist** | **exists**, with `filter-linear` |
| `geometryShader` | no | **yes** |
| `wideLines` | no | **yes** (1.0–127.5) |
| `multiViewport` | no | **yes** (16 viewports) |
| timestamps | — | **yes, 52.1 ns**, 48 timestamp bits |
| image ring | 3 | **4** (frontend's sync mask `0xf`) |
| max 2D image | 16384 | 16384 (same) |

Driver `0x802a4035` (0.676.53), vendor `0x5143`. Memory: one 7191 MiB device-local heap plus a
256 MiB protected heap; **types 4/5/6 are `device-local | host-visible`**, i.e. the shared-bus
picture performance.md §1 assumed, now confirmed rather than inferred. One graphics queue family
with 3 queues. 113 device extensions, including `VK_KHR_dynamic_rendering`,
`VK_KHR_synchronization2`, `VK_KHR_timeline_semaphore` and `VK_KHR_push_descriptor` — none of which
the core uses, and all of which were unreachable on the desktop target.

✅ **`performance.md`'s "GPU timestamps were deliberately not built … revisit on Quest 3" is now
actionable**: this device reports 48 timestamp bits at 52.1 ns, so the differential-switching
workaround can be replaced with real numbers whenever that work is picked up.

⚠️ **The renderer's own paths are exercised, not merely loaded.** `daytona` logs
`1707 polygons, 1 window run, 664 scissor draws` and then **grows all four geometry slots from 2048
to 4096 polygons** — so the per-slot resize path, which on desktop only ever ran under synthetic
pressure, runs for real on the first drive-in.

---

## 6. What is NOT known, in the order it is likely to bite

1. **Whether it runs at all.** Not one instruction of this has executed on an ARM device.
2. **Whether RetroArch Android's Vulkan driver drives our context negotiation.** Everything about
   `RETRO_HW_CONTEXT_VULKAN`, `set_image` and the `context_destroy`/`context_reset` lifecycle has
   only ever been exercised against MoltenVK and `retrohost --vk`.
3. **Speed, which is the actual question.** `perf.sh` says the emulation thread is the long pole at
   3.0–4.8 ms/frame on an M-series Mac with two interpreted i960s. Nothing predicts that figure on a
   phone. **`--NOASM=1` is a confound here** — if the numbers are bad, re-check with the arm64 DRC
   before concluding anything about the hardware.
4. **Endianness/alignment in the shader's packed lanes.** Both hosts are little-endian arm64, so this
   is probably fine, but the raster tail does a lot of hand-packed bit work.
5. **Adreno's tiler vs. a front-to-back stream with `discard`.** performance.md §3.1 predicts this is
   where the desktop-measured conclusions stop transferring.

🚨 **None of the harness travels.** `retrohost` does not cross-compile, so `ab.sh`, `res.sh`,
`state.sh` and every digest in `ab-baselines.md` are host-only. On-device verification is playing it
and looking at it. **Do not treat a phone screenshot as an accuracy measurement** — it is not
comparable to anything in `devnotes/`.

---

## 7. Installing and launching — no menu navigation required

**The whole loop is scriptable and there is no reason to drive the device by hand.** This was found
by trying it, and it is the single biggest workflow difference from the desktop side.

### 7.1 Where things go, and why not where you would guess

| | |
| --- | --- |
| core | `<core_assets_directory>` = `/sdcard/RetroArch/downloads` |
| ROMs | **the SD card**: `/storage/<uuid>/ROMS/model2`, card `fsLabel=RPFlip2` |

🚨 **RetroArch's cores directory (`/data/user/0/com.retroarch.aarch64/cores`) is NOT adb-writable**
— `Permission denied`, and the Odin is not rooted (`su` absent, `adb root` refused). Redirecting
`libretro_directory` at `/sdcard` would work and was the first plan; it was rejected because it
**hides every other core the device already has**, which is rude on a handheld somebody plays.

✅ **The supported route installs itself.** Push the `.so` to the downloads directory, then launch
with `LIBRETRO` naming that path: RetroArch **copies it into its own cores directory** and reports
`Core installation complete: model2_libretro_android.so`. ⚠️ **That copy ANRs the UI thread** — 57 MB
— and the ANR dialog is not a failure; the install completes. Re-run after every rebuild.

⚠️ **ROMs are on the SD card by decision, not convenience.** `ROMS/model2` is an existing **ES-DE**
system folder (it already contained a `systeminfo.txt`), so the sets drop into the tree the device
already uses. `deploy-android.sh` finds the card by **`fsLabel`**, never by mount point — the mount
point is the volume UUID (`/storage/F8B2-FD4C`) and changes with the card.

🚨 **Nothing goes in `<system dir>/model2`, the core's second rompath.** The first rompath entry is
the content's own directory (`retro_entry.cpp:669`), so device and BIOS sets — `segabill.zip` for
`vonj`, the `model1io2` BIOS merged into `vcop.zip` — work by sitting beside the games. One
directory, one answer to "where is it", exactly as `devnotes/roms` is arranged on the desktop.

### 7.2 Launching by intent

```sh
adb shell am start -n com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture \
  -e ROM      /storage/F8B2-FD4C/ROMS/model2/daytona.zip \
  -e LIBRETRO /data/user/0/com.retroarch.aarch64/cores/model2_libretro_android.so \
  -e CONFIGFILE /storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg \
  -e DATADIR  /data/user/0/com.retroarch.aarch64
```

⚠️ Point `LIBRETRO` at the **downloads** copy the first time (that is what triggers the install) and
at the **cores** copy thereafter.

### 7.3 Reading the result

- `video_driver` must be `vulkan` — the core declares `RETRO_HW_CONTEXT_VULKAN` and will not load on
  `gl`. It already was on this device.
- 🚨 **RetroArch Android logs almost nothing to logcat.** `adb logcat -s RetroArch:V` produced no
  core output at all and reads exactly like a core that never loaded. The log that matters is the
  **file**, and it is off by default: set `log_to_file="true"`, `log_verbosity="true"`,
  `libretro_log_level="0"` in `retroarch.cfg` (adb-writable, under `Android/data`), then read
  `/sdcard/RetroArch/logs/retroarch.log`. ⚠️ **Those three are currently ON on this device**;
  `devnotes/odin-retroarch.cfg.bak` is the config as found, and Settings → Logging reverts them.
- **Screenshots come from `adb exec-out screencap -p`**, which is the device framebuffer — so unlike
  a desktop RetroArch screenshot it has no P3 conversion in the way. It is still the *frontend's*
  scaled output, not the core's 496×384, so it is a look-at-it tool and **not** an accuracy
  measurement.
- A native crash symbolises with `adb logcat | $ANDROID_NDK_HOME/ndk-stack -sym .` from the repo
  root, which is what the unstripped `.so` is for.

### 7.4 Two loose ends on the device

- ⚠️ **There is no `.info` file for the core**, so it lists by filename and the content browser does
  not filter by extension. Harmless; `libretro_info_path` is inside the app's private dir and cannot
  be written without also redirecting it and losing every other core's metadata.
- ⚠️ **ES-DE will not use this core.** `ROMS/model2/systeminfo.txt`'s launch command names
  `mamearcade_libretro_android.so`. Launching a Model 2 game from ES-DE therefore runs MAME's arcade
  core, not ours — deliberately left alone, since it is the device owner's frontend config.
