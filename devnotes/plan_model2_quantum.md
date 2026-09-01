# Scheduler-quantum experiment — RUN 2026-09-01. Result: the quantum is a dead lever; the
# **UART baud clock** is the live one, and it is worth 34–57 % of emulation-thread compute.

**Verdict:** do not sweep the quantum. `model2o` already runs at MAME's coarsest possible quantum,
and on `model2b` the driver's `set_maximum_quantum(18000)` is fully masked and does nothing. What
actually forces ~1,000,000 scheduler synchronisations per emulated second on **every** Model 2
machine is a 500 kHz `clock_device` feeding the i8251's TxC/RxC. Silencing it cuts desktop core time
by 34–57 %. That is 3–10× the plan's own 0.5 ms go/no-go gate, and it displaces the TGP recompiler
as the next thing to build.

Re-create the instrumentation with `git apply devnotes/qprobe.patch` (throwaway; three upstream files,
all env-gated and off by default — do not commit it).

## What was measured

`M2VK_QPROBE=1` on `retrohost --vk`, daytona, per emulated second:

```
[qprobe] configured maximum_quantum = 0.016666666        <- 1/60 s, MAME's default
[qprobe] perfect interleave (m_quantum_minimum) = 60.0 ns
[qprobe] ACTUAL quantum in use = 16666666.7 ns (60.0 Hz)
[qprobe] t=6s slices/s=1001909 rounds/s=2003933 devruns/s=4003355 timerfires/s=2002771
[qprobe]   timer clock_device::clock_tick     2000000/s (33333/frame)
[qprobe]   timer ym_generic_device::fm_timer_handler   868/s
[qprobe]   timer screen_device::vblank_begin            58/s
```

Read that timer histogram again: **`clock_tick` is 99.87 % of every timer callback in the machine.**

### 1. The quantum cannot be coarsened — it is already at the ceiling

`device_scheduler::rebuild_execute_list` defaults to `attotime::from_hz(60)` when a driver sets
nothing, and `compute_perfect_interleave` only ever raises the floor
(`m_actual = max(m_requested, m_quantum_minimum)`). model2o sets nothing, so it runs at 16.67 ms —
the coarsest MAME allows. **The plan's proposed sweep (18000 → 6000 → 2400 → 1200 Hz) is 55 µs →
833 µs: every arm is *finer* than what daytona already runs, i.e. strictly slower.** The lever was
pointing the wrong way.

### 2. The quantum is irrelevant anyway — timers set the granularity

`timeslice()` runs devices to `min(basetime + quantum, next timer expiry)`. With a 1 µs timer always
pending, the quantum never binds. Measured on model2b (`fvipers`), which *does* set 18000 Hz:

```
[qprobe] ACTUAL quantum in use = 55555.6 ns (18000.0 Hz)
[qprobe] t=6s slices/s=1005559 ...
```

1.006 M slices/s against a quantum that permits 18 k/s. **`model2.cpp:2915`'s
`set_maximum_quantum(attotime::from_hz(18000))` has no observable effect.** Do not cite it as
precedent for anything.

### 3. The real source: two 500 kHz `clock_device`s per machine

| machine | clock devices at 500 kHz | edges/s |
|---|---|---|
| model2o (daytona) | `:uart_clock` + `:m1audio:uart_clock` | 2 M |
| model2a/2b (vf2, srallyc, vcop2, fvipers) | `:uart_clock` (the SCSP block) | 1 M |

`model2.cpp:2586`, `model2.cpp:2654`, `shared/segam1audio.cpp:80` — all `16_MHz_XTAL / 2 / 16` =
500 kHz, all feeding `i8251_device::write_txc` + `write_rxc`. `clock_device` fires `clock_tick` on
**both** edges, so each device is 1 M timer callbacks/s.

The i8251 acts on the falling edge for TxC and the rising edge for RxC, then divides by
`m_br_factor = 16` (`i8251.cpp:389`, `:171`). **15 of every 16 edges do nothing but increment a
counter** — and each one still costs a full scheduler break: 33 k rounds/frame, 67 k device
`run()` dispatches/frame on daytona.

Daytona's two clocks are phase-coincident, which is why silencing only one is nearly free (−4 %,
digest unchanged) and silencing both is transformative: the *break points*, not the callbacks, are
the cost.

## The prize (desktop, `retrohost --vk`, 1500–1800 frames, `perf: core ms/frame`)

`M2VK_QPROBE_NOUART=all` silences the baud clocks. This **breaks the sound link on purpose** — it is
an upper bound, not a candidate. Digest moves on daytona (sound state diverges); it is unchanged on
the others because their video path never observes the link.

