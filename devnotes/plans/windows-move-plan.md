# Moving development to Windows

Written 2026-09-03, on the Windows box, from a survey of what is actually installed here. The Mac
is not the reference machine any more: the target is a Quest 3 headset and the shipping consumer is
the **Age of Joy** Unity project, both of which already live on this machine. This plan closes the
gap between "the Android core builds here" (true today) and "everything the Mac did happens here".

Read with [android.md](../reference/android.md) (Mac-written; §2's brew/NDK notes do not apply) and
[roms.md](../reference/roms.md) (Mac paths throughout).

## Status — 2026-09-03

**W0, W1, W2 and W3 are DONE.** The host build produces `modelizer_libretro.dll`, `retrohost.exe` is
ported, `ab.sh` + `state.sh` both PASS with the vf2 A/B metrics reproducing the Mac's baseline row to
the digit, and RetroArch (`C:\retroarch-win64`, portable) plays the core behind
`devnotes\shortcuts\Game Launcher.bat`. What was installed, what had to change and what was measured
is in **[windows.md](../reference/windows.md)** — that file, not this one, is the standing reference for working
here. Open: W4 (`deploy-aoj.sh` written, not yet exercised on a real Unity build), W5 (CLAUDE.md
rewrite), W6 (regenerate the baselines here).

---

## 1. What already works on Windows

Surveyed, not assumed:

| Piece | State |
|---|---|
| Repo | `E:\m2-vk`, clean, HEAD `be0ac5b26aa`, branch `main`, `origin` intact. |
| Host toolchain | MSYS2 at `C:\msys64` — mingw64 gcc/g++/python, `usr/bin/make`, `usr/bin/bash`. |
| NDK | r27d (clang 18) at `C:\NVPACK\android-ndk-r27d`. NVPACK's own r14b is unusable. |
| adb | `C:\NVPACK\android-sdk-windows\platform-tools`, on PATH. |
| **Android core build** | ✅ Works. `build-android.sh` carries a `MINGW*` branch, `.exe` suffixes and a Vulkan-header copy instead of a symlink. Produces `libmodelizer_libretro_android.so` (103 MB unstripped) in the repo root. |
| **Quest deploy (RetroArch arm)** | ✅ Works. `deploy-android.sh` has the Windows host-tag branch; the Quest needs `M2VK_ANDROID_ROMDIR=/sdcard/Download` to get past the SD-card gate. |
| **ROM library** | ✅ Complete, but **not** in `devnotes/roms`. It is at `E:\AgeOfJoy-2022.1_curif\claudedocs\ModelizerROms\{Model2,model1,system22,system23}` — including the two loose-file directories `manxttc/` and `overrev/` that carry BIOSes their zips do not. |
| **AOJ integration** | ✅ Working, manual. `E:\AgeOfJoy-2022.1_curif\Assets\Plugins\Android64\libmodelizer_libretro_android.so` is a hand-copy of the repo build, **unstripped**, and per-cabinet core options are pushed from each cabinet's `description.yaml` `environment:` block via `LibretroModelizerCore.cs` → `LibretroHWBridge.SetOption`. |

## 2. What is missing

Everything in this list is a Mac-only capability today.

1. **No Windows host build of the core.** `modelizer_libretro.dll` has never been produced. The
   `mingw* or vs*` and `BASE_TARGETOS=win32` branches in
   [scripts/src/osd/libretro_m2.lua](../../scripts/src/osd/libretro_m2.lua) exist but have never been
   compiled — they are untested code, not a working path.
2. **No `retrohost`.** This is the serious loss: every non-visual verification instrument in the
   project runs on it — `ab.sh` (A/B vs the software rasteriser), `res.sh`, `perf.sh`, `state.sh`
   (savestates 8/8), the video digest, the audio digest+RMS, `M2VK_HOST_OPT_AT` liveness, the
   `M2VK_HOST_DESCRIPTORS` input read-out. `retrohost.c` is POSIX/macOS C: `<dlfcn.h>`,
   `<mach/mach.h>`, and a MoltenVK `dlopen` candidate list.
3. **No RetroArch on Windows.** No install anywhere on this box — so there is no "just play it"
   loop, and no `m2-vk.opt` to read before interpreting a hand-check.
4. **`devnotes/roms` is empty.** The one-ROM-directory rule (CLAUDE.md gotcha 2) does not hold here.
5. **No Vulkan headers, no numpy, no glslc.** `/mingw64/include/vulkan` absent; `import numpy` fails
   (`ppmdiff.py` needs it); `glslc` absent (only matters when a shader changes).
6. **CLAUDE.md and every harness script are Mac-shaped**: `.dylib` defaults, `/opt/homebrew`,
   `~/Desktop/Model 2.app`, the installed-core symlink check, `caffeinate`, `sysctl -n hw.ncpu`, and
   the `RPFlip2` SD-card assumption (an Odin handheld's card, not the Quest's).

## 3. Phases

Ordered so each phase is usable on its own. W1 and W2 are the real work; everything else is chores.

### W0 — Host prerequisites and the ROM directory ✅ DONE

```sh
pacman -S mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-python-numpy mingw-w64-x86_64-shaderc
```
(shaderc only if a shader will ever be recompiled here; it is not needed to build the core.)

**ROM directory — settled.** The user dropped the complete set into `E:/m2-vk/roms` (flat, 80
entries, including the loose-file dirs `manxttc/` and `overrev/`), and `devnotes/roms` is now a
directory junction to it (`mklink /J`), so every script and doc path that says `devnotes/roms` is
correct with nothing to edit. The options that were weighed:

- **(a) Recommended — junction + flatten.** Make `E:\ModelizerROms` the one directory, move the four
  family subdirectories' contents up into it (no name collisions across the four), and
  `mklink /J E:\m2-vk\devnotes\roms E:\ModelizerROms`. Every script then works unmodified, and the
  Unity project's copy stops being the master.
- **(b) Junction only, keep the four subdirs.** `devnotes/roms` → `ModelizerROms`, and pass a
  four-entry rompath everywhere. Cheaper now, and it breaks `push_set()` in `deploy-android.sh`,
  `ab.sh`'s content path and the "one answer to where is it" rule. Not recommended.

Either way the two loose-file dirs (`manxttc\`, `overrev\`) must land beside the zips, not inside a
subfolder — a copy that drops them is the `vcop` trap: the set looks present and fails at load.

**Pin the shell.** All building happens in MSYS2's MINGW64 bash, never Git-for-Windows bash:
```
MSYSTEM=MINGW64 C:\msys64\usr\bin\bash -lc 'cd /e/m2-vk && ...'
```

### W1 — Windows host build → `modelizer_libretro.dll` ✅ DONE

The fast iteration loop. Without it every code change has to go through a 100 MB cross-build and a
USB cable before it can be looked at.

1. Point the build at the Vulkan headers. `libretro_m2.lua` defaults non-macOS to `/usr/include`,
   which under MSYS2 is `C:\msys64\usr\include` — the wrong prefix. Either export
   `M2VK_VULKAN_INCLUDEDIR=/mingw64/include` or (better, one line) add a `windows` branch to that
   default block so the build works with no environment.
2. Build:
   ```sh
   make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10
   ```
3. Expected breakage, from reading the win32 branches rather than from a run — treat as a checklist,
   not a prediction:
   - `ocore_libretro_m2`'s win32 branch compiles `osdlib_win32.cpp`, which includes `winutf8.h`;
     `winutf8.cpp` is **not** in that project's `files{}` (only `winutil.cpp` is) → probable link
     error, one line to add.
   - `winsocket.cpp` / `winptty.cpp` will want `ws2_32` (and `osdlib_win32.cpp` `user32`/`psapi`) on
     the link line; the existing `links { m, pthread }` block correctly excludes Windows but adds
     nothing in its place.
   - Confirm the DLL actually exports `retro_*` (mingw exports everything from a SharedLib by
     default, so this should be free — verify with `nm`/`objdump`, the same check the Android build
     already prints).
4. **Acceptance:** the DLL loads in RetroArch-Windows and boots `vf2` to a rendered frame. Not
   "it compiled".

*Fallback if mingw fights back:* build the core and harness for Linux under WSL2 instead. Rejected as
the first choice — the `--vk` arms would run on WSL's Vulkan passthrough, which is a different
renderer story from the one being shipped, and the Windows DLL is wanted anyway for the play loop.

### W2 — `retrohost` on Windows (the harness) ✅ DONE

Four concrete edits to `devnotes/retrohost.c`, all local:

| Mac dependency | Windows replacement |
|---|---|
| `<dlfcn.h>` `dlopen`/`dlsym`/`dlerror` | ~20-line shim over `LoadLibraryA`/`GetProcAddress`, or `pacman -S mingw-w64-x86_64-dlfcn`. |
| `<mach/mach.h>` RSS (`M2VK_HOST_RSS`) | `GetProcessMemoryInfo` (psapi). |
| MoltenVK `dlopen` candidate list | `vulkan-1.dll` (the ICD loader ships with the GPU driver); keep `M2VK_HOST_MOLTENVK` as the override, rename or alias it. |
| `pthread`, `clock_gettime` | Work as-is under mingw-w64 (winpthreads). |

`build-retrohost.sh` needs a host branch: `.exe` suffix, `M2VK_VULKAN_INCLUDEDIR` default
`/mingw64/include`, `-lpsapi`.

Then `ab.sh` / `res.sh` / `perf.sh` / `state.sh` need their `CORE=` defaults switched from
`.dylib` to the host's extension (one variable each; they are already `#!/usr/bin/env bash`, which
was the hard part and is already done). `sysctl -n hw.ncpu` already falls through to `nproc`.

**Acceptance:** `ab.sh vf2 2500` runs four arms and reports exact PASS; `state.sh` reports 8/8.

⚠️ **The baselines will have to be regenerated here, and that is expected.**
`ab-baselines.md` already predates the lazy-baud default flip and the billboard-park default, so its
digest tables do not match a default run on any machine. Regenerate with `ab-table.py` / `res-table.py`
on Windows and say in the file that Windows is now the reference host. Never retype a digest.

### W3 — The play loop: RetroArch on Windows

1. Install RetroArch (Windows x64), set `video_driver = vulkan` — the core declares
   `RETRO_HW_CONTEXT_VULKAN` and will not load otherwise.
2. Replace the Mac's symlink-into-the-cores-directory with an explicit `deploy-desktop.sh` that
   copies the freshly built DLL and prints its timestamp. The Mac's symlink silently reverted to a
   copy at least four times and cost whole sessions; a copy step that announces itself has the same
   convenience and none of the failure mode.
3. Record the Windows core-options path (`…\RetroArch\config\m2-vk\m2-vk.opt`) in CLAUDE.md. The
   worklog's two most expensive misdiagnoses — the sound-thread serial stall and the vcop
   "lag" — were both *the live `.opt` disagreeing with what the notes said*. `cat` it before
   interpreting any hand-check.
4. A `Modelizer.cmd` shortcut standing in for `~/Desktop/Model 2.app`: launches RetroArch with no
   flags and with every `M2VK_*` cleared, so a play session is never accidentally pinned by a
   harness variable.

### W4 — Quest + Age of Joy, scripted

The Quest has two arms and they are not interchangeable — say which one a result came from:

- **RetroArch-on-Quest** — `build-android.sh` → `deploy-android.sh` → the manual
  *Load Core → Install or Restore a Core* → `adb logcat -s m2stall:V m2prof:V`. Has a core-options
  menu and logcat. This is the debugging arm.
- **Age of Joy APK** — the shipping arm. Core options come from each cabinet's `description.yaml`
  `environment:` block, not from a menu, so an option that was only ever set in RetroArch's menu is
  *not set* in AOJ.

Work items:
1. Teach `deploy-android.sh` a Quest mode so `M2VK_ANDROID_ROMDIR=/sdcard/Download` stops being a
   thing to remember (the `RPFlip2` fsLabel gate is an Odin card and fails on the Quest *before* the
   core is pushed, with no ROM args given).
2. New `devnotes/deploy-aoj.sh`: `llvm-strip` the core and copy it to
   `E:\AgeOfJoy-2022.1_curif\Assets\Plugins\Android64\`. Today's hand-copy ships 103 MB of debug
   info into a 1.2 GB APK. Leave the `.meta` alone — Unity owns it.
3. Fold the existing `_pushtoheadset.bat` / `_logcat.bat` habits into one documented sequence, so
   "core changed → APK on headset" is a list, not a memory.

### W5 — Documentation

1. **Rewrite CLAUDE.md for this box.** Build+run, harness, "just playing it" and the deploy sections
   are all Mac-specific. Drop the `.app`, the symlink-reversion warning (replaced by W3's copy step),
   `caffeinate`, and the Homebrew prefixes. Keep the *lessons* that are host-independent — the
   run-invocation gotchas, "screenshot before believing poly statistics", the banned automated
   button-press testing, the option-drift traps.
2. New `devnotes/reference/windows.md`: what is installed on this box and where (the table in §1), the two
   shell invocations, and the ROM-directory decision as built.
3. Update the `windows-android-build` memory to point at it, and add a memory for the desktop build
   once W1 lands.

### W6 — Optional tail

- Regenerate `ab-baselines.md` + `res-baselines.md` on Windows and re-anchor them as the reference.
- Re-run the compat sweep so `model2_quest_checklist.md`'s hand-written status column and the
  measured table agree.
- Decide whether the Mac stays as a second host at all. If not, note in `legalstuff.md` §9 that the
  release binary is now built on Windows.

## 4. Sequencing and the one real decision

W0 → W1 → W3 gets a full playable desktop loop and is worth doing first; W2 is a day's work on its
own and can follow. W4/W5 are chores that should land the same week so the notes stop lying.

**The decision: is W2 (retrohost) worth porting, or does Windows development run on hand-checks and
the Quest?** Recommendation: **port it.** The worklog's own record is that attract-mode green runs
hid a dead UART transmitter through 14 A/B fixtures and 8 savestate fixtures, and that the fix was
found by a scripted byte-stream diff — an instrument that only exists inside `retrohost`. Dropping
the harness does not slow verification down, it removes it. The port is four mechanical edits, not a
redesign.
