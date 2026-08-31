# M1 sound board threading — staged plan + desktop A/B recipe

**Goal:** run the SEGAM1AUDIO board (the 68000 sound board) on its own host thread, so heavy-frame
wall time approaches the slowest single device instead of the sum of all of them. On a Quest 3 the
Model 2 emulation is CPU-bound and the per-device profile is flat — five co-equal CPUs at 6–12 %
each — so parallelising a device is the right lever, not optimising one path. The sound 68000
(`:m1audio:sndcpu`) is co-largest (~12 %) and the loosest-coupled device, so it goes first. This is
the Model 1 sound-board / Daytona-class path (`model2o`); the later `model2a/b/c` SCSP path shares
`soundram` and is a separate, harder job.

## Why it's safe to split (feasibility spike, done)

- **Coupling is a serial cable + the audio mixer, nothing else.** SEGAM1AUDIO's only wires to the
  machine are the bidirectional i8251↔i8251 serial link (`write_txd` / `rxd_handler`) and its audio
  streams into the global speaker. **No shared RAM.** It's a genuinely separate board, exactly like
  the hardware.
- **The link tolerates ~1 frame of latency.** It's a MIDI-rate (31.25 kbps) fire-and-forget command
  stream, interrupt-driven both ways; the main never busy-waits on a byte-level reply inside a frame.
  A sound effect landing one frame late is inaudible.
- **The accuracy harness is a video-only guardrail.** `devnotes/ab.sh` / retrohost `hash_frame()`
  FNVs the frame buffer and nothing else. Threading the sound board doesn't touch the render path, so
  a **bit-identical pixel digest proves the main emulation is uncorrupted**; audio is ear-validated on
  device, and savestates must still round-trip.

## Two-box workflow (this is inherent, not a limitation)

Correctness and performance are measured on different machines and neither can do the other's half:

- **Mac (host core + `ab.sh`)** — the correctness gate. `ab.sh` is deterministic (scripted input,
  fixed frame count, software render) so its digest is bit-comparable. Every code change is validated
  here first: digest matches baseline ⇒ emulation not corrupted.
- **Quest 3 (Android `.so`, driven from the AoJ side)** — the performance verdict. The whole problem
  is a Quest-specific CPU ceiling; a desktop host proves nothing about the target `realtime`. And a
  live VR run has no per-frame digest and isn't bit-reproducible, so no accuracy gate is possible
  on-device.

So each change goes: **validate on Mac (digest) → build Android `.so` → measure on Quest.**

## Stage 0 — de-risk with NO threading (CODE WRITTEN, awaiting Mac digest run)

A fixed, single-threaded, order-preserving delay on the **sound→main serial reply** line, to answer
one question: *does the main gate its video on sound-reply timing?* If a ~1-frame delay leaves the
pixel digest bit-identical, the thread split is safe for video.

Implemented in `src/mame/shared/segam1audio.{h,cpp}`:

- `output_txd()` (the sound board's UART TXD → `m_rxd_handler` → main UART RXD) now, when the delay is
  enabled, enqueues `(deliver_time, state)` and delivers it `m_rxd_delay` later on a single
  `emu_timer` (`rxd_delay_tick`). Constant delay + monotonic enqueue ⇒ FIFO order == delivery order,
  so one timer armed on the empty→non-empty edge is exact.
- Delay is read once in `device_start` from the env var **`M2VK_SOUND_DELAY`** (microseconds; µs
  because Daytona's frame rate is odd). Unset / non-positive ⇒ zero delay ⇒ the old immediate path,
  byte-for-byte. `getenv` returns null on device, so shipping / Android builds are unaffected.
- Reply-only: the main→sound direction (`write_txd`) is untouched — it can't affect main video, and
  delaying replies is the faithful model of "the sound board runs a frame behind".

### Desktop A/B recipe (Mac)

```sh
# from the m2-vk repo root
make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10   # → ./modelizer_libretro.dylib
./devnotes/build-retrohost.sh

ROMS=/path/to/Model2            # the folder holding daytona.zip

# baseline (delay OFF) vs ~1 frame delay (17400 µs ≈ 1 frame; also bracket with 34800 = ~2 frames)
                          ROMS=$ROMS ./devnotes/ab.sh daytona 2500 /tmp/ab-base
MODE="M2VK_SOUND_DELAY=17400" ROMS=$ROMS ./devnotes/ab.sh daytona 2500 /tmp/ab-delay

# compare the 3D video digests across the two runs
grep '^3d ' /tmp/ab-base/daytona.txt /tmp/ab-delay/daytona.txt
```

Boot→attract already runs the sound board and its reply path, so it's a valid first cut. For a harder
test, drive gameplay via retrohost's control script (`frame:control[:held][:port]`, e.g.
`600:select,660:start,...`) so engine sounds flood the link.

**Reading:**
- **Digests bit-identical** across baseline and both delay brackets ⇒ the main doesn't gate video on
  sound-reply timing ⇒ **Stage 1 is safe → proceed.**
- **Digests move** ⇒ **STOP.** The main gates video on reply timing; reassess the boundary before
  building any thread.

## Stage 1 — thread the board (behind `M2VK_SOUND_THREAD=0/1`, default OFF)

Lift the whole SEGAM1AUDIO subtree onto a worker thread ~1 frame behind the main:

- main→sound bytes cross a lock-free queue;
- the board's mixed audio lands in a ring the main's audio output pulls (reuse the ring already built
  for the HW cores);
- sound→main replies return a frame later (the Stage 0 delay, now real cross-thread latency).

The hard part is MAME-side: the m1audio subtree currently runs on the single global
`device_scheduler` and must be lifted onto a private timeline with serial + audio bridges. Env-flag so
one binary A/Bs cleanly. **Register the reply FIFO (and any thread hand-off state) in save state** —
Stage 0 skips this because `ab.sh` runs boot-deterministic, but the threaded build must survive
savestates.

Verify: `ab.sh` digest still matches the no-thread baseline (main unaffected), audio ear-test on
device, savestates round-trip.

## Stage 2 — measure on the Quest (AoJ / device side)

Build the Android `.so`, repackage into the AoJ APK, drive a heavy Daytona race, read the `[speed]`
line. Expect to recover most of the ~4 ms the sound 68000 costs → `realtime` up toward the target
(heavy-frame `retro_run` ≤ 17.4 ms, `realtime` ≥ ~0.99×). If it lands short, evaluate threading the
TGP copro next, and/or the broadened interpreter hot-path work.
