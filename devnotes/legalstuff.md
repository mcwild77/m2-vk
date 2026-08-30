# legalstuff.md — can we ship a binary of this?

**Short answer: yes.** Verified against the tree at `c38dbbefffe` on 2026-07-26, not from memory.
Everything below is [measured] — the commands that produced each figure are in §7 so it can be
re-run after an upstream-tag merge, which is exactly when it will need re-running.

⚠️ **Not legal advice.** This is an audit of what is actually in the repo and what the licenses in it
say. It is written so a future session does not have to redo the scan, and so the four release
chores in §4 do not get skipped.

⚠️ **The copyright question is the easy one and it passes cleanly. The trademark question (§5) is
the one with an actual action attached**, and it is the part most likely to be waved past because it
is not about code.

---

## 1. The finding, in one table

| | |
|---|---|
| Can the binary be redistributed? | **Yes** |
| Under what licence? | **GPL-2.0-or-later** — what [COPYING](../COPYING) says the whole is, and what every other MAME derivative ships as |
| Any GPL-tagged source linked in? | **None.** 0 of 606 resolved objects |
| Any copyleft at all? | 2 files, **LGPL-2.1+**, and the public repo already satisfies them (§3) |
| Any GPL-incompatible third party? | **None in the core.** One Apache-2.0 in the sdl3 build only — §6 |
| Our own new code | 25 files, all **BSD-3-Clause / `copyright-holders:mcwild77`** |
| ROMs tracked in git? | **No.** `roms/` is `dir.txt` and nothing else |
| Blockers | **None.** Four chores in §4, one naming decision in §5 |

---

## 2. What is actually linked, by licence

606 of the 740 `src/` objects in `build/` resolve to a `.cpp`/`.c`/`.mm` on disk. Their `// license:`
tags:

