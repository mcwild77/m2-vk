# System 21 (Star Blade) on the Quest 3 — the C67 DSP is the wall, and idle-skip does NOT help

**Status, 2026-09-04: PARKED. Diagnosed, one lever prototyped and rejected.** Star Blade runs at
**~0.74× realtime (~44 fps)** on the Quest 3 under Age of Joy. The cause is now measured precisely: a
single interpreted DSP dominates the frame, and its time is **real geometry compute, not reclaimable
idle**. The one cheap-looking lever — a slave-DSP idle-loop skip, which is even an upstream TODO — was
built, validated, and **reverted**: zero speed-up and it corrupts Air Combat. What remains is
large-ticket only (a TMS320C25 recompiler, or HLE'ing the C67 geometry). This file is the record so it
is not re-derived.

Sibling docs: `system21plan.md` (the renderer, DONE), `retroarch-quest-perf.md` (the Model 2 CPU dyno
loop — its method applies here), `performance.md` (Model 2 §2a), `lazy-baud.md` (the Model 2 lever this
was the failed analogue of).

---

## 1. Symptom and how it was diagnosed on-device

Reported live: Star Blade runs badly in AoJ (`com.curif.AgeOfJoy`, a Unity VR arcade shell that loads
the same libretro core). ADB was the only access; **`simpleperf` is blocked** on the Quest for a
non-debuggable app (perf_event permission denied even with `security.perf_harden=0`), so stack
profiling was out. The core's own instrumentation carried the diagnosis instead:

- **`pdlr [speed]`** (logcat): `coreFrames=45/s (target 59.94) realtime=0.74x retro_run avg=22.3 max=24ms
  blit=0.05ms`. The core hits ~44–45 of 60 fps; `blit` (present) is negligible.
- **`m2stall`** (`m2vk_stallmeter.h`): `frame 22.4ms = cpu 20.73 + park 0.99 + other 0.69`. The `cpu`
  term is `CLOCK_THREAD_CPUTIME_ID` — real emulation compute — and it is the **entire** frame. `park`
  (waiting on the retro_run baton) and `other` (throttle/scheduler) are noise. So it is compute-bound
  on the one emulation thread, NOT a frontend/baton stall.
- **`VrApi`** (logcat): `CPU4/GPU=4/4` (both at max level), `App≈10ms`, `Stale=0` (no reprojection),
  GPU ~80% busy at 545 MHz. That GPU load is AoJ's Unity scene, not our core (our `blit` is 0.05 ms).
- **`m2vk_affinity.h`** verified working: the emulation thread is pinned to `Cpus_allowed_list: 3-5`
  (the 2208 MHz big cluster) and was observed running on CPU 4. Not a scheduling problem.

Then the definitive split, via a **`PROFILER=1` core in RetroArch on the headset** (cleaner than through
Unity). `m2vk_profile.h` enables MAME's own per-device profiler and dumps `unnorm% norm% 'tag'` to
logcat (tag `m2prof`) every ~60 frames. Rock-steady across dumps:

| device | share |
|---|---|
| **`:namcos21dsp_c67:dspslave0`** | **~58%** |
| `:namcos21dsp_c67:dspmaster` | ~10% |
| `:maincpu` / `:slave` (2× 68000) | ~6% / ~5% |
| `:audiocpu`, `:c68mcu:mcu` | ~3% / ~2% |
| Video Update + OSD Blitting (our seam: geometry capture + present) | ~7% + ~7% |
| sound / timers / input / overhead | ~4% |

Two structural facts from `namcos21_dsp_c67.cpp` explain the 58%:

1. The board is 1 master + 4 slave C67 DSPs (`namco_c67_device : tms320c25_device`, a real interpreted
   core, internal ROM `c67.bin`). The emulation runs **one** slave **clocked at 4× (160 MHz)** to stand
   in for four real 40 MHz slaves running in parallel (`device_add_mconfig`: `set_clock(clock()*4)`;
   `dspslave1..3` are `set_disable()`d and never appear in the profile). That 4× is the cost multiplier.
2. Our renderer replaces MAME's polygon *fill*, but MAME still runs the DSP to *produce* the geometry —
   inherent, and where the time is.

---

## 2. The idle-skip lever — hypothesis, prototype, and why it was rejected

**Hypothesis (wrong):** the 4× clock is sized for the worst-case frame, so in a typical scene the slave
finishes early and spends the rest of its inflated budget spinning on its port-2 poll
(`get_input_bytes_advertised_for_slave`) waiting for the master — reclaimable, the same shape as
`lazy-baud`. The driver header even lists `TODO: add DSP idle loop speedup hacks?`.

**Prototype (2026-09-04, reverted the same day):** a new header `src/osd/libretro_m2/m2vk_dsp_idle.h`
plus guarded (`#ifdef S21VK`) hooks in `namcos21_dsp_c67.cpp` — `spin_until_trigger` on the slave in the
one port-2 branch where the poll makes no progress (advertised == available, no kickstart pending), with
`trigger()` wakes from every master-side event that changes that condition (`transmit_word_to_slave`,
`dsp_port8_w` latching `master_finished`, `namcos21_kickstart`, `reset_dsps`). Gated by
`M2VK_DSP_IDLE_SKIP` (default ON, `=0` to disable). The wake in `namcos21_kickstart` is load-bearing: it
pulses the slave RESET line, and a RESET pulse does not clear a separate TRIGGER-suspend bit.

**Validation (Windows host + retrohost, software renderer, off vs on):**

- **No speed-up.** `M2VK_HOST_PERF` (the unthrottled headroom — retrohost throttles to 1×, so wall-clock
  is meaningless): starblad **15.948 → 15.947 ms/frame**; aircomb slightly *worse*. The emulated
  instruction stream is identical host↔Quest, so ~0 ms on the host ⇒ ~0 ms on the Quest.
- **Breaks Air Combat.** Per-frame hashes (`M2VK_HOST_FRAME_HASH=0`) over 2400 frames, off vs on:
  starblad 1/2400, solvalou 1/2400, cybsled 7/2400 — all isolated, self-correcting transients — but
  **aircomb 160/2400, a CONTIGUOUS block (frames 1330–1489) that aligns at NO frame shift** (shift-0
  only 21/181 in-window; every other shift worse). That is ~2.7 s of genuinely wrong geometry, not a
  phase shift: parking the slave during the throttled per-word handshake desyncs multi-word primitive
  assembly. (starblad's one differing frame measured ~1.6% of pixels — a single object one frame late.)

**Conclusion:** the port-2 poll is *active handshaking*, not an idle wait; the spins it skips are
negligible in duration (hence 0 ms), while shifting their timing is enough to corrupt aircomb. **The
slave's 58% is real DSP compute.** The upstream TODO's "idle loop hack" would have to be an *internal*
microcode branch-to-self detected at CPU-PC level (à la `eolith_speedup.cpp`), not a port hook — but the
host perf shows total slave compute is genuine (the whole machine is only ~104% of realtime on this
box), so there is little internal spin to reclaim either. Reverted; tree clean, upstream diff unchanged.

---

## 3. What is actually left (all large)

- **Lower the 4× clock — no.** The slave is ~compute-bound at 4×; less throughput drops polygons.
- **A TMS320C25 recompiler.** MAME's `tms320c2x` core is interpreter-only (no DRC back end). This is the
  clean fix and a major undertaking.
- **HLE the C67 geometry.** Do the transform in C++ instead of running the DSP. Big effort, real
  accuracy risk against the software digest.
- **Optimise the `tms320c2x` interpreter hot path.** Modest, general, helps every C67 game a little —
  will not close a 25% gap alone.

None is a session's work, which is why System 21 perf is parked.

---

## 4. Reusable method notes (for whoever un-parks this)

- **Deploy a diagnostic core to the Quest's RetroArch** without ROMs: build with the flag, then
  `M2VK_ANDROID_ROMDIR=/sdcard/Download ./devnotes/deploy-android.sh` (the SD-label gate needs *some*
  existing dir on the Quest). On the headset: Load Core → Install or Restore a Core → the pushed `.so`
  (re-install after every rebuild). A **`PROFILER=1`** core is left in `RetroArch/downloads` on the
  headset from this session; rebuild `PROFILER=0` before shipping anything.
- **`PROFILER=1` needs a clean object tree** — it flips a global define:
  `rm -rf build/android/obj && REGENIE=1 PROFILER=1 ./devnotes/build-android.sh`. Under the profiler,
  read the per-device **percentages**, never the inflated `[speed]`.
- **retrohost throttles to 1×.** For a compute number use `M2VK_HOST_PERF=1` (`speed:` line), never wall
  time. For correctness use `M2VK_HOST_FRAME_HASH=<frame>` (per-frame `fh` hashes) and diff the streams
  — the whole-run `digest:` conflates a benign one-frame timing shift with real corruption; the
  per-frame stream separates them (test frame-shift alignment to tell phase from corruption).
- **Windows build gotchas** (see the memory `windows-host-build` / `windows-android-build`): build via
  `MSYSTEM=MINGW64 C:\msys64\usr\bin\bash -lc '…'` with `export OS=Windows_NT`; use `make -C /e/m2-vk`
  (a login shell `cd`s to `$HOME`); run `retrohost.exe` with `/mingw64/bin` on `PATH` for the runtime
  DLLs; set `MSYS_NO_PATHCONV=1` so `/sdcard/…` and `/storage/…` adb paths are not mangled.
