# Performance — where the time goes, and what to trade for it

**Status: a proposal, not a decision.** The authoritative architecture and phase plan is
`../Polydiver/PDDocs/model2/model2_libretro_core.md`. This file is analysis to feed into it; §8 says
which phase each item belongs to, but *changing the phase plan happens in the Polydiver doc*, not
here. Written 2026-07-26, after P3 step 6 landed and while step 7 was in flight in another session.

**Nothing in here has been profiled on-GPU.** No timestamps, no per-stage breakdown. Claims are
tagged **[measured]** where a run produced the number and **[inferred]** where it is reasoning about
the shader or the target hardware. Do not promote an [inferred] line to a fact without a run behind
it — §2 is a worked example of what happens when a number gets copied forward instead of measured.

---

## 1. The target, which is new information

**Quest 3 — Snapdragon XR2 Gen 2, Adreno 740 — as an embedded binary, with the Vulkan output piped
into a Unity session.** The same shape is already in production for Dreamcast and Model 3 emulation,
where it is tight but holds 60 FPS.

This is not recorded anywhere else in `devnotes/`, and **every judgement below keys off it.** Several
"costs nothing" conclusions in the worklog were measured on an M5 with unified memory and do not
transfer.

**What is already right:** the handoff. The core creates its own device via the HW-render
context-negotiation interface and renders into frontend-provided images
(`RETRO_HW_RENDER_INTERFACE_VULKAN` / `set_image`) — deliberately the same shape as Polydiver's AHB
handoff, chosen at P2 *because* of the VR path. Not a concern, and not something to redesign.

**What it invalidates:** the desktop performance picture, in two ways — §2 (the headline number was
never a headroom measurement) and §3.3 (a memcpy that is free on unified memory is not free on a
shared mobile bus).

---

## 2. The 105 % figure is a throttle artifact, not headroom

The number quoted through P3 is RetroArch's `Average speed:`, and it is **not** a measure of how much
room is left. RetroArch runs the core at its declared 57.52 Hz, so 104.5 % means "keeps up with
realtime", not "has 5 % spare".

The tell is that it does not move **[measured]**:

| step | what landed | RetroArch speed |
|---|---|---|
| 3 | untextured polygons on the GPU | 104.45 % |
| 4 | the entire textured path — 2 MB sheets, mip chain, microtexture | 104.45 % |
| 5 | the translucent cutout | 104.38 % |
| 6 | per-polygon scissor | 104.50 % |

Landing ~16 texel fetches per fragment moved it by less than the noise. If the GPU were anywhere near
its limit that is impossible; the number is pinned by the throttle.

**The real headroom number is `retrohost`'s unthrottled figure.** It has now been taken — see §2a,
which is the answer and which reorders most of this document.

---

## 2a. ✅ ANSWERED — the unthrottled numbers, and they change what §4 is worth

**[measured 2026-07-26, `devnotes/perf.sh`, 6 fixtures × 3 configs × 3 repeats, 2500 frames each with
the first 1500 skipped as boot, nothing else running in the tree.]** Speeds are `% of realtime` against
the declared 57.52 Hz; `pipe` is the pipelined bound (see the harness caveat below).