| Licence | Files | Notes |
|---|---|---|
| **BSD-3-Clause** | **585** | 579 tagged `license:BSD-3-Clause`, 6 tagged `license: BSD-3-Clause` (space after the colon — Dirk Best's files; the tally script has to allow for it) |
| *(untagged)* | 17 | 15 are **generated** m68000 files (`m68kmake.py` output from BSD-3 Musashi), plus `disasmintf.cpp` and `nanosvg.cpp` (zlib) |
| **LGPL-2.1+** | 2 | `devices/imagedev/mfmhd.cpp`, `lib/formats/rpk.cpp` — TI-99 hardware, pulled in by the generic device/format libs, not by anything Model 2 |
| **Public domain** | 1 | `lib/util/md5.cpp` |
| **MIT** | 1 | `lib/util/path_to_regex.cpp` |

**Zero GPL-2.0-tagged source files.** That is the headline and it is worth restating because it is
counter-intuitive: MAME *as a whole* is GPL-2.0+, but that is because the union of all drivers
contains GPL files — this subtarget links none of them. It does not change what we should release
under (§4.1), it just means there is no copyleft obligation hiding in a driver.

The remaining 134 unresolved objects are third-party (§6) and generated sources with no matching
file at the derived path.

⚠️ **`build/` holds objects from *both* the `OSD=libretro_m2` and `OSD=sdl3` builds** — they share
the `src/` and `3rdparty/` object trees, so this scan is a **superset** of what the core links. That
is the right direction to be wrong in for an audit: everything above is clear, and the core links
less than everything above. It does mean bgfx/lua/portaudio appear in §6 despite
`module_stubs.cpp` stubbing them out of the core entirely.

### Our own files
All 25 new files carry `// license:BSD-3-Clause` + `// copyright-holders:mcwild77`, in MAME's own
header format. Nothing to fix. This matters more than it looks: MAME's contribution rules
([README.md:68](../README.md)) require every new file to carry one of BSD-3 / LGPL-2.1 / GPL-2.0,
so being already-compliant is what keeps the upstreaming option open.

### Two vendored headers
`src/osd/libretro_m2/libretro.h` (© 2010-2024 The RetroArch team) and `libretro_vulkan.h`
(© 2010-2020) are both **MIT-equivalent, notice-only** — "Permission is hereby granted, free of
charge…", retain the notice, no warranty. Both carry a scoping line saying the licence applies to
that header alone and does not reach the program using it. Redistributing them inside the source
tree is fine; the notice is already in the file.

⚠️ These two are the only files in `src/osd/libretro_m2/` **without** a MAME `// license:` tag. Not
a legal problem — they carry their own, upstream, and altering a vendored file's header is worse
than leaving it. It *is* a problem for MAME's `srcclean`/license tooling if this is ever submitted
upstream. Note it there rather than "fixing" it here.

### The binary itself
`otool -L model2_libretro.dylib` links **nothing but system frameworks** — libSystem, libc++,
CoreAudio/AudioToolbox/AudioUnit, CoreMIDI, CoreServices, Carbon, ApplicationServices,
CoreFoundation, OpenGL. No Vulkan library (all entry points come from the frontend's
`get_instance_proc_addr`, as designed at P2), no Homebrew dylib, nothing that has to be shipped
alongside or that could drag a licence in at link time.

---

## 3. The LGPL question, and why it is already answered

`mfmhd.cpp` and `rpk.cpp` are LGPL-2.1+ and statically linked. LGPL §6 lets you distribute a work
linked against LGPL code provided the recipient can **relink** it — normally satisfied by shipping
object files or a shared library.

**A public source repo that builds the binary satisfies it outright**, and more completely than §6
requires: anyone can rebuild the whole thing. So there is nothing to do, with one standing condition
—

⚠️ **This obligation survives any move to a closed binary.** If a Quest 3 / Unity build is ever
shipped without corresponding source, these two files become a real problem. They are trivially
removable (nothing Model 2 touches TI-99 floppy or cartridge formats), so the fix if that day comes
is to cut them from the build, not to argue about §6. Worth knowing *before* that build exists.

LGPL-2.1+ is GPL-compatible in any case, so it raises no conflict with §4.1.

---

## 4. The four chores, at release time

**4.1 — Release under GPL-2.0-or-later.** Not because anything forces it (§2 found no GPL source),
but because [COPYING](../COPYING) declares the whole to be GPL and BSD-3 + LGPL-2.1+ are both
compatible with it. It is the unambiguous, conventional, argument-free choice. Claiming BSD for the
binary would be defensible from §2 alone and is not worth the conversation it would start.

**4.2 — Tag the release.** GPL and LGPL require source that **corresponds to the binary**. A public
repo at a drifting `main` does not satisfy that; a tag that built the artifact does. Use the
project's own prefix — `m2vk-v0.1` — per the tag convention in `CLAUDE.md` (own tags are prefixed so
they never collide with upstream `mame*` tags).

**4.3 — Ship `docs/legal/` with the binary.** [COPYING](../COPYING) states it explicitly: *"Full
license texts may be found in docs/legal in source and binary distributions."* All eight texts are
present in the tree (BSD-2-Clause, BSD-3-Clause, BSL-1.0, CC0, GPL-2.0, LGPL-2.1, MIT, Zlib). Copy
the directory into the release archive.

**4.4 — Keep the IJG notice.** libjpeg's licence requires *"This software is based in part on the
work of the Independent JPEG Group"*. [COPYING](../COPYING) already carries it. Shipping COPYING
alongside the binary discharges this and several other notice requirements at once, so 4.3 and 4.4
are one action: **put `COPYING` and `docs/legal/` in the archive.**

---

## 5. ⚠️ The trademark point — the only one with a decision in it

Copyright is settled. Trademark is not, and it is a separate body of law that the licences above say
nothing about. [README.md:83](../README.md) — upstream's own words, still verbatim in our fork:

> Please note that MAME is a registered trademark of Gregory Ember, and permission is required to
> use the "MAME" name, logo, or wordmark.

[COPYING](../COPYING) opens with the same statement. The distinction that matters:

