# Model 1 — mame0289 upstream sync plan

Fold upstream's Model 1 reworks into our tree. This is the **M1-6 tail** of
[model1plan.md](model1plan.md): the seam/renderer (M1-0…M1-5) is done against the **mame0288**
baseline, and risk 1 there deferred the driver-bug fixes to "a normal `mame0289+` sync before the
compat tail". That sync is this file.

**Target is `mame0289`** — verified 2026-08-29 as the latest upstream release tag (`git ls-remote
--tags upstream` tops out at 0289; already fetched locally at `d0b7160e548`). No 0290+ exists yet;
not worth waiting for. This is a **full monthly-release merge — ~5235 files, ~200k insertions** — not
a Model 1 patch.

## Why do it (the payoff)

Seven Model 1 commits land, priority order:

- **#15649 — repair 2-bit corruption in 315-5711 TGP copro ROM** → **SWA, Wing War, NetMerc render
  correctly.** The headline. These are exactly the games the mame0288 baseline gets *wrong*, and the
  reason model1plan.md kept them out of early testing (first target was `vf`, the one the baseline
  renders right). This sync is what lets them onto the shippable list.
- **#15642 — improved video and timer emulation** + **#15715** (returns the latched count when reading
  a stopped timer — fixes a `vf` hair-physics regression that #15642 itself introduced). **Take both or
  neither.**
- **#15738** skip degenerate moiré direct polygons, **#15712** clip wireframe lines to the viewport,
  **#15597** HUD layering / blink palette / wireframe fixes. Cosmetic — but #15597 touches the exact
  `screen_update_model1` region our 2D composite taps.

## The cost — every hook site sits in reworked code

Upstream `model1_v.cpp` is **+288 / −79** vs mame0288, `model1.h` **+61 / −20**, `model1.cpp`
**+160 / −60** — and our 8 `#ifdef M1VK` hooks in `model1_v.cpp` all land inside reworked hunks.
**Good news:** `quad_t` keeps its exact shape in 0289 (`point_t *p[4]`, `float z`, `int col`; now with
a constructor at `model1.h:84` but identical members), so the **seam's data contract survives** — we
still cross on `q.col` / `q.z` / `q.p[]`. **Bad news:** signature and code-path changes mean this is a
re-derivation, not a mechanical re-apply.

| Our hook (M1VK site) | What upstream changed | Re-work |
|---|---|---|
| 2× in `push_object` — albedo/lumval/bank capture for **No Lighting** (M1-5) | `push_object` gained a `float &old_z` param; **the luma/color path was reworked**; `compute_specular` went `static`→member | **Highest risk.** Re-derive the No-Lighting decode (pre-luma albedo + `color_xlat` LUT) against the new code. Re-spec before re-writing, same discipline as the original M1-5. |
| `draw_quads` — `submit_quad()` per sorted quad | region reworked (moiré #15738) | Re-place; confirm sort/submission order unchanged |
| `screen_update_model1` — 2D under/over capture (2 sites) | HUD layering reworked (#15597) | Confirm the opaque-under band (6/4/2/0) and over band (7/5/3/1) still hold; the composite depends on it |
| `fclip_clip_right`, `push_direct` | reworked hunks | Re-place, re-validate |

## ⚠️ The savestate gap is NOT confirmed-closed by this sync — 🚫 MOOT as of 2026-09-04

**Savestates are disabled core-wide** (`retro_serialize_size` returns 0; see `CLAUDE.md`). This gap no
longer has to be settled and `state.sh vf` is retired — drop step 5 below. Kept for the record.


model1plan.md M1-5 assumed the sync would close the TGP-copro / `gen_fifo` savestate failure
(`state.sh vf` fails: unregistered MB86233 copro / fifo state on the baseline). **The 0289 diff does
not support that claim.** The only fifo-related change is a constructor-arg cleanup
(`GENERIC_FIFO_U32(config, "copro_fifo_in")` — the trailing `0` dropped); **no new `save_item` /
`save_pointer` for the copro appears.** Treat "sync fixes savestates" as **unverified** — re-run
`state.sh vf` after the merge to settle it. It may remain a separate task.

## Blast radius beyond Model 1

Full merge → it also re-touches our other guarded hooks and forces a whole-core re-validation:

- **model2** hooks: `model2.cpp` +45/−62, `model2_v.cpp` +2 (upstream churn on the same files we tap).
- **scsp** audio hooks: `scsp.cpp` +75/−29, `scsp.h` +14/−4.
- **C++20 core change (#afd3f3):** retired `util::endianness` for `std::endian`, stripped save
  privileges from `endianness_t`, moved float utils to `corefloat.h` — can ripple into the OSD.

So post-merge validation is **all four families' A/B**, not just Model 1.

## Phases

1. **Trial-merge, throwaway.** New worktree, `git merge mame0289`, count real conflicts (expect them
   concentrated in the 8 M1 hook sites + model2/scsp hooks + `modelizer.flt` / `modelizer.lua`). Don't
   commit; this is the go/no-go conflict count.
2. **Re-apply the seams, hardest first** — the `push_object` No-Lighting decode against the reworked
   luma path. Then `draw_quads`, `screen_update_model1` 2D bands, the two clip/direct sites. Keep
   everything `#ifdef M1VK`-guarded (mergeability golden rule) — no re-apply should raise the guarded
   footprint materially above M1-5's 89 insertions.
3. **Rebuild** the unified core
   (`make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10`) and **A/B all four
   families** (`ab.sh` across the fixture set). The endianness/C++20 change means Model 2 / S22 / S21
   need re-verification too — this is not a Model-1-only regression check.
4. **Verify the payoff.** Boot `swa`, `wingwar`, `netmerc`; confirm they now render (they are the games
   the baseline gets wrong — this is the only direct test of #15649). Screenshot to
   `devnotes/screenshots/` from `retrohost --vk`.
5. **Settle savestates.** Re-run `state.sh vf`; record whether the gap actually closed, and if not,
   file it as its own task rather than leaving it implied-fixed.
6. **Re-check the M1-6 cosmetic residual.** The ~5555-px vk-vs-sw edge diff (fill-rule / polygon
   silhouette) from M1-5 — re-measure after the moiré/wireframe fixes (#15738/#15712); some of it may
   dissolve.

## Notes / risks

- **Whole-core rebuild + four-family A/B is the real cost**, not the seam re-apply. Budget for it.
- **`compute_specular` static→member** hints the lighting math moved; the original M1-5 finding was
  "specular is `#if 0`'d → accurate to omit" — re-confirm that still holds on the 0289 code before
  trusting the No-Lighting albedo path.
- Keep the No-AI-nomenclature commit rules (CLAUDE.md) on every commit this produces.
- After merge, `mame0288` stays the *baseline anchor* per repo convention; measure the guarded diff
  with `git diff --shortstat mame0289 -- src/devices src/mame` going forward.
