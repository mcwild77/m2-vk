# RetroArch on the Quest 3 — the performance iteration loop

**Status, 2026-08-31: the plan of record for measuring CPU speed.** The per-frame optimisation work
is CPU-bound (see `performance.md` §2a/§6 and the device profile behind `m1audio-thread-plan.md`), and
the iteration cost of measuring it on-device was the whole friction: every change meant rebuilding the
`.so`, repackaging it into the deployment APK, sideloading, and then driving to a heavy scene by hand
in VR. This retires that inner loop. The perf leg runs the **same Android core** in **RetroArch on the
Quest 3 itself**, deployed by `deploy-android.sh` and launched by intent — a fraction of the turnaround,
and it can be driven from the same box that runs the correctness harness.

The full APK run does not go away: it stays as the once-per-milestone **absolute** acceptance gate (§7).
RetroArch is the fast **relative** dyno for every change in between.

---

## 1. Why RetroArch is a faithful stand-in — the load is entirely on the CPU

The one fact that makes this substitution sound: **the GPU is idle and the renderer is not on the
critical path.** The heavy-scene device profile has `gpu_busy` at 8–53 %, and the GPU *down-clocks
itself* to 492–545 MHz of an available 690 while the frame is late — it finishes early and coasts,
`thermal_pwrlevel = 0`. Nothing that a different frontend does to the *present* path can move a number
that is set by the interpreter finishing its frame. The whole ~13 % we are chasing is CPU: five
interpreted MAME CPUs at 6–12 % each, no DRC (`--NOASM=1`).

That cost lives inside the core, so it is **frontend-independent**:

- The core links no Vulkan library and resolves every entry point at run time from the frontend's
  `vkGetInstanceProcAddr` (`android.md` §1, `renderer_vk/vk_funcs.h`). RetroArch's Vulkan context and
  the deployment frontend's are the same contract — the core cannot tell them apart.
- It declares a stock `RETRO_HW_CONTEXT_VULKAN` with **no negotiation interface**
  (`renderer_vk/vk_context.cpp:532`) — it consumes whatever `VkDevice` the frontend hands it. RetroArch
  is the reference implementation of exactly that path.
- The per-device profiler that produced the diagnosis is a core-side hook (`m2vk_profile.h`, built with
  `PROFILER=1`) that writes straight to logcat. It reads identically no matter who is hosting the core.

So RetroArch runs the same instructions on the same big cores against the same 1.92 GHz cap. What it
does *not* reproduce is the deployment frontend's own CPU load — that is the reason §7 keeps the APK run
as the final word.

✅ **The context-negotiation question is already answered.** `vf2` and `daytona` boot, render and play
under RetroArch's Vulkan driver on an **Adreno 740** (`android.md` — Odin 2 Portal, the Quest 3's GPU
silicon family). The "does RetroArch drive our negotiation" unknown in `android.md` §6.2 is retired for
this GPU. Smoke-test it once on the Quest 3 (a different OS image and driver revision) before trusting a
number, then stop worrying about it.

---

## 2. What is already built vs. what is Quest-specific

The install and launch mechanics are done and documented in **`android.md` §7** and **`deploy-android.sh`**
— push the stripped `.so` to RetroArch's *downloads* directory (the cores directory is not adb-writable
on a production build), let RetroArch copy it into place, launch by intent. Do not re-derive any of that;
it works. Only the deltas below are Quest-3-specific.