- A **source fork** named `mame-model2-vk` on GitHub reads as "a fork of MAME", which is nominative
  use and is ordinary practice across hundreds of public forks. Low risk. ✅ **Moot as of
  2026-08-07 — the repo was renamed to [`mcwild77/m2-vk`](https://github.com/mcwild77/m2-vk) and
  the name no longer carries the wordmark at all.** This was the weakest of the three arguments here
  (it rested on a reader inferring nominative use from a name) and it is now not needed.
- A **downloadable binary** with MAME in the product name is the wordmark on a distributed product.
  That is the thing the notice is about. **Unchanged by the rename** — and it was already handled,
  see below.

**Half of this is already right and should not be undone.**
[retro_entry.cpp:349](../src/osd/libretro_m2/retro_entry.cpp) sets
`info->library_name = "m2-vk"` — so nothing MAME-branded appears anywhere in RetroArch's UI, in
`config/m2-vk/m2-vk.opt`, or in a playlist. That was the largest exposure and it is closed.
⚠️ **The value was `"Model 2"` until 2026-08-07** and this paragraph named it as such; the rename to
`m2-vk` was a naming-consistency change (the core's name now matches the repo's and the `.info`'s
`corename`), **not** a trademark one — neither string carries the wordmark, so the conclusion here is
unchanged either way. What the rename *did* move is three user-data paths RetroArch derives from
`library_name`: the per-core options file, `saves/<name>/` and `states/<name>/`. See §9.

**What is left to do:**

1. **Name the release asset and the release itself to match the core** — ✅ **settled on 2026-08-07 as
   `m2-vk`**, which is now the repo name, the `.info`'s `corename` and the core's own `library_name`,
   all three agreeing. The original wording said "Model 2", not "MAME Model 2"; either avoids the
   wordmark, and `m2-vk` additionally stops the core presenting itself under the *system's* name.
   ⚠️ **The second half of this item is superseded.** It read *"keeping the repository name
   is fine and is arguably clearer about provenance"*, which was true and was not the only option; on
   2026-08-07 the repo was renamed `mame-model2-vk` → **`m2-vk`**, so the wordmark is gone from the
   name and provenance now has to come from the README instead — which is item 2, and which the
   rename therefore makes *more* load-bearing, not less. 🚨 **No committed file ever named the repo**
   (checked at the rename: the only hits in the tree were `CLAUDE.md`, `devnotes/`, `.vscode/` and
   the `devnotes/shortcuts/` launchers, every one of them local-only or gitignored), so the rename
   required no commit and the release checklist in §9 is otherwise unaffected.
2. ⚠️ **[README.md](../README.md) is upstream's, completely unmodified** (last touched by
   `d0231349f75 readme: re-add mameworld link` — an upstream commit). Anyone landing on the repo
   reads a page that presents itself *as MAME*, with MAME's forums, bug tracker and download links.
   That is the single worst thing here for a public release, and it is a documentation problem
   rather than a legal one. A short fork README should say: what this is (a Model-2-only libretro
   core with a Vulkan renderer), that it is derived from MAME and anchored at `mame0288`, that
   MAME is a registered trademark of Gregory Ember and this project is not affiliated with or
   endorsed by MAMEdev, and that bugs go here and not to MAME Testers.

⚠️ **Writing that README is the one piece of new public-facing prose this release needs, and the
commit-hygiene rules in `CLAUDE.md` apply to it in full** — plain human tone, no AI nomenclature
anywhere in it or in the release notes.

---

## 6. Third party

26 third-party libraries have objects in `build/`: asmjit, astc-encoder, bgfx, bimg, bx, dear-imgui,
expat, flac, libjpeg, linenoise, lsqlite3, lua, lua-linenoise, lua-zlib, luafilesystem, lzma,
portaudio, portmidi, softfloat3, sqlite3, tinyexr, utf8proc, wdlfft, ymfm, zlib, zstd.

**None is GPL.** They are MIT / BSD-2 / BSD-3 / zlib / public domain / IJG throughout.