| game | board | base | no baud clock | saving |
|---|---|---|---|---|
| daytona | 2O | 3.797 | 2.018 | **−47 %** |
| vf2 | 2A | 3.650 | 2.047 | **−44 %** |
| srallyc | 2A | 3.685 | 1.570 | **−57 %** |
| vcop2 | 2A | 3.538 | 1.968 | **−44 %** |
| fvipers | 2B | 4.501 | 2.985 | **−34 %** |

### The control — this is scheduler overhead, not "the game did less"

The table above changes emulated behaviour, so on its own it proves nothing (measure-dont-handwave).
The control adds a `CLOCK` device **nothing listens to**: it changes no emulated state, only
scheduler granularity, so the digest must not move. Daytona, `M2VK_QPROBE_DUMMY=<Hz>`:

| dummy clock | new break points/s | core ms/frame | digest |
|---|---|---|---|
| none | — | 3.817 | `570fa675693d242f` |
| 250 kHz | ~0 (coincident with the 500 kHz grid) | 3.959 | `570fa675693d242f` |
| 500 kHz | ~0 (coincident) | 3.998 | `570fa675693d242f` |
| 1 MHz | **+1 M** (the odd half-edges fall between) | **5.300** | `570fa675693d242f` |

Digest identical in all four arms. Cost tracks *new* break points and ignores extra callbacks on
existing ones: **+1 M break points/s = +1.48 ms/frame ≈ 85 ns each on this desktop.** The machine
currently carries ~1 M/s of them, and removing them measured −1.78 ms. The two numbers agree. The
saving is real scheduler overhead.

## ✅ BUILT 2026-09-01 — see [lazy-baud.md](lazy-baud.md)

Both shapes below are implemented in `src/osd/libretro_m2/m2vk_baud.{h,cpp}`, default ON
(`M2VK_LAZY_BAUD=0` restores the stock `CLOCK`). Measured **−36 % to −48 %** of desktop core ms/frame,
essentially at the `NOUART` upper bound in the table above. The UART itself is bit-exact — proved by
the dummy-clock control run in reverse (lazy clock **on** *plus* a dummy `CLOCK` restoring only the
break points reproduces the stock video **and audio** digests exactly). What does move is MAME's
device interleaving, on vf2 and vcop2. The rest of this section is the design as written before the
build; `lazy-baud.md` is what was actually built and what it measures.

## What to build instead — a lazy baud clock

The mechanism has to keep the *observable* i8251 events (TxD transitions, TxRDY/TxEMPTY/RxRDY, the
received byte) at bit-exact instants while not being a scheduler break point 1 M times a second.
Two shapes, cheapest first:

1. **Bit-boundary scheduling for TX.** `transmit_clock()` produces output only when
   `m_txc_count == m_br_factor`. Schedule one timer at that boundary instead of being poked 16×.
   Bit-exact by construction, 16× fewer break points. When TX is fully idle (register empty, TxRDY
   set, nothing pending) it needs no timer at all until `data_w`.
2. **Demand-gating for RX.** The receiver needs no edges until RxD falls. Wake on `write_rxd`, with
   the restart phase computed on the true grid (`edges = floor((now − anchor) / period)`) so
   `m_rxc_count` lands where free-running would have put it. Naively batching RX edges is **not**
   safe — start-bit detection would quantise to 32 µs on a 32 µs bit and can sample the neighbour.

MIDI at 31.25 kbaud is idle the overwhelming majority of the time, so a demand-gated clock should
recover most of the table above.

⚠️ Ship it the project way (`m2vk_snd::enabled()` pattern): new file, a guarded hook, nothing bare in
an upstream file. And note the video digest **cannot see sound** — vf2/srallyc/vcop2 held their
digests with the link fully dead. Any candidate needs a listening check, not just `ab.sh`.

## Still open

- **Quest measurement (was Step 3).** Not run. Desktop cores are not Adreno's; the per-break-point
  cost there is likely 2.5–4× the 85 ns measured here, which would make the saving larger, not
  smaller. Needs `build-android.sh` → `deploy-android.sh` → the manual Install-or-Restore step →
  `adb logcat -s m2stall:V m2prof:V` on a heavy daytona race.
- **The sound thread does not fix this.** With `m2vk_snd::enabled()` the m1audio board (and its
  500 kHz clock) moves to the worker machine, but the main machine keeps `:uart_clock` at 1 M
  edges/s. That is consistent with the Quest's 50 → 57.5: the thread moved half the break points to
  another core rather than removing them.

## Context: what this gates

The 2026-09-01 worklog entry picked the **MB86233 (TGP) recompiler** as the next big job. That
argument stands on its own merits, but it is no longer the *first* thing to do: the TGP is one
device inside a scheduler that is spending ~40 % of its time on switching, and a recompiler makes
each dispatch cheaper without making them fewer. Do the lazy baud clock first, then re-measure the
per-device profile before committing to the DRC.
