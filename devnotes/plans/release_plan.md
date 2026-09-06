# release_plan.md — packaging Modelizer for its own public repo

**Written 2026-08-29.** How the core leaves this `mame-model2-vk` working tree and becomes two
shippable things: the public **`modelizer`** core, and a **`modelizer-aoj`** build made for the
Age of Joy VR arcade. This is the *packaging/distribution* plan; the renderer/input/option work that
makes the core presentable is the R-series in [shippable-plan.md](../plan_finished/shippable-plan.md), absorbed by
[modelizer-plan.md](modelizer-plan.md) — read those for the code queue. This file is only the repo
+ variant + release mechanics.

## The four decisions (settled 2026-08-29)

1. **One repo, not two, not a self-fork.** `modelizer` and `modelizer-aoj` live in **one source
   tree**. `-aoj` is a **build variant**, not a forked repo and not a long-lived branch.
2. **Keep the mame0288 fork lineage.** The new `modelizer` repo carries this tree's history from the
   `mame0288` tag — honest GPL provenance, and `upstream = mamedev/mame` stays wired for monthly
   merges.
3. **AoJ consumes a prebuilt binary.** Age of Joy pulls a compiled `-aoj` `.so` from a GitHub
   Release by tag. It does **not** build the core from source.
4. **The `-aoj` delta is UNKNOWN until integration-tested.** See the box below — this is the whole
   reason the variant is a thin seam and not a spec.

### Why the flycast-aoj precedent does not transfer literally
`flycast-aoj` is a fork **only because Flycast is not the user's** — a long-lived `age-of-joy` branch
is the only tool you have to carry a delta on top of someone else's tree. Modelizer **is** upstream,
so that constraint does not exist; forking your own project to carry a thin delta just means
cherry-picking every renderer fix across two trees forever. And the flycast-aoj delta turned out to be
**exactly one change** — an AICA audio-timing throttle that only bit *in-process inside the AoJ app*.
That is the shape to expect here too: a small integration-seam fix, discovered by testing, not a
different product.

## 🚧 The `-aoj` delta is discovered by testing, not specified up front
**We do not yet know what `modelizer-aoj` must differ in.** Do not invent options for it. The known
*candidate* categories, from the flycast-aoj experience + a VR-arcade context, are:
- **In-process-frontend quirks** — the flycast-aoj bug was precisely this class (audio init race when
  run in-process). The single most likely thing to actually need code.
- **Locked / hidden core options** — VR-appropriate defaults baked in, menu knobs pinned so a cabinet
  player can't wander into `software` renderer or a broken resolution.
- **VR input mapping defaults** — whatever the AoJ controller layer expects.
- **Branding** — `library_name = "modelizer-aoj"`.

Leave a **seam** for these (a `MODELIZER_AOJ` build define, one place options get overridden), fill it
**after** the core runs inside Age of Joy. Anything written here before that test is a guess.

## Phases

### Phase 0 — Naming (do first; touches the most files)
Folded into [modelizer-plan.md](modelizer-plan.md)'s M2 (the rename to "Modelizer"). For release:
- Repo → `modelizer`. Core `library_name`: `m2-vk` → `modelizer`
  ([retro_entry.cpp:470](../../src/osd/libretro_m2/retro_entry.cpp#L470)); variant → `modelizer-aoj`.
- Settle the RetroArch **display name** for a core that plays three families (open in
  shippable-plan R4 — decide before the release build).

### Phase 1 — Stand up the `modelizer` repo
- New GitHub repo `modelizer`; push current `main` (descends from `mame0288`, history rides along).
  Keep `upstream = mamedev/mame`.
- 🚨 **Guard the local-ignore hack across the push to a PUBLIC remote.** `CLAUDE.md` + `devnotes/`
  are skip-worktree / never indexed, so a push carries nothing — but run the three `CLAUDE.md` checks
  (see CLAUDE.md's local-ignore section) **before the first push**, because this remote is public.
- Legal drop per [legalstuff.md](../reference/legalstuff.md) §9: `COPYING` + `docs/legal/`, declare
  GPL-2.0-or-later, tag the building commit, ship the **core only** (not the standalone). Re-run the
  audit after the name change.
- **README — the user's to write.** Flagged, never drafted (legalstuff §5.2/§9).

### Phase 2 — The `-aoj` build variant (same tree)
- Add a `MODELIZER_AOJ` build define that emits a second core artifact. Since AoJ takes a prebuilt
  binary, this is **compile-time**, not a branch.
- Wire the seam only: branded `library_name`, one option-override hook, one input-defaults hook —
  all no-ops until Phase 2b fills them.
- **Phase 2b (blocked on AoJ integration test):** run `modelizer-aoj` inside Age of Joy, find what
  actually breaks/needs locking, fill the seam. This is where the real `-aoj` work happens.

### Phase 3 — Release engineering
- Build both cores from **one tagged commit**: `modelizer` (desktop `.dylib` + Android `.so`) and
  `modelizer-aoj` (VR/Android `.so`). `build-android.sh` already exists — extend it to emit the
  variant.
- GitHub Releases carry the prebuilt binaries; AoJ pulls the `-aoj` `.so` by tag.

### Phase 4 — Presentability
The **R-series** ([shippable-plan.md](../plan_finished/shippable-plan.md) / [modelizer-plan.md](modelizer-plan.md)) is
the visible-polish work — R1 input mapping, R3 the S22 option set, R2 compat matrix. That gates a
credible public core. Reference, don't duplicate.

## Critical path
Phase 0 (naming) → Phase 1 (repo + legal). Phase 4 (R-series polish) runs in parallel. Phase 2's seam
can land early, but **Phase 2b waits on the AoJ integration test**, and Phase 3 is last.