⚠️ **One exception worth writing down so it is not rediscovered: `3rdparty/astc-encoder` is
Apache-2.0**, the only licence in the tree that is incompatible with GPL-2.0-**only**. It is a
non-issue twice over: it arrives via bimg/bgfx, which the libretro core stubs out entirely in
`module_stubs.cpp`, so it is in the `OSD=sdl3` build and not in the dylib; and MAME is GPL-2.0-**or
later**, so the combination resolves at GPL-3.0 regardless. **It only becomes a live question if the
standalone `mamemodel2` is ever shipped as a release artifact** — which it currently is not, being a
development/A-B binary.

Build-time-only tools are not distributed and carry no obligation into the binary: `shaderc`
(Apache-2.0, run by `build_shaders.sh`), `vulkan-headers` (Apache-2.0, headers only), MoltenVK
(Apache-2.0, dlopened by `retrohost --vk` from the *user's* Homebrew, never bundled), and genie.
The SPIR-V in `shaders/*_spv.h` is compiled output of our own BSD-3 GLSL.

---

## 7. ROMs, and repo hygiene at the moment of the audit

**No copyrighted content is tracked.** `git ls-files roms/` returns `roms/dir.txt` and nothing else;
there is no `.zip`/`.7z`/`.chd`/`.rom` anywhere in the index outside bgfx's own example meshes and
shaders. `hash/` is CC0 per COPYING. Standing rules: never commit a ROM, never bundle one with a
release, and **do not link to one from the release page** — a link is a distribution question in its
own right and undoes the point of not shipping them.

**`.gitignore` is pristine.** `git show HEAD:.gitignore` contains no `CLAUDE.md` or `devnotes/` line,
so a GitHub-generated source tarball of the tag leaks nothing about either.

🚨 **CORRECTED TWICE, 2026-07-26. The mechanism is `.gitignore` + `skip-worktree` and it MUST NOT be
"simplified" to `.git/info/exclude` — that was tried and it fails.** This section is where the wrong
answer was first written and then copied into CLAUDE.md, so the record is kept in full.

**What the audit claimed** (all three false): that the working `.gitignore` was byte-identical to the
committed one, that `git ls-files -v` showed no skip-worktree flag anywhere, and that the ignore
therefore rested on `.git/info/exclude` alone. Measured: `git check-ignore -v CLAUDE.md` names
**`.gitignore:52`**, `git ls-files -v | grep -v '^H '` prints **`S .gitignore`**, and the working file
carries two lines the committed one does not.

**What was then recommended, and is worse:** removing the skip-worktree arrangement to leave the exclude
file doing the job, on the untested assertion that "either mechanism alone blocks `git add -A`". **It was
executed and it broke immediately** — `git status` reported `?? CLAUDE.md`, unprotected, one `git add -A`
from being published. Reverted from a backup in the same minute; `git add -A --dry-run` is clean again.

**The root cause, which is the fact worth keeping:** upstream's own `.gitignore` **line 41 is `!/*.md`**,
a negation that un-ignores every root-level `.md` file (it is how `README.md` survives a `/*` blanket
ignore). **`.gitignore` takes precedence over `$GIT_DIR/info/exclude`**, so that negation beats the
exclude file outright. The only place a pattern can win is **inside `.gitignore`, below line 41** — hence
lines 52–53, hence `--skip-worktree` to keep them out of the index, hence the committed file staying
pristine. The exclude file is a partial backstop, not a replacement.

⚠️ **`devnotes/` is double-covered** (upstream's `/*/` at line 13 catches every root directory), so a
half-broken state *looks* fine: `devnotes/` stays hidden and only `CLAUDE.md` pops out. **Check
`CLAUDE.md` by name, always.** The three checks that actually settle it:

```sh
git check-ignore -v CLAUDE.md          # must name .gitignore:52, NOT .git/info/exclude
git ls-files -v | grep -v '^H '        # must print: S .gitignore
git add -A --dry-run | grep -iE 'CLAUDE|devnotes'   # must print NOTHING
```

**So the merge dance is unavoidable**: `--no-skip-worktree` → merge → re-add the two lines →
re-`--skip-worktree`. Rare — `git log --since=2023-01-01 mame0288 -- .gitignore` is empty, upstream has
not touched it in over two years — which is exactly why nobody will remember it. ⚠️ **The dangerous
moment is the recovery, not the merge**: committing `.gitignore` with the two local lines still in it
publishes `/CLAUDE.md`, the precise disclosure the hygiene rules exist to prevent.

⚠️ **No clone carries any of it** — not the working-tree edit, not the flag, not the exclude file. A
fresh checkout shows `CLAUDE.md` untracked and would commit it. Re-add the lines, re-apply
`--skip-worktree`, run the three checks, before anything else.

**Process note: this is now the fourth documented conclusion in this project to outlive its measurement**
(the others: the `cf043ff583370663` digest, `perf.sh`'s speed bracket, and step 6's `dropped` counter),
and every one was caught by re-running a check rather than re-reading prose. The new failure mode here is
worse than staleness, though, and worth naming separately: **a *recommendation* was derived from an
untested assertion and acted on.** "Either mechanism alone blocks `git add -A`" was one dry-run away from
being checked and was not checked. Hence §9's new first row.

---

## 8. How to re-run this audit

Worth doing after any upstream-tag merge (`mame0289`…), since a merge can pull GPL-tagged files into
the driver's dependency set without anything visibly changing.

```sh
# licence tally over everything linked into build/ (superset: both OSD builds share the obj tree)
find build/osx_clang/obj/x64/Release -name '*.o' | grep '/src/' \
  | sed 's|.*/\(src/.*\)\.o$|\1|' | sort -u > /tmp/srcobjs.txt
while read -r p; do
  for ext in cpp c mm; do
    [ -f "$p.$ext" ] || continue
    tag=$(head -2 "$p.$ext" | grep -oE 'license: ?[^ ]*' | head -1)
    echo "${tag:-license:NONE} $p.$ext"; break
  done
done < /tmp/srcobjs.txt | awk '{print $1}' | sort | uniq -c | sort -rn
# ^ note the ' ?' in the grep — six files write "// license: BSD-3-Clause" with a space

# anything GPL that is actually linked — this is the one that must stay empty
# (…same loop, then:)  grep -i 'gpl' | grep -v 'LGPL'

# third-party components with objects
find build/osx_clang/obj/x64/Release/3rdparty -name '*.o' \
  | sed 's|.*/3rdparty/||' | cut -d/ -f1 | sort -u

# nothing copyrighted in the index
git ls-files | grep -iE '\.(zip|7z|chd|rom)$'
git ls-files roms/

# the ignore hack is still doing its job
git show HEAD:.gitignore | grep -nE 'CLAUDE|devnotes'   # must print nothing
cat .git/info/exclude | grep -vE '^#|^$'                # must list both

# what the user sees in the frontend
grep -n 'library_name' src/osd/libretro_m2/retro_entry.cpp

# the binary drags in nothing unexpected
otool -L model2_libretro.dylib
```

---

## 9. Release checklist

- [ ] **`git show <tag>:.gitignore | grep -E 'CLAUDE|devnotes'` returns nothing**, plus the three
      working-copy checks in §7 (`check-ignore -v CLAUDE.md` naming `.gitignore:52`, `ls-files -v`
      showing `S .gitignore`, and a clean `git add -A --dry-run`). §7 has been wrong twice; run the
      commands, do not read the prose. **`CLAUDE.md` by name** — `devnotes/` is double-covered and hides
      a half-broken state.
- [ ] Tag `m2vk-v0.1` at the commit that builds the artifact (§4.2)
- [ ] `COPYING` + `docs/legal/` in the release archive (§4.3, §4.4)
- [ ] Release stated as **GPL-2.0-or-later** (§4.1)
- [ ] Release + asset named **"Model 2"**, not "MAME Model 2" (§5.1)
- [ ] **Fork README written** — what this is, derived from MAME @ `mame0288`, trademark disclaimer,
      not affiliated with MAMEdev, bugs here not MAME Testers (§5.2). Plain human tone.
- [ ] No ROM bundled and **no ROM linked from the release page** (§7)
- [ ] Confirm the release artifact is the **core only** — shipping the standalone `mamemodel2` pulls
      Apache-2.0 astc-encoder into the picture (§6)
