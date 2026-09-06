# shipping-repo.md — standing up the public `modelizer-aoj` repository

**Written 2026-09-03.** How the core leaves this private `m2-vk` working tree and becomes a **clean,
public repository whose sole job is to package the libretro core for Age of Joy** — the direct
analogue of `flycast-aoj`. This is the *repository + history + release-engineering* plan. It refines
[release_plan.md](release_plan.md) (which is still the source of truth for the four settled
distribution decisions and the `-aoj` build-variant seam) and defers all renderer/input/option work to
[modelizer-plan.md](modelizer-plan.md) / [shippable-plan.md](../plan_finished/shippable-plan.md). Legal facts come from
[legalstuff.md](../reference/legalstuff.md); the Android artifact is described in [android.md](../reference/android.md).

> **Relationship to `release_plan.md` (read this first).** `release_plan.md` decided *one repo named
> `modelizer`, with `-aoj` as a compile-time build variant in the same tree.* This plan sharpens that
> to the **near-term reality**: Age of Joy is the *only* consumer we have (README.md: "This mainly
> exists so we can run MAME 3D games inside Age of Joy"), so the first public repo we stand up is the
> AoJ-packaging one. Nothing here contradicts `release_plan.md`'s four decisions — the public repo is
> still a single MAME fork carrying the mame0289 lineage (decision 2), `-aoj` is still a build variant
> not a self-fork (decision 1), and AoJ still consumes a prebuilt binary by tag (decision 3). The only
> refinement is **naming the first public repo for its purpose** rather than shipping a generic
> `modelizer` repo before there is a second consumer to justify the generic name. Whether the public
> repo is called `modelizer` or `modelizer-aoj` is an open question for the user (§8); the mechanics
> below are identical either way.

---

## 1. Purpose & scope

Stand up a **minimal, public repository** that produces the **exact same libretro core binary this
dev tree builds**, existing so **Age of Joy can bundle it with a clean licence and provenance story**.
This mirrors `flycast-aoj`: a public repo that exists *only* to package a known-good emulator build for
use inside the "Age of Joy" Unity VR arcade frontend. This dev repo (`m2-vk`) stays private and messy —
it carries `CLAUDE.md`, all of `devnotes/`, the harness, the ROMs, and the working history. The public
repo is the **clean public face**: it carries our source, the build scripts, the licence drop, CI, and
a human-written README, and **nothing else**.

The shippable artifact is the Android arm64 core `libmodelizer_libretro_android.so` (per
[android.md](../reference/android.md); desktop `.dll`/`.dylib`/`.so` as a bonus if cheap). Age of Joy loads it by
dropping the `.so` into its `\cores\` directory — the core name is derived from the filename up to
`_libretro_android.so`, so the on-disk name **is** the AoJ core id, and AoJ core options are supplied
by an optional YAML in `\configuration\cores\` (see §6.4).

---

## 2. The hard architectural question — full fork vs. overlay

Our core is **not a small standalone**. It only builds inside the full MAME tree: it links
`libemu`/`libmame`/`libutil`/the device libraries, the whole i960/MB86233/68000/Z80 CPU cores, SCSP
sound, etc. Everything *we* wrote is new files under `src/osd/libretro_m2/` plus a **small, measured**
set of guarded hook calls in upstream files (measured 2026-09-01 vs `mame0289`: **906 insertions / 18
deletions across 15 files** — `git diff --shortstat mame0289 -- src/devices src/mame`; the whole of
`src/osd/libretro_m2/` is new files not counted by that path filter). So "just our code" can **never**
mean shipping a handful of extracted files — the delta does not build on its own.

Two realistic shapes for the public repo:

### Option (a) — a full MAME fork, pared to the modelizer subtarget *(RECOMMENDED)*
The public repo **is** a fork of `mamedev/mame` anchored at a release tag (`mame0289`), carrying our
`src/osd/libretro_m2/` + the guarded hooks, the build scripts, the legal drop, CI, and a clean
AoJ-focused README. Upstream MAME's own history and per-file attribution ride along (it is a GPL fork,
so this is correct and required where the licence reaches). "Pared to the modelizer subtarget" means
the *default build* is `SUBTARGET=modelizer OSD=libretro_m2` and the README/branding are AoJ-facing —
**not** that MAME source is deleted (it can't be; the core needs it to link).

This is exactly what `flycast-aoj` is: a full fork of `flyinghead/flycast` carrying a thin
Age-of-Joy delta. `flycast-aoj`'s entire delta turned out to be **one change** (an AICA audio-timing
throttle that only bit in-process inside the AoJ app) — strong evidence that the fork-carries-the-whole-
tree shape is the right one and the AoJ-specific delta is tiny.

**Trade-offs:**
- ✅ **Identical-binary guarantee is trivial** — same tree, same tag, same toolchain → same bytes (§5).
- ✅ **Cleanest GPL provenance** — upstream MAME history/attribution is intact; a GitHub source
  tarball of the tag corresponds exactly to the binary (satisfies GPL/LGPL "corresponding source",
  legalstuff §4.2/§3).
- ✅ **Self-contained and reproducible** — no build-time assembly step that can drift.
- ✅ **Matches the established precedent** (`flycast-aoj`) and `release_plan.md` decision 2.
- ⚠️ **Large repo** — the full MAME tree. Acceptable; it is what every MAME derivative ships.
- ⚠️ **Monthly-merge cadence touches two remotes** — but the public repo simply *is* `main` pushed to
  a public origin, so a merge here is a push there. No extra tree to maintain.

### Option (b) — an overlay / patch-set applied at build time
The public repo carries *only* our delta (`src/osd/libretro_m2/` + a patch file for the guarded hooks
+ build scripts + CI). CI clones `mamedev/mame` at a pinned tag, copies/patches our files in, and builds.

**Trade-offs:**
- ✅ Small repo (just our ~15 touched files + new dir).
- ❌ **Identical-binary guarantee is fragile** — the binary now depends on an *external* checkout
  being pinned byte-for-byte; a shallow-clone or tag-repoint upstream silently changes the output.
- ❌ **Provenance is murkier** — a patch-set does not obviously credit MAME's authors, and a source
  tarball of *our* repo is **not** the corresponding source for the binary (a GPL problem for us and
  for AoJ, which redistributes the binary).
- ❌ **Two-step assembly** is a standing source of build breakage against upstream reorganizations.
- ❌ **No precedent** — `flycast-aoj` did not do this.

### Recommendation
**Option (a): a full MAME fork, pared to the modelizer subtarget.** It is the only shape that makes the
identical-binary and corresponding-source guarantees cheap and honest, it matches the `flycast-aoj`
precedent, and it is already what `release_plan.md` decision 2 chose. The "large repo" cost is the
normal cost of every public MAME derivative and buys everything else. Do **not** pursue the overlay —
it trades a one-time repo-size cost for permanent build fragility and a real GPL corresponding-source
gap.

---

## 3. Exactly what goes in the public repo vs. what is stripped

### Goes in
- **The full MAME source tree** at the anchored tag (needed to build; carries upstream attribution).
- **`src/osd/libretro_m2/`** — all our OSD/renderer/core files (25 new files per legalstuff §2).
- **The guarded hooks** in upstream files (`scsp.cpp`/`scsp.h`, `model2.cpp`/`model2_v.cpp`/`model2.flt`,
  `osdlib_unix.cpp`, `scripts/src/main.lua`, `scripts/src/osd/libretro_m2.lua`, `scripts/toolchain.lua`,
  etc. — the 15 files the `mame0289` diff names, all either new or guarded on `OSD_LIBRETRO_M2`).
- **Build scripts renamed/de-branded** — the equivalents of `devnotes/build-android.sh` and
  `devnotes/deploy-android.sh`, moved out of `devnotes/` (which does not ship) into a committed
  location (e.g. `scripts/aoj/` or repo root), stripped of `M2VK_*`/internal nomenclature in comments.
- **Legal drop** (legalstuff §9): `COPYING` (already in tree) and `docs/legal/` (already in tree — the
  eight licence texts). Declare **GPL-2.0-or-later**.
- **CI** — a GitHub Actions workflow that builds the Android `.so` (and desktop if cheap) from the
  pinned toolchain and publishes it as a **release artifact** (§5).
- **A human-written README** (legalstuff §5.2) — what this is, that it is derived from MAME @
  `mame0289`, the trademark disclaimer, not-affiliated-with-MAMEdev, bugs-here-not-MAME-Testers, and
  the **AI-use disclaimer prose** (§4). This is the *one* piece of new public-facing prose the release
  needs.

### Stripped (must NOT appear in the public repo)
- **`devnotes/`** — the entire directory. Local-only; never ships.
- **`CLAUDE.md`** — local-only; never ships. (Check by name — it is easy to miss; see legalstuff §7.)
- **ROMs** — none are tracked here and none go in (`roms/` is `dir.txt` only; legalstuff §7). Do **not**
  link to a ROM from the release page.
- **Any internal / AI nomenclature anywhere machine-readable** — see §4. This includes `M2VK_*` and
  `m2-vk`/`Model 2`-only branding where the AoJ core wants its own name (`library_name`; §6.3).
- **The standalone build** as a *release artifact* — ship the **core only**. The standalone
  `mamemodelizer` links Apache-2.0 `3rdparty/astc-encoder` via bgfx, which the core stubs out; keeping
  the standalone out of releases keeps that licence question dead (legalstuff §6, §9).

---

## 4. Git history & authorship policy — the critical section

The public repo must contain **no AI/Claude nomenclature anywhere machine-readable** — not in commit
messages, **not in commit author or committer name/email**, not in branch names, not in code comments,
not in file names, not in PR/issue text, not in tags or release notes. Commits are authored **as the
human git user only** (`mcwild77 <shonuffeb@gmail.com>`), with plain, human commit messages. There is
**no `Co-Authored-By: Claude` trailer** and no "Generated with Claude Code" footer — ever.

**The one deliberate exception is prose in the README.** The user wants an honest AI-use disclaimer
there (transparency), so the README *will* state that AI assistance was used in development. That is
the **only** place AI is mentioned, and it is human-authored prose in a file, **not** machine-readable
provenance metadata. The reasoning the user gave: public projects that visibly use AI in their
*machine-readable* history have been targeted and harassed, so keep AI out of the author/committer
metadata and the log while being honest in the README. The disclaimer being *in prose* and the
metadata being *clean* are not in tension — they are two separate surfaces with two separate rules.

### The history recipe (produces clean history; guarantees no AI strings; credits MAME correctly)

Our delta is small and lives mostly in new files, so the robust approach is to **rebuild the public
history from upstream MAME's real history plus a handful of fresh, human-authored commits** — never by
cherry-picking or pushing this dev repo's own commits (whose messages/metadata may carry internal or AI
strings from before the hygiene rules, and which are not worth auditing commit-by-commit).

1. **Start from upstream's genuine history.** Clone `mamedev/mame`; the base commit of the public repo
   is the real `mame0289` tag commit. This preserves **all** upstream authorship exactly — which is
   correct GPL provenance and satisfies "corresponding source", and costs us nothing because it is
   upstream's own work under their own names.
2. **Lay our delta on top as clean commits.** Copy our files (the new `src/osd/libretro_m2/` tree, the
   de-branded build scripts, the legal drop, CI) and apply the guarded hooks into the checked-out
   `mame0289` tree from this dev repo's **working tree** (not via `git cherry-pick`/`git am`, which
   carry metadata). Commit as a **small number of logical, human-authored commits** —
   `git -c user.name='mcwild77' -c user.email='shonuffeb@gmail.com' commit` with plain messages
   (e.g. "Add libretro OSD with Vulkan renderer for Sega Model 2 / Namco System 22 / System 21",
   "Add Android build scripts", "Add legal texts and README"). None of this dev repo's history rides
   along, so nothing from it can leak.
3. **Keep `upstream = mamedev/mame` wired** for the monthly release-tag merges (CLAUDE.md convention).
   Our own release tags use the project prefix (`m2vk-v0.1` today, or a new `-aoj` prefix — §8) so they
   never collide with upstream `mame*` tags.

### The pre-push audit (run before the FIRST push, and before every push after a merge)
No AI string may survive in anything machine-readable. Run all of these and require empty output
(the README disclaimer is expected prose and is not matched by the log/metadata checks):

```sh
# 1. No AI / internal strings in any commit message, author, OR committer (all history):
git log --all --format='%an%n%ae%n%cn%n%ce%n%s%n%b' \
  | grep -iE 'claude|anthropic|co-authored-by|generated with|\bA\.?I\b' && echo 'LEAK' || echo ok

# 2. No AI strings anywhere in the working tree (README disclaimer is the sole allowed mention;
#    grep it out if it uses one of these words):
grep -rInE 'claude|anthropic' . --exclude-dir=.git && echo 'CHECK' || echo ok

# 3. Neither CLAUDE.md nor devnotes/ is tracked or stageable:
git ls-files | grep -iE 'CLAUDE|devnotes' && echo 'LEAK' || echo ok
git add -A --dry-run 2>/dev/null | grep -iE 'CLAUDE|devnotes' && echo 'LEAK' || echo ok

# 4. No ROMs tracked (legalstuff §7):
git ls-files | grep -iE '\.(zip|7z|chd|rom)$' && echo 'CHECK' || echo ok

# 5. Every commit's author AND committer is the human (spot the odd one out):
git log --all --format='%ae | %ce' | sort -u   # must be only shonuffeb@gmail.com
```

Because the public repo is built fresh from upstream + clean commits (not from this dev repo's
history), checks 1/5 pass by construction — but run them anyway; they are the guard that a stray
cherry-pick or a bad `--amend` never slips through. Check 2 is the one that catches a `claude`/internal
string left in a *code comment* or *filename* that would otherwise ship.

---

## 5. The "identical binary" guarantee

Age of Joy redistributes the compiled `.so`; the guarantee we want is that the binary AoJ ships is
bit-for-bit (or at minimum provenance-equivalent) to what this dev tree builds. Option (a) makes this
cheap because the public repo *is* the same tree at a tag. The pins:

- **Pinned upstream tag** — the public repo's base is exactly `mame0289` (or whatever tag we anchor
  and re-audit at). Not `master`, not a moving `main`.
- **Pinned toolchain** — **NDK r27d / clang 18** (android.md §2; `build-android.sh` defaults to r27d
  and refuses to guess). CI installs that exact NDK.
- **Reproducible build flags** — `make SUBTARGET=modelizer OSD=libretro_m2` for desktop; for Android
  the genie invocation in `build-android.sh` (`--osd=libretro_m2 --targetos=android --PLATFORM=arm64`).
  `PLATFORM=arm64` builds the native `drcbearm64` UML back end the Model 2B SHARC needs (NOASM is
  **not** passed as of 2026-09-01 — see the `build-android.sh` PARAMS comment).
- **Shipping defaults** — `PROFILER=0` (the default; the per-device profiler must **not** be in a
  shipping build — `build-android.sh` only adds `--PROFILER=1` when `PROFILER` is set). Committed
  SPIR-V shaders (`--target-env=vulkan1.0`), so no shaderc on the CI host.
- **Tagged releases** — every release is cut from a single tag that builds the artifact (legalstuff
  §4.2). The tag is the corresponding source.
- **CI publishes the artifact** — a GitHub Actions workflow builds the `.so` from the pins above and
  attaches it to the GitHub Release. AoJ pulls the `-aoj` `.so` **by tag** (release_plan.md decision
  3). This closes the loop: the binary AoJ ships is the CI output of the tagged corresponding source,
  reproducible by anyone from the same tag + NDK r27d.

Full bit-for-bit reproducibility across machines additionally needs a stable build path and
`SOURCE_DATE_EPOCH`; if strict determinism is wanted, pin those in CI. For the AoJ use-case,
**provenance-equivalence (same tag + same NDK + published-by-CI)** is the practical bar and is met by
the above; note it as a possible follow-up rather than a blocker.

---

## 6. Licence & trademark checklist for the public repo

Distilled from [legalstuff.md](../reference/legalstuff.md) §9 (re-run legalstuff §8's audit after the name change
and after any upstream merge):

1. **Release under GPL-2.0-or-later** (legalstuff §4.1). Zero GPL-tagged source is linked, but
   `COPYING` declares the whole GPL and BSD-3 + LGPL-2.1+ are compatible — it is the argument-free
   choice.
2. **Ship `COPYING` + `docs/legal/`** in the repo and in any release archive (legalstuff §4.3/§4.4).
   Both already exist in the tree. This also discharges the IJG/libjpeg notice.
3. **Tag the building commit** with a project-prefixed tag (`m2vk-v0.1` / a new `-aoj` prefix), so the
   public tree corresponds to the binary and satisfies GPL/LGPL corresponding-source (legalstuff
   §4.2, §3). The LGPL-2.1+ pair (`mfmhd.cpp`, `rpk.cpp`) is satisfied by the public source outright.
4. **Keep MAME branding out of the product.** `retro_entry.cpp` sets `library_name` (currently
   `"m2-vk"`); for the AoJ repo set it to the AoJ core name (§6.4 — the value must match the shipped
   `.so` filename stem so AoJ's filename-derived core id lines up). Never "MAME Model 2".
5. **Ship the core only, not the standalone** — avoids Apache-2.0 `astc-encoder` going live
   (legalstuff §6, §9).
6. **Trademark — the one open legal item.** Copyright is settled; trademark is not. MAME is a
   registered trademark; the README must carry the disclaimer (not affiliated with / not endorsed by
   MAMEdev, MAME is a trademark of Gregory Ember) and the repo/asset names must avoid the wordmark
   (legalstuff §5). Naming the repo `modelizer-aoj` (or `modelizer`) keeps the wordmark out of the
   name entirely.
7. **No ROM bundled, no ROM linked from the release page** (legalstuff §7).

> **AoJ core-name mechanics (verified against the AoJ docs).** Age of Joy derives the core id from the
> `.so` filename, taking everything before `_libretro_android.so`; the file must **not** be renamed by
> the user. So the shipped filename **is** the AoJ core id, and it should agree with `library_name`.
> AoJ core options go in an optional YAML under `\configuration\cores\` using an
> `environment: { properties: { <key>: <value> } }` shape (the way `mamearcade` needs
> `mame_media_type: rom`). If the core needs any pinned option under AoJ, it ships as that YAML — a
> candidate for the `-aoj` seam (release_plan.md Phase 2b, discovered by integration testing).

---

## 7. Step-by-step setup sequence

An ordered, actionable checklist to stand up the public repo from scratch. Nothing here builds or
creates the repo yet — it is the runbook for when the user says go.

1. **Settle the name** (§8) — `modelizer-aoj` vs `modelizer`; and the core `library_name` / shipped
   `.so` stem, which must agree (§6.4). Blocks steps that write branding.
2. **Finish the de-branding in *this* tree first** (or on a scratch branch): `library_name`, comment
   scrub of `M2VK_*`/internal strings in the files that will ship, build scripts moved out of
   `devnotes/` into a committed path. Easier to audit here than after the import.
3. **Create the empty public GitHub repo** under `mcwild77` with the settled name.
4. **Build the public history** per §4: clone `mamedev/mame`, checkout `mame0289`, copy in our delta
   from this tree's working copy, commit as a few clean human-authored commits. Wire
   `upstream = mamedev/mame`.
5. **Add the legal drop + README + CI**: confirm `COPYING`/`docs/legal/` present, declare
   GPL-2.0-or-later, write the human README (with the AI disclaimer prose), add the Actions workflow
   that builds the Android `.so` on NDK r27d and attaches it to a release.
6. **Run the §4 pre-push audit** — all five checks empty (README disclaimer excepted). This is the
   gate; do not push until it is clean.
7. **First push** to the public remote.
8. **Cut the first tagged release** (`m2vk-v0.1` / `-aoj` prefix) from the building commit; let CI
   build and attach the `.so`; ship `COPYING` + `docs/legal/` in the archive.
9. **Hand Age of Joy the release-by-tag link** and confirm AoJ's `\cores\` drop-in + optional
   `\configuration\cores\` YAML (§6.4). Any breakage found here is the `-aoj` seam work
   (release_plan.md Phase 2b) — fill the seam, re-tag.

---

## 8. Open questions / decisions for the user

1. **Repo name.** `modelizer-aoj` (purpose-named, mirrors `flycast-aoj`, signals it is the AoJ package)
   vs `modelizer` (generic, matches release_plan.md's Phase 0, room for non-AoJ consumers later). The
   mechanics above are identical; only the README framing and `library_name` change. Recommendation:
   `modelizer-aoj` now, since AoJ is the only consumer and the name makes the repo's purpose obvious —
   but this is the user's call and it interacts with the release_plan.md naming phase.
2. **`library_name` / shipped `.so` stem.** Must agree with the AoJ filename-derived core id (§6.4).
   `modelizer` → `modelizer_libretro_android.so` → AoJ core id `modelizer`. Confirm the exact string.
3. **How does AoJ actually consume it — prebuilt `.so` drop-in (confirmed by the AoJ docs) vs. a git
   submodule / build-from-source?** The docs and release_plan.md decision 3 both say prebuilt `.so` by
   tag; confirm AoJ does not want a submodule, and confirm whether AoJ wants the core options YAML
   shipped alongside (§6.4).
4. **Does the `-aoj` build need any code delta yet?** Unknown until the core runs inside Age of Joy
   (release_plan.md Phase 2b — the flycast-aoj precedent was one in-process audio-timing fix). Do not
   invent options up front; leave the `MODELIZER_AOJ` seam and fill it after the integration test.
5. **Desktop artifacts in the same release?** Cheap to add (`.dll`/`.dylib`/`.so`) if there is a
   desktop AoJ or a plain-RetroArch audience; skip if only the Quest matters. User's call.
6. **Strict bit-for-bit reproducibility** (`SOURCE_DATE_EPOCH`, fixed build path) vs.
   provenance-equivalence (same tag + NDK + CI-published). The latter is the practical bar for AoJ; the
   former is a follow-up if wanted (§5).
7. **README authorship** — legalstuff §5.2 flags the fork README as "the user's to write, never
   drafted unasked." This plan specifies *what it must contain* (trademark disclaimer, MAME provenance,
   AI-use disclaimer, bug-report routing) but does not draft it. Confirm the user wants it drafted, or
   writes it themselves.
