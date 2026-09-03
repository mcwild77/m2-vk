# The Windows box — host setup, invocations, and what had to change

Written 2026-09-03, when development moved off the Mac. This is the "how to build and measure here"
file; the phased move itself is [windows-move-plan.md](windows-move-plan.md), and the Android/Quest
specifics are in [android.md](android.md) (Mac-written — its §2 brew/NDK notes do not apply here).

**Status: the whole desktop loop works** — the host build, `retrohost`, the A/B and savestate
harnesses, and RetroArch playing the core with a game launcher in front of it. The shippable path
(Quest, Age of Joy) already worked before this.

---

## 1. What is installed, and where

| Piece | Where |
|---|---|
| Repo | `E:\m2-vk` (`/e/m2-vk` in the shell) |
| Host toolchain | MSYS2 at `C:\msys64` — mingw64 gcc 16.2.0, `usr/bin/make` 4.4.1, python 3.14.7 |
| NDK | r27d (clang 18) at `C:\NVPACK\android-ndk-r27d`. NVPACK's own r14b is too old to use. |
| adb | `C:\NVPACK\android-sdk-windows\platform-tools`, on PATH |
| ROMs | `E:\m2-vk\roms` — the complete set, flat, including the loose-file dirs `manxttc\` and `overrev\`. `devnotes\roms` is a **directory junction** to it, so every script and doc path that says `devnotes/roms` is correct. |
| GPU | NVIDIA GeForce RTX 3070, Vulkan 1.4.329 — the `--vk` arm runs on the real ICD loader (`vulkan-1.dll`), not on a translation layer |
| RetroArch | `C:\retroarch-win64` — **portable**: its config is `C:\retroarch-win64\retroarch.cfg` and the core options live at `C:\retroarch-win64\config\m2-vk\m2-vk.opt` (the `m2-vk` directory name comes from the core's `library_name`) |
| Age of Joy | `E:\AgeOfJoy-2022.1_curif` (Unity), core at `Assets\Plugins\Android64\libmodelizer_libretro_android.so` |

**pacman packages this needs** (all installed 2026-09-03):

```sh
pacman -S mingw-w64-x86_64-vulkan-headers   # the core and retrohost build against these
pacman -S mingw-w64-x86_64-python-numpy     # ppmdiff.py
pacman -S diffutils                         # cmp — ab.sh's background-reference guard
pacman -S git                               # the report header's commit id
pacman -S mingw-w64-x86_64-shaderc          # glslc, ONLY if a shader is ever recompiled here
```

## 2. The two invocation gotchas, each of which cost a build

**Every build and harness command runs through MSYS2's MINGW64 bash**, not Git-for-Windows bash
(which has no compiler, no pacman, no cygpath):

```sh
MSYSTEM=MINGW64 C:\msys64\usr\bin\bash -lc 'cd /e/m2-vk && export OS=Windows_NT && <command>'
```

1. **`cd /e/m2-vk` is not optional.** `bash -l` sources `/etc/profile`, which changes to `$HOME`.
   Without the `cd` the build fails with `No targets specified and no makefile found`, which reads
   like a broken checkout.
2. 🚨 **`OS=Windows_NT` is not optional.** [makefile:144](../makefile#L144) selects the entire
   Windows branch on that variable. Windows sets it, an interactive MSYS2 terminal inherits it — and
   a bash started from Git-for-Windows bash does **not**. The makefile then falls through to `uname`
   detection and stops with `Unable to detect OS from uname -a: MINGW64_NT-10.0-19045`. Exporting it
   is restoring the variable Windows itself sets, not a workaround.

## 3. Build and run

```sh
# the core -> modelizer_libretro.dll (repo root)
make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j24

# the harness
./devnotes/build-retrohost.sh                    # -> devnotes/retrohost.exe

# a run: [--vk] <core> <rom> <frames> <out.ppm> [input script]
./devnotes/retrohost --vk ./modelizer_libretro.dll devnotes/roms/vf2.zip 1200 /tmp/vk.ppm