| | Odin 2 (`android.md`) | Quest 3 |
| --- | --- | --- |
| RetroArch package | `com.retroarch.aarch64` | same (arm64 → aarch64 build) |
| ROM location | SD card, found by `fsLabel` | **no SD card** — internal storage, set `M2VK_ANDROID_ROMDIR` |
| Input | built-in gamepad | **pair a Bluetooth pad** (RetroArch's touch overlay is unusable in the 2D panel) |
| Video driver | `vulkan` (already was) | must be `vulkan` — confirm in `retroarch.cfg` |

`deploy-android.sh` errors out looking for the SD card when there isn't one, so name the internal ROM
directory explicitly:

```sh
export M2VK_ANDROID_ROMDIR=/sdcard/ROMS/model2      # /sdcard = internal, adb-writable
./devnotes/build-android.sh
./devnotes/deploy-android.sh daytona                 # strip + push core to downloads, ROM to ROMDIR
```

Prerequisite: install the aarch64 RetroArch APK on the headset and launch it once, so
`/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg` exists — the deploy script
reads its paths out of that file rather than assuming them.

RetroArch runs as an ordinary flat 2D Android app inside the Quest's panel. That is what we want: it
replaces the entire deployment-frontend CPU load with a thin host, leaving the emulation thread the run
of the big cores. (The system compositor still owns cores 6–7, same as always.)

---

## 3. The loop, per change

1. **Build** the Android core: `./devnotes/build-android.sh` (`REGENIE=1` after any `scripts/` change).
2. **Deploy**: `./devnotes/deploy-android.sh daytona` — with `M2VK_ANDROID_ROMDIR` set as above. On the
   headset, `Load Core → Install or Restore a Core → libmodelizer_libretro_android.so` the first time
   after each rebuild (RetroArch copies it into its cores directory; the UI ANRs briefly on the ~57 MB
   copy — that is not a failure, `android.md` §7.1).
3. **Launch by intent** (no menu navigation — `android.md` §7.2), pointing `ROM` at the internal path:

   ```sh
   adb shell am start -n com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture \
     -e ROM      /sdcard/ROMS/model2/daytona.zip \
     -e LIBRETRO /data/user/0/com.retroarch.aarch64/cores/libmodelizer_libretro_android.so \
     -e CONFIGFILE /storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg \
     -e DATADIR  /data/user/0/com.retroarch.aarch64
   ```

   (First launch after a rebuild: point `LIBRETRO` at the *downloads* copy to trigger the install, the
   *cores* copy thereafter — `android.md` §7.2.)
4. **Measure** on a heavy scene (§4).

The whole loop is one box: build the `.so`, `adb` it to the tethered Quest, launch, read the log. No
APK repackage, no VR walk.

---

## 4. Reading speed without the deployment `[speed]` line

The deployment frontend prints a `realtime` / `retro_run avg/max` line; RetroArch does not. The signal
is the same, read three ways:

- **Real-time verdict — audio + FPS.** RetroArch gates emulation to audio (`audio_sync = "true"`). A core
  that misses the frame budget crackles and underruns — the *same* audible symptom the deployment build
  has — and the on-screen counter (`fps_show = "true"`) falls below the driver's target refresh. This is
  the pass/fail: sustained target FPS on a heavy scene with clean audio.
- **Per-device split — the core's own profiler, and it is the authoritative one.** Build `PROFILER=1`
  (`m2vk_profile.h` enables MAME's `g_profiler` and dumps per-device % to logcat, tag `m2prof`). This is
  a direct `__android_log` call from inside the core, so — unlike RetroArch's own logging, which is
  near-silent to logcat and needs file-logging turned on (`android.md` §7.3) — it appears regardless of
  frontend config:

  ```sh
  adb logcat -c && adb logcat -s m2prof:V
  ```

  The profiler roughly doubles frame time, so read the **ranking**, not the absolute ms — exactly as on
  the deployment side. Ship builds default `PROFILER=0` to a dummy `g_profiler`, so this costs nothing
  when off.
- **`retro_run` timing.** Not exposed by RetroArch directly; the profiler split plus RetroArch's frametime
  counter cover it. If a per-frame `retro_run` number is wanted, the `m2prof` hook is the place to add a
  cheap logcat timer.

**Two things that must match the deployment measurement or the number lies:**

- **Heavy scene is mandatory** — a full-grid race, not attract. Light scenes already hit 1.00× and prove
  nothing. Drive it with the paired Bluetooth pad.
- **Pin the clock.** `adb shell cmd power set-fixed-performance-mode-enabled true` before measuring, so
  DVFS does not dip below the 1.92 GHz cap mid-run (worth ~7 % on its own, and it is system-wide, so it
  applies to RetroArch identically). It cannot exceed the cap — there is no clock headroom on this device.

---

## 5. A/B a core change on one binary

Every speed lever is env-gated so one build A/Bs cleanly (`M2VK_SOUND_THREAD`, `M2VK_SOUND_DELAY`,
`M2VK_ASYNC_PRESENT`, …). Pass the env through the launch so the same installed `.so` runs both arms —
set it on the app's environment before the intent, or bake it into the run and diff the `m2prof` ranking
and the sustained FPS across arms. Always re-run the correctness harness (`ab.sh` digests, on the host —
§6) before believing a speed win is free of an accuracy cost.

---

## 6. What this does NOT replace — correctness stays on the host

RetroArch-on-Quest is a **performance** instrument only. It has no per-frame digest and is not
bit-reproducible, so it cannot gate accuracy — same limitation any live device run has (`android.md`
§6, "none of the harness travels"). Correctness stays where it belongs: `ab.sh` on the host core, whose
FNV pixel digest is deterministic and bit-comparable to `ab-baselines.md`. The flow per change is
unchanged in principle — **validate on the host (digest matches baseline) → build the Android `.so` →
measure on the Quest** — only the last leg is faster now.

---

## 7. The absolute gate stays the deployment APK

RetroArch reads **optimistically** relative to the shipping build: it is a thin host, so it leaves the
emulation thread more of the big cores than the real frontend does (which runs its own main and render
threads alongside — ~29 % on the main thread in the device profile). A change that just reaches target
FPS in RetroArch has *not* been proven to hold once that load is added back, and thermals are gentler
without the VR render path on top.

So the milestone gate is unchanged: build the Android `.so`, repackage into the deployment APK, drive a
heavy race in VR, confirm the real `realtime` line clears the bar. Run it **once per milestone**, not
per change. RetroArch tells you which changes are worth taking that far.

---

## See also

- `android.md` — the install/intent mechanics this loop rides on (§7), and the first-light proof on
  Adreno 740.
- `m1audio-thread-plan.md` — the first speed lever (thread the sound 68000); its Stage 2 "measure on
  device" step is this loop.
- `performance.md` §2a/§6 — the desktop analysis that sized the CPU ceiling and established the GPU is
  not the constraint.
- `build-android.sh`, `deploy-android.sh` — the build and push.