| game | emul (thread alone) | sw (MAME's rasterizer) | vk (pipelined) | vk core ms | vk gpuwait ms |
|---|---|---|---|---|---|
| `vf2` | 416.3 % | 311.7 % | **399.0 %** | 4.357 | 0.720 |
| `vcop2` | 512.4 % | 412.9 % | **488.1 %** | 3.562 | 0.916 |
| `srallyc` | 410.5 % | 332.5 % | **394.6 %** | 4.406 | 0.925 |
| `sgt24h` | 377.7 % | 306.4 % | **364.6 %** | 4.767 | 0.897 |
| `desert` | 578.2 % | 513.8 % | **550.8 %** | 3.157 | 0.709 |
| `waverunr` | 422.9 % | 327.0 % | **403.8 %** | 4.305 | 0.936 |

Spread over 3 repeats was ≤ 4.0 % everywhere and ≤ 1.3 % on ten of the eighteen cells, so these are
signal and not contention (§7's protocol).

**Three findings, in the order they matter.**

**1. P3 is worth +7 % to +28 %, and that is the whole prize.** `sw` → `vk`: vf2 +28 %, waverunr +23 %,
srallyc +19 %, sgt24h +19 %, vcop2 +18 %, desert +7 %. Real, but an order of magnitude smaller than
"the rasterizer was the largest single CPU cost" suggested, because it was never the *largest* cost —
just the largest one we could remove.

**2. ⚠️ The Vulkan path is already within 3.5–5 % of the emulation-only ceiling.** vf2 399.0 of
416.3 = 95.8 %; vcop2 95.3 %; srallyc 96.1 %; sgt24h 96.5 %; desert 95.3 %; waverunr 95.5 %. **Every
item in §4 is bidding for at most ~4.5 % of the frame.** Driving the renderer's cost to *literally
zero* moves vf2 from 399 % to 416 %. This is §6's answer made quantitative: with two interpreted i960s,
the copro DSPs and a 68000 on the emulation thread, the renderer is no longer where the time is.

**3. The renderer's CPU side costs ~0.15–0.20 ms/frame.** `vk core` minus `emul core`: vf2 0.181,
desert 0.151, waverunr −0.006 (i.e. below the noise). Recording, buffer fills and the 2 MB texture
memcpy together are **~4 % of the frame**. §3.3's dirty-range fix is measured to be worth nothing here.

### ⚠️ The harness cannot overlap CPU and GPU, so the speed is a bracket

`retrohost --vk` reads every frame back off the GPU and waits on a fence to do it, immediately after
the core's submit. CPU and GPU therefore never run at once, which no real frontend does. So `perf.sh`
prints two columns and **quoting one of them is wrong**:

- `serial%` = `core + gpuwait` — the lower bound, what this harness achieves
- `pipe%` = `max(core, gpuwait)` — the upper bound, what a pipelined frontend reaches

The truth is between them and much nearer `pipe%`, because `gpuwait` is itself an *over*-estimate of
the core's GPU time: the wait also covers our own whole-frame image→buffer copy. **On the serial
figure alone the Vulkan path reads as slower than the software rasterizer on vcop2** (388.2 % against
412.9 %), which it is not — that is the harness's serialisation, not the renderer. The table above
quotes `pipe`.

Cross-check: MAME's own `Average speed:` prints at the end of every `retrohost` run and tracks the
wall figure (vf2 vulkan 2500: MAME 353.73 %, our wall 355.0 %). It includes load time and the boot
frames, so it is a sanity check, not the measurement.

---

## 3. Where the GPU cost is

> ## ⚠️ §3 is now measured, and it was right about the ranking and wrong about the stakes
>
> **[measured 2026-07-26, differential switching on `waverunr` — 99.18 % 3D coverage, the largest 3D
> layer in the fixture set — and `desert`, the microtexture extreme. `gpuwait` ms/frame:]**
>
> | | waverunr | desert |
> |---|---|---|
> | 2D composite + our read-back copy (`M2VK_NO_3D=1`) | 0.492 | 0.480 |
> | + untextured 3D (`M2VK_FORCE_SOLID=2`) | 0.548 | 0.509 |
> | + the whole texture chain | **0.936** | **0.709** |
>
> So of the 3D layer's GPU cost, **rasterisation is 0.056 ms and the hand-written index-space
> filtering is 0.388 ms — 87 % of it.** §3.1's ranking *within the GPU* is confirmed outright.
>
> **But the GPU is 0.94 ms of a 4.31 ms frame, and over half of that is the harness's own read-back.**
> The filtering §3.1 calls "the dominant cost" is **9 % of the frame on the fixture that stresses it
> hardest** and 5 % on the microtexture extreme. Everything below is a correct description of where the
> GPU's time goes and a wrong description of what it is worth on this machine. Read it with §2a.
>
> None of this transfers to Quest 3, where §1 says the memory bus and a weaker GPU relative to its CPU
> change the ratio — but the ratio has to be **re-measured there**, not extrapolated from here.

### 3.1 Index-space filtering — the dominant cost *of the GPU's share*

`poly.frag`'s `fetch_bilinear_texel` does four `get_texel` transliterations by hand: address
arithmetic into a storage buffer, four SSBO loads, then packed-lane LERPs. The mip chain makes that
trilinear, so eight. Microtexture adds another trilinear on top. **Worst case is roughly 16 manual
texel fetches per fragment, every one a general storage-buffer load, with the texture units completely
idle** **[inferred from the shader; not profiled]**.

**Why hardware samplers are unusable, and this is the load-bearing constraint:** MAME filters in
**index space, before the LUTs**. A hardware sampler blends post-LUT colours, which is a different
answer — this is already written down as a settled P3 decision ("filtering must be done by hand in the
shader"). It is correct. It is also the most expensive thing in the renderer.

On Adreno this is worse than it is here: Apple's unified memory and large caches are forgiving of
random general loads, and Adreno's advantage is concentrated in the texture path we are not using
**[inferred]**.

### 3.2 Early-Z is partly given up, and it has to be

There is no `EarlyFragmentTests` execution mode, so depth writes happen at **late** fragment tests.
That is required, not incidental: a discarded fragment must leave the draw-order key unclaimed so a
later polygon can win the pixel, which is exactly what reproduces `m_fillmap`. The `checker` stipple
has rested on it since step 3 and the translucent cutout since step 5.

**Two things make this cost more than it looks:**

- **One pipeline serves the whole pass, so every polygon pays it** — not just the translucent ones.
  `checker` discards too, and a `discard` anywhere in the shader module is a property of the module.
- **Model 2 draws front-to-back with first-writer-wins.** That is the *ideal* submission order for
  early-Z, and the renderer is currently unable to exploit it. This is a free advantage being thrown
  away, on top of an expensive fragment shader.

Early depth *rejection* is still permitted; it is early depth *write* that is not, so the buffer
updates lag and rejection for fragments already in flight is less effective. **A partial loss, not a
total one** **[inferred — this is exactly the claim that wants a timestamp behind it]**.

### 3.3 The 2 MB texture upload

Both sheets are `memcpy`'d into the slot's buffer every frame that draws a textured polygon,
unconditionally, with no dirty check — about **115 MB/s** at 57.5 fps, on the frontend thread inside
`retro_run`.

- **Free here** **[measured: RetroArch unchanged at 104.45 % across step 4, which is what landed it]**.
- **Not free on Quest 3**, where LPDDR5 is shared with the CPU and the bandwidth budget is far tighter
  **[inferred]**.

`p3-hw-geometry.md` already names the fix and the reason it was deferred: a dirty check on 2 MB is a
compare against a shadow copy, i.e. the memcpy again — so the check belongs in the **write handlers**,
as a dirty range on `tex0_w`/`tex1_w`. Texture RAM barely changes outside scene loads.

---

## 4. Ranked

> ⚠️ **§2a says do not fund this list on desktop.** The whole of it is bidding for the ~4.5 % of the
> frame that separates the Vulkan path from the emulation-thread-alone ceiling, and item 1 — the best
> item here — is bidding for a slice of the 0.056 ms that rasterisation costs. The ordering below is
> still believed correct; it is the *budget* that turned out not to exist. Two consequences:
>
> - **Anything here justified by desktop speed is now unjustified.** Item 2 (dirty-range the upload) is
>   measured at ~0 on this machine; do it for Quest 3's bandwidth or not at all.
> - **Item 1 keeps its place for a different reason.** performance.md put it in P4 so the depth path is
>   verified once rather than twice, and that argument is about *risk*, not speed. It survives.
>
> The list becomes live again on Quest 3, where the ratio is different and has to be re-measured (§1).

### Free — no accuracy cost, should not be core options

1. **Split the pipeline so opaque non-checker polygons have no `discard`.** Specialisation constant,
   two pipelines. Recovers early-Z on a stream that is *already* front-to-back, which is the best
   cost/benefit item on this list and the one to do first. MAME reaches the same specialisations by
   template parameter, so this mirrors the reference implementation rather than diverging from it.
2. **Dirty-range the 2 MB upload** (§3.3). Independent of everything else.

### Core options — accuracy traded for speed

3. **`microtexture=off`** — removes up to 8 fetches per fragment. The per-polygon `FLAG_UTEX` already
   exists, so this is nearly free to implement.
4. **`filtering=trilinear|bilinear|nearest`** — bilinear halves the fetches, nearest quarters what is
   left. Visually minor at 1× and *more* minor as resolution rises.
5. **The big lever: a texture cache with hardware sampling.** See §5.

### Uncertain, and probably not worth the risk

6. **fp16 / `mediump`** — a real ~2× ALU win on Adreno **[inferred]**, but the shading chain is
   *integer* index math (`int(uoz*z*256)`, LUT indices, the packed alpha lane). fp16 would break it in
   ways the harness would catch but a shader author would not predict. Low priority.

---

## 5. The big lever — and an earlier draft of this section was too pessimistic

> **Read §5a first.** The rest of §5 describes decoding textures to **colour**, which is genuinely
> hard and genuinely lossy. That framing was wrong, or at least wrongly narrow: it assumed hardware
> sampling means sampling decoded colour. It does not have to, and the alternative is much cheaper and
> much more accurate. §5 is kept because its analysis of the *colour* route is still correct — it is
> just no longer the route to take.

### 5a. What to actually do: hardware-filter the INDICES, not the colours

Checked against `poly.frag` on 2026-07-26 **[measured — read off the shader, not inferred]**:

- `get_texel` returns `texel & 0x0f` — **a 4-bit index**, promoted to 8 bits (`15 << 4 = 0xf0`).
- `LERP(x,y,a) = x + (((y-x)*a) >> 8)` — an 8-bit fractional weight.
- The shading tail is `t → lumaram[lumabase + (t>>1)] → × poly.luma/256 → clamp 0x3f →
  colorxlat[component ramp][luma] → RGB`.

**So the texture is pure luminance. Every bit of colour arrives afterwards, from `palcolor`.**

That means the settled rule *"filtering must be done by hand in the shader"* is correct about
**colour** and too strong in general. Store the 4-bit indices in an **R8 image** and let the sampler
bilinear *those*: that **is** index-space filtering, performed by the texture unit, computing the same
arithmetic `fetch_bilinear_texel` does by hand. `lumaram`, the luma multiply, the clamp and
`colorxlat` all stay per-fragment and bit-exact.

Consequences, and they are large:

- ~16 SSBO loads collapse to 1–2 sampler taps.
- **The cache key needs neither `colorbase`/`palcolor` nor `lumabase`**, because no LUT is applied at
  decode time. The atlas-plus-cache problem in §5 below **evaporates**: what remains is a *format
  conversion* — unswizzle 4bpp into linear R8 when texture RAM dirties — which is a far smaller job.

**What genuinely cannot be done by a sampler, and it is only one thing:** the transparency neighbour
rule, `if (tex00 == 0x000000f0u) tex00 = tex01 & 0xffu;` — a data-dependent conditional that stops a
transparent texel dragging colour out of the transparent region (without it, dark halos on cutout
edges). It exists **only in `fetch_bilinear_texel<true>`; the opaque specialisation has no alpha
handling at all.** So:

- **opaque textured → hardware sampler, essentially exact;**
- **translucent textured → keep the hand-written path** (or approximate with a dilate at decode time).

That boundary is the *same* specialisation split recommended in §4.1 for early-Z, so one change serves
both — and it mirrors how MAME reaches these, by template parameter.

Three smaller things, all solvable: keep MAME's `fast_log2` LOD selection and use `textureLod()` with
an explicit level plus a manual blend, so selection stays exact; upload Model 2's **own** resident mip
levels explicitly rather than generating them, or the chain will not match; and handle wrap/mirror
inside the page sub-rect (`texx/texy/texwidth/texheight`) either by blitting each page to its own
image — at which point `MIRRORED_REPEAT` works directly — or with manual wrap math and `CLAMP`.

**One thing to check before committing to this:** MAME's LERP uses 8 fractional bits, so hardware
filtering matches only if **`subTexelPrecisionBits` ≥ 8**. The Vulkan minimum is 4 and it is *not* in
the limits currently recorded in [vulkan-target.md](vulkan-target.md). If it is below 8 the opaque
path stops being exact and becomes merely close. **Probe it first.**

### 5b. The colour-decode route, which is what not to do

Decode texture pages **through the LUTs** into real `VkImage`s, then sample with a real sampler, real
mips and hardware trilinear. That collapses ~16 SSBO loads to 2 sampler taps and puts the texture unit
and its cache to work. It is what essentially every HLE emulator renderer does.

**The catch: the LUTs are per-polygon, not global.** `lumabase` (`texheader[1] & 0xff) << 7`) and
`colorbase` (`texheader[3] >> 6) & 0x3ff`) vary per polygon, and `palcolor` is resolved per polygon at
submit. So **the same texels shade differently depending on which polygon reads them** — you cannot
decode a page once and be done. The cache has to be keyed on (page, `lumabase`, `colorbase`,
texparams), with eviction, which is an **atlas plus a cache**.

`p3-hw-geometry.md` explicitly rejected exactly that: *"texture RAM is 2 MB total and wants no atlas
or cache"*. **That decision was right for its constraints** — at 2 MB total and 1× native there was
genuinely nothing to gain, and the mip chain is already resident at shifted addresses.

**The constraints changed.** Reopen it deliberately, in the Polydiver doc, with the reason recorded —
not quietly, and not by discovering the old note and assuming it is still current.

The accuracy cost is precisely "filtering after the LUTs instead of before", which is the difference
step 4 attributed the whole colour residual to: one or two steps along a ramp. At VR viewing distance
on an upscaled screen it is very unlikely to be visible — but that is a claim the step-7 harness can
now *price* rather than argue about (§8).

---

## 6. The thing that could make all of the above irrelevant

**The CPU may be the wall, not the renderer.** Model 2 is two i960s plus the TGP/copro DSPs plus a
68000 for sound, and the emulation thread runs them.

~~**Unknown, and not verified: whether MAME recompiles or interprets the i960.**~~ **ANSWERED
2026-07-26 (P3 step 8) — MAME INTERPRETS the i960. There is no recompiler.** [measured]
`src/devices/cpu/i960/` contains exactly two things, `i960.cpp` and `i960dis.cpp`: the core and a
disassembler. There is **no `i960fe.cpp`** — the frontend file every DRC core in MAME has — and
`i960.cpp` contains **zero** references to `drcuml`, against four in `sh/sh2.cpp`, which is a DRC core
in the same tree. `execute_run()` ([i960.cpp:2204](../src/devices/cpu/i960/i960.cpp#L2204)) is a plain
`while (m_icount > 0)` fetch-decode-execute loop with a `debugger_instruction_hook` per instruction.

**So §4 optimises the wrong half if the CPU is the wall.** Two i960s interpreted, plus the copro DSPs
and a 68000, all on the emulation thread, and none of it is touched by anything in this document.
Renderer work is still worth doing — but the first Quest 3 measurement to take is the **emulation
thread alone**, which `M2VK_NO_3D=1` already isolates on both renderers, before any GPU-side effort is
funded. **This does not change the ranking in §4; it changes how much §4 is worth**, and that is a
different question that needs the number in §2 that has never been taken.

**The good news is already banked.** `p3-hw-geometry.md`: the software rasterizer *"is where nearly
all of MAME's Model 2 CPU time goes"*, and P3 switched it off at the seam. **The largest single CPU win
available in this project has already landed.** That is a genuine reason for optimism about the port
that the 105 % figure obscures rather than supports.

**Rough sizing** **[inferred]**: an XR2 Gen 2 big core is realistically 3–4× slower single-threaded
than an M5 P-core on interpreter-style code. Take the desktop unthrottled multiple from §2, divide by
~3.5, and that is the first estimate of whether it fits at all.

### 6.1 ✅ CONFIRMED on Quest silicon — the per-device split, daytona heavy race (2026-08-31) [measured]

The `PROFILER=1` core, run on the Quest 3 under RetroArch's Vulkan driver with the clock pinned and
driven into a sustained full-grid race, settled the §6 hypothesis with real numbers. Ranking (profiled,
so read the order, not the absolute — the profiler roughly doubles frame time; full table and method in
`retroarch-quest-perf.md` §4.1):

`:maincpu` 12 % ≈ `:m1audio:sndcpu` 12 % > `:copro_tgp` 8–9 % ≈ `:ioboard:iocpu` 8 % > `:drivecpu` 6 %,
against `Video Update` 2 % / `Sound Generation` 1 %.

- **§1/§6 confirmed: the load is on the interpreted CPUs, not the renderer.** The GPU-side buckets are
  2–1 %; the five interpreted CPUs are the frame.
- **The sound 68000 is co-largest and load-independent** — 12 % in both attract and the heavy race,
  while `:maincpu` eased 13→12 and `:copro_tgp` climbed 0→9 under geometry load. A fixed cost on the
  critical path → the first lever (thread it, `m1audio-thread-plan.md`) aims at the right device.
- **`:drivecpu` is a clean 6 %** of pure force-feedback we never use on a pad → a candidate second lever.
- **Corroborated across four titles the same session** — Sega Rally, Motor Raid and Dynamite Cop
  (Dynamite Deka 2) all give the same shape (sound 68000 tagged `:audiocpu` on these). The sound CPU is
  the **outright largest device in two of the four** (Motor Raid 17–18 %, Dynamite Cop 18 %, both ahead
  of `:maincpu`) and never below #2 — and Dynamite Cop is a *brawler*, so this is a Model-2-wide
  property, not driving-specific. `:drivecpu` appears only on the wheel cabs (6 % daytona, 8–9 % Sega
  Rally), absent on the bike and the brawler. Table in `retroarch-quest-perf.md` §4.1.

---

## 7. Measurement hygiene — a new gotcha, and it belongs in CLAUDE.md's list

**Concurrent sessions in the same tree silently corrupt wall-clock measurements.** Everything the
step-7 harness measures — coverage, colour agreement, SSIM, digests — is deterministic pixel output
and is **immune**. A timing number is not, and there is no way to tell a contended result from a real
one after the fact.

Observed 2026-07-26 while preparing to measure §2: another session running `ab.sh` loops
(`lastbrnx` 2300 frames, then `vstriker` 1500) at 86–183 % CPU **[measured]**.

- **Build-tree collision is the related risk.** Both sessions run `./model2_libretro.dylib`. A `make`
  in one swaps the binary underneath the other's in-flight loop, and nothing in either log would say
  so. Check `ps` for `retrohost`/`RetroArch`/`make` before building or timing.
- **`M2_SAVE_DIR` per run (gotcha 7) does *not* cover this.** That hazard is about NVRAM crossing and
  both harnesses already handle it. This is a different failure.
- **Protocol for any timing number: 3 repeats per game, report the spread.** A performance figure with
  no variance estimate cannot distinguish contention from signal.

**A loose end this does *not* explain, corrected within hours of being written here.** An earlier
draft of this section offered concurrent harness activity as the lead on the anomalous
non-reproducing `schamp` digest from the step-6 worklog (`b3c2896438f248d0` once, then eleven runs of
`8f1abfef0c4f9bed`). **That was wrong.** Step 7 reproduced the *phenomenon* on `lastbrnx`, which is
bistable between two digests 11.26 % of a frame apart — **under `renderer=software` with
`M2VK_NO_3D=1`, where none of our code runs at all.** It is almost certainly `draw_framebuffer`'s
`frame_number() & 1`. Not ours, not concurrency, and not the renderer.

The hygiene point above stands on its own — contention really does corrupt wall-clock numbers, which
is why §2's measurement was deferred. But it is not a universal explanation for surprising results,
and reaching for it was the wrong instinct. **The rule that actually generalises: re-run a one-off
disagreement before believing it, and before theorising about it.** `ab.sh` now says so in its own
failure message.

---

## 8. Where each item goes in the plan

**P3 is DONE — all 8 steps, `c38dbbefffe`.** The stale-3D fix this section used to queue ahead of step
8 landed inside it, counter included: `dupes` / `empty` / `dropped`, where `empty` is the inverted
check that catches the bug coming back. Nothing here depended on it.

✅ **The performance harness landed on 2026-07-26** — `devnotes/perf.sh` + `M2VK_HOST_PERF` in
`retrohost.c`. §2a is its output. It was the assigned step-8 item that P3 shipped without; it is now
built and the numbers reorder this document rather than merely filling a hole in it.

| item | phase | why there |
|---|---|---|
| ~~**i960 interpret-vs-recompile check** (§6)~~ | **DONE, P3 step 8** | **It interprets.** See §6 — no DRC, no frontend file, no `drcuml` |
| ~~**A perf mode for the harness**~~ | **DONE, 2026-07-26** | `devnotes/perf.sh`: 3 configs × 3 repeats with the spread, `emul`/`sw`/`vk`, per-bucket means so the boot plateau is visible rather than averaged in. Output and consequences in §2a. **GPU timestamps were deliberately not built** — see the row below |
| ~~**GPU timestamps for a per-stage breakdown**~~ | **DEFERRED, with a reason** | The breakdown exists without them: differential switching (`M2VK_NO_3D=1` / `M2VK_FORCE_SOLID=2` / full) attributes the GPU frame to 0.05 ms and needed no core change, no query pool and no risk to the accuracy harness. See the box above §3.1. Timestamps would refine *below* 0.05 ms, on a quantity that is 10 % of the frame — i.e. below the noise of something that already does not matter here. **Revisit on Quest 3**, where the GPU's share is expected to be the interesting one |
| **Pipeline split for early-Z** (§4.1) | **P4** | P4 is interpolated depth and decals, so it is already opening the depth pipeline. Doing the `discard` split at the same time avoids verifying the depth path twice, and P4 is also where `vkCmdSetDepthBias` finally becomes legal. ⚠️ **The speed argument for it is dead** (§2a) — the "verify the depth path once" argument is what keeps it here |
| ~~**Dirty-range the upload** (§3.3)~~ | **NOT ON DESKTOP; Quest 3 only** | Measured at **~0** here: the renderer's entire CPU side, 2 MB memcpy included, is 0.15–0.20 ms/frame (§2a finding 3). Do it for Adreno's bandwidth budget or not at all |
| **Filtering / microtexture core options** (§4.3–4) | **P5 for the switches, P6 for the defaults** | Cheap — the per-polygon flags exist. P6 is where core options get their final shape and any per-game defaults live. Worth ≤ 9 % of the frame here (the box above §3.1); the case for them is Quest 3 and accuracy/speed choice, not desktop speed |
| **Texture cache + hardware sampling** (§5) | **a prerequisite for P5 on Quest 3, not an optional fast path** | See §9. Its desktop ceiling is now known: 0.388 ms of a 4.31 ms frame on `waverunr` |
| **The Quest 3 port itself** | **currently unphased — and §2a promotes it** | P0–P6 does not have a target-port phase. It is now the *only* place the remaining optimisation questions can be answered, which makes it the blocker on §4 rather than a sequel to it. Needs raising in the Polydiver doc |

---

## 9. What P5 already knew, and the fourth thing it did not

`p3-hw-geometry.md` establishes that internal-resolution scaling is architecturally **unblocked** —
draw-order depth does not care, `noperspective` interpolation is correct finer sampling, and
per-polygon scissor scales by the same factor. It already flags three shader-side costs:

1. **Mip selection is resolution-blind** — `mml` needs a bias derived from `fast_log2`'s fixed point
   (do not guess it, and do not use `maxSamplerLodBias`, which is irrelevant to hand-written filtering).
2. **The `checker` stipple gets finer with S**, turning a 50 % screen door into a grey dither.
3. **Scaling must be a runtime option**, because the A/B harness only means anything at 1×.

**The fourth, which is new: on Quest 3 it may not be affordable at all.** Fragment cost is where the
whole budget sits, and 4× internal res is ~16× the fragments times ~16 manual fetches each. 2× is
probably the realistic ceiling unless §5 lands first. Architecturally unblocked; economically not.

---

## 10. Open questions

1. ~~Does MAME interpret or recompile the i960?~~ **Answered: it interprets.** (§6)
2. ~~What is the unthrottled `--vk` speed, per fixture, with a spread?~~ **Answered: §2a.**
   364–551 % pipelined, within 3.5–5 % of the emulation-only ceiling on all six fixtures.
3. ~~What does the frame actually attribute to?~~ **Answered by differential switching rather than by
   timestamps** — the box above §3.1. GPU 0.71–0.94 ms of a 3.16–4.77 ms frame, of which the filtering
   is 87 % of the 3D layer's share and the rasterisation is 0.056 ms.
4. How much does early-Z recovery actually buy, given the stream is already front-to-back? (§3.2)
   **Now bounded: at most the 0.388 ms the filtering costs on `waverunr`, i.e. ≤ 9 % of the frame** —
   so the honest answer on desktop is "not enough to matter", and the question moves to Quest 3.
5. What does the step-7 harness score for a hardware-sampled texture path — i.e. what is the real
   accuracy price of §5, in SSIM over the covered region?
6. Is the Quest 3 screen presented as one view, or does anything about the Unity composition double
   the fragment cost?
7. Does the Quest 3 build want `renderer=software` at all? At 3–4× slower CPU it is unlikely to be
   usable, which would make the A/B ground truth a desktop-only activity. **§2a narrows this: `sw` is
   only 20–28 % behind `vk` on desktop, so if the CPU budget were the only issue it might survive —
   but the emulation thread alone already needs 4.2 ms/frame here.**

**The new first question, which did not exist before §2a:** what does the *emulation thread* cost on
an XR2 Gen 2, and is 17.4 ms/frame reachable at all with two interpreted i960s? On this machine it is
3.0–4.8 ms. That is the number the port lives or dies by, and no amount of renderer work moves it.