# the harness proper
./devnotes/ab.sh vf2 2500 /tmp/ab
VK=1 ./devnotes/state.sh vf2 3000 1500 /tmp/state

# the Quest
./devnotes/build-android.sh
M2VK_ANDROID_ROMDIR=/sdcard/Download ./devnotes/deploy-android.sh   # RetroArch arm
./devnotes/deploy-aoj.sh                                            # Age of Joy arm (strips first)
```

`/tmp` in the shell is `C:\msys64\tmp`; the harness writes its PPMs and reports there.

### Playing it — `devnotes\shortcuts\Game Launcher.bat`

**Playing and measuring must not share a command line.** Every command above pins options and passes
switches, which is wrong for a session where the options menu is meant to be in charge. The launcher
is the play command: double-click it, pick a game by number, quit RetroArch, and the list is still
there for the next one.

- Lists the 68 installed sets grouped by family (Model 1 / Model 2 / System 21 / System 22 /
  System 23), catalogued from the compiled driver table and filtered at startup to the zips actually
  present — a family with nothing installed prints no header. The six device/BIOS sets and
  `driveyes` (which lives in the uncompiled `namcos21_de.cpp`) are deliberately not listed.
- **Runs the core straight out of the repo** (`-L <repo>\modelizer_libretro.dll`) rather than a copy
  installed into RetroArch's cores directory, and prints the core's build timestamp in the header.
  That is the Mac's reverting-symlink trap designed out rather than warned about: there is no second
  copy to go stale.
- Clears every `M2VK_*` / `M2OPT_*` variable from the environment first, so a harness switch left in
  a shell cannot silently pin a play session.
- Forces exactly one setting, through `--appendconfig`: `video_driver = "vulkan"`. The core declares
  `RETRO_HW_CONTEXT_VULKAN` and will not load under any other driver. Everything else is left to the
  menu.
- `M2VK_RETROARCH=<path to retroarch.exe>` overrides the frontend location.

⚠️ The Mac's advice to **read `m2-vk.opt` before interpreting a hand-check** applies here unchanged,
and the launcher does not touch that file — it is `C:\retroarch-win64\config\m2-vk\m2-vk.opt`.

## 4. What had to change to make it build

Four edits, all small, none of them upstream MAME:

1. **[scripts/src/osd/libretro_m2.lua](../scripts/src/osd/libretro_m2.lua)** — the Vulkan-header
   default was a single path (`/usr/include` off macOS), which under MSYS2 points at
   `C:\msys64\usr\include` and holds nothing. It is now a per-host candidate list, and Windows names
   the **drive-letter** form: genie is a native binary and does not resolve the shell's
   `/mingw64/include`. `VULKAN_SDK` is honoured too, for a box with LunarG and no MSYS2.
2. **[src/osd/libretro_m2/m2vk_soundthread.cpp](../src/osd/libretro_m2/m2vk_soundthread.cpp)** —
   the one compile error, and it is a toolchain-era issue rather than a Windows one. `osdepend.h`
   forward-declares `ui::menu_item` and declares `virtual std::vector<ui::menu_item>
   get_slider_list()`; **libstdc++ 16** instantiates that vector's defaulted constructor (hence its
   destructor) from the declaration alone, which an incomplete element type cannot satisfy. Fixed by
   including `ui/menuitem.h` first, exactly as upstream's own `osdobj_common.cpp` and `osdwindow.h`
   already do. It is the only file of ours that includes `osdepend.h` directly.
3. **[devnotes/retrohost.c](retrohost.c)** — ported. `<dlfcn.h>` → a `dl_open`/`dl_sym`/`dl_error`
   shim (`LoadLibraryA`/`GetProcAddress` on Windows, dlfcn everywhere else); `<mach/mach.h>` RSS →
   `GetProcessMemoryInfo` on Windows and `/proc/self/statm` on Linux; the MoltenVK candidate list →
   per-platform, `vulkan-1.dll` on Windows, with `M2VK_HOST_VULKAN` as the override
   (`M2VK_HOST_MOLTENVK` still accepted); `setenv` → `env_default()`. pthreads and
   `clock_gettime(CLOCK_MONOTONIC)` needed nothing — mingw-w64 supplies both.
4. **[devnotes/hostenv.sh](hostenv.sh)** (new), sourced by `ab.sh` / `res.sh` / `perf.sh` /
   `state.sh` — carries `CORE_EXT` (`.dylib`/`.dll`/`.so`), `EXE`, and `hostpath()`.

### 🚨 `hostpath()` — the silent failure worth knowing about

MSYS2 rewrites POSIX paths in **command-line arguments** to native form, and **not** in
**environment variables**. Every path the harness hands the core through an env var —
`M2_SAVE_DIR`, `M2VK_HOST_SAVE_AT`, `M2VK_HOST_LOAD_AT`, `M2_SYSTEM_DIR` — therefore arrived as the
literal string `/tmp/...`, which native `fopen` resolves against the current drive's root
(`E:\tmp\...`) and fails to open.

What that looked like: `state.sh` reported `FAIL: no state file was written`, and `ab.sh` **passed**
while silently writing no NVRAM at all — the per-run `M2_SAVE_DIR` isolation the script exists to
provide was not happening, and nothing said so. `hostpath()` runs `cygpath -m` on Windows and is a
no-op elsewhere; arguments still need nothing.

Also host-aware now: `perf.sh`'s "is anything else running" check (macOS `ps -Ac -o comm=` sees
nothing on Windows; MSYS2's own `ps` needs `-W` to see native processes) and its `uptime` load line.

## 5. Verification — what was actually measured here

All on `modelizer_libretro.dll`, RTX 3070, 2026-09-03.

| Check | Result |
|---|---|
| Core boots (software path) | vf2 1200 frames, 25 exported `retro_*` entry points, 330 % speed |
| Core boots (Vulkan path) | vf2 1200 frames, 122 % speed, `NVIDIA GeForce RTX 3070 api 1.4.329` |
| `ab.sh vf2 2500` | **PASS.** covered 107568 / 107569, agreement **1.0000**, A-only 1, B-only 2, interior disagreements **0**, white **0**, same colour 95.554 %, ssim covered **0.996983** |
| `state.sh vf2` (Vulkan) | **PASS** on all four controls: D == E, N != D, **C == D**, A == R |
| RetroArch plays it | vf2 booted through `retroarch.exe -L modelizer_libretro.dll`, 1800 frames in 31 s (= 57.52 Hz, full speed), `Using HW render, vulkan driver forced`, `Using GPU: NVIDIA GeForce RTX 3070`, screenshot shows the attract fight rendering correctly at **57.546 fps** |

**The `ab.sh` metrics reproduce [ab-baselines.md](ab-baselines.md)'s Mac row for vf2 to the digit**
(107568 · 1.0000 · 1/2 · 0 · 95.56 % · 0.9970), and the background reference digest
`c3aaa56633c1c4f7` is bit-identical to the Mac's. That is the evidence that the Windows build is the
same renderer, not merely a build that runs.

⚠️ **The 3D digests do NOT match the recorded table, and that is expected, not a regression.**
Windows reads `5035b4ef3a1e1084` (software) / `e8051a92c7b6bc33` (Vulkan) against the table's
`9c20f1fac9d9fe92` / `de94f44a06151f71`. `ab-baselines.md` predates the lazy-baud and
billboard-park default flips, which move device timing on every host — the worklog says so at
2026-09-01, and the metrics reproducing while the digests move is exactly that signature. The
baselines want regenerating **here** ([windows-move-plan.md](windows-move-plan.md) W6); until then
read the metric columns, not the digest tables.

## 6. Still open

- **The baselines have not been regenerated here** — see the warning above.
- The connected **8BitDo Ultimate 2C Wired Controller has no RetroArch autoconfig profile**
  (`Controller ... not configured, using fallback` in the log), so its buttons come from the fallback
  binding rather than a per-pad one. Not a core problem; worth a pass through RetroArch's input
  binding if the mapping feels wrong.
- CLAUDE.md still describes the Mac (`.dylib`, `~/Desktop/Model 2.app`, the core symlink,
  `caffeinate`, Homebrew prefixes). W5.
