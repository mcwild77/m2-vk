# ROM set — what works, and how to invoke it

Local-only note. ROMs live in `devnotes/roms/` (gitignored along with the rest of `devnotes/`, so
there is no risk of committing them). Audited against **mame0288** on 2026-07-25.

## The invocation

```sh
./mamemodel2 <set> -rompath "devnotes/roms;/Users/mcwildmacbookair/Documents/GitHub/Polydiver/roms" \
  -video none -window -nomaximize -nothrottle -skip_gameinfo -str 45
```

**Both paths are still needed for `doaa`, but no longer for `daytona`** (fixed 2026-07-26 — see
below). `devnotes/roms` alone now audits **59/70**, up from 49; adding Polydiver's older set brings it
to 60/71.

| Set | Missing from `devnotes/roms` | Supplied by | status |
| --- | --- | --- | --- |
| `daytona` | `epr-16488.ic12`, `epr-16488a.ic12`, `epr-16726.bin` | `Polydiver/roms/daytona.zip` | **fixed** — the three files were copied into `devnotes/roms/daytona.zip` on 2026-07-26, because `retrohost` has no way to reach a second rompath and the screenshot pass needed the set. `daytona` and 6 of its 8 clones now verify; `daytona93` remains bad for an unrelated reason. |
| `doa` | `epr-19379c.15`, `epr-19380c.16`, `mpr-19324.19` | `Polydiver/roms/doa.zip` | **fixed 2026-07-31** — the whole zip was swapped rather than three files merged in, because Polydiver's is a different *complete* revision (30.3 MB against 31.1) and merging would have produced a set matching neither. Verified: 400 frames, video and audio, no audit line. The displaced zip is not in the repo — it went to the session scratchpad, so it is gone once that is cleaned. |

Note `epr-16726.bin` is already present in `srallyc.zip` — it is a drive-board ROM shared between
Sega Rally and Daytona.

🚨 **`devnotes/roms` IS THE ONE ROM DIRECTORY, EVERYWHERE, as of 2026-07-31 (second change that day).**
The morning's fix pointed RetroArch's second rompath at `Polydiver/roms`; that closed the `doa` failure
below but left **two** disagreeing sets in play — the harness ran out of `devnotes/roms` while the
Desktop app's playlist ran out of `~/Documents/ROMs/Model 2`, so a set could work under `ab.sh` and fail
in the app, or the reverse, with nothing in either log saying why. All three readers now point at one
directory:

```sh
ln -sfn "$PWD/devnotes/roms" ~/Documents/RetroArch/system/model2   # the core's second rompath
# ~/Documents/RetroArch/playlists/Sega - Model 2.lpl — all 32 item paths rewritten to devnotes/roms
# devnotes/tools/padmap-serve.py ROM_DIRS — now a one-element list
```

⚠️ **`~/Documents/ROMs/Model 2` and `Polydiver/roms` are no longer read by anything here.** They still
exist and nothing was deleted; the playlist backup is `Sega - Model 2.lpl.bak`.

✅ **It cost nothing, and that was measured rather than assumed** (`retrohost` per set with
`M2_SYSTEM_DIR` unset, so `devnotes/roms` alone). `daytona` **passes**, which retires this file's own
"needs both rompath entries" warning — the three files were copied in on 2026-07-26 and the row above
already said so. `doa` was the single casualty and **was fixed the same hour** by swapping in
Polydiver's zip (see its row). So `devnotes/roms` is now a superset of what either of the retired
directories offered, and no set is reachable only from outside it.

⚠️ **The failure looks like the frontend, not like a ROM**: RetroArch exits within a couple of seconds,
**with status 0**, and the reason is only in its log. That is what made the pad editor's ▶ Play button
report "playing doa" for a run that never drew a frame ([padmap-tool.md](padmap-tool.md) §3.2.2).

**Why this matters beyond the standalone binary:** `retrohost` takes a single ROM *file* path and adds
only `$M2_SYSTEM_DIR` to the rompath, so a set that needs two rompath entries cannot be loaded by the
A/B harness at all. Anything that has to be measured or screenshotted must be self-contained in
`devnotes/roms`. The fix is mechanical — extract the named files and `zip -j` them into the local set,
then confirm with `./mamemodel2 -rompath devnotes/roms -verifyroms <set>` before and after.

Flags worth remembering: `-window` is **required** (fullscreen is MAME's default and `-video none`
still creates a window, so omitting it blanks the display). No recording flag is needed — see the
headless section of [seam.md](seam.md). Runs go at 300–500 % of real time.

## 🚨 Three sets were MISNAMED, not missing (2026-08-02)

A boot sweep of every local zip ([compatibility.md](compatibility.md) is the result) found three
failures, and **all three were fixed the same day with no new dumps**. The pattern each time: a zip
named after a **parent** whose contents are a **clone**.

| Was | Now | Why it failed | Fix |
|---|---|---|---|
| `hotd.zip` | **`hotdo.zip`** | holds `epr-19696.15`/`epr-19697.16`; the `hotd` parent wants the `a` revisions | copied to the clone's name — 823 3D frames |
| `von.zip` | **`vonj.zip`** | holds `epr-18664b.15`/`epr-18665b.16` — that is `vonj`, Japan Rev B | copied to the clone's name — 633 3D frames |
| `vcop.zip` | `vcop.zip` (patched) | missing `epr-17181.6` (the **`model1io2`** board BIOS) and `hd44780_a00.bin` (an LCD character ROM MAME flags `BAD_DUMP`) | both extracted from `Polydiver/roms/vcop.zip` and `zip -j`'d in — 478 3D frames. Original kept as `vcop.zip.bak` |

⚠️ **The two clone zips were ADDED, not renamed over** — `hotd.zip` and `von.zip` are still present and
still fail. The RetroArch playlist was repointed at the working names (backup
`Sega - Model 2.lpl.bak2`).

🚨 **This partly retracts the file's own claim above that "`devnotes/roms` is a superset of what either
retired directory offered".** It was not, for `vcop`: the two files it needed existed **only** in
`Polydiver/roms/vcop.zip`, and the 2026-07-31 consolidation broke `vcop` without anyone noticing —
because every earlier `vcop` run passed `M2_SYSTEM_DIR` pointing at Polydiver's directory. It is a
superset now. **The lesson is that "it cost nothing" was measured per set against the sets that were
being run, and `vcop` was not among them that day.**

⚠️ **`von.zip` and `hotd.zip` were also the whole of [tools/README.md](tools/README.md) §7's "two games
are genuinely missing files"** — struck there. **No set in the library is now blocked on a dump we do
not have.**

## Sets to use, and to avoid

- **Use `manxttc`, not `manxtt`.** `manxtt` verifies fine but renders **zero** 3D frames even at 120
  emulated seconds. That is a MAME limitation, not a ROM problem: `model2.cpp:7657` marks it
  `MACHINE_NOT_WORKING` with the comment *"Defaults to DX mode"*. `manxttc` is the same game in Twin
  mode and renders normally (~950 polys/frame). `manxttdx` is DX too — same dead end.
- **`MACHINE_NOT_WORKING` is not a useful filter.** Many Model 2 sets carry it — `srallyc`,
  `fvipers`, `lastbrnx`, `dynamcop`, `schamp`, `von`, `skytargt` — yet all render 3D fine. The flag
  reflects sound/IO/link gaps, not the rasterizer. Only the Manx TT DX-mode sets actually fail to
  render.
- **Four parents fail audit but need nothing:** `doa` → use clone `doaa`; `hotd` → `hotdo`;
  `von` → `vonj` or `vonu`; `vcop` runs as-is (its only mismatch is `hd44780_a00.bin`, an LCD
  character ROM MAME itself flags `BAD_DUMP`; boots with a warning and renders ~620 polys/frame).

## Local patches applied to the set

Two download artefacts and one gap, fixed 2026-07-25:

- `overrev (1).zip` → renamed `overrev.zip`. MAME matches zip name to set name, so the `" (1)"`
  suffix made it invisible.
- `manxttc (1).zip` — **not** used to replace `manxttc.zip`. It is not a strict superset: the older
  zip carries `mpr-18862.4` / `mpr-18863.5` where the newer one has `epr-18862.4` / `epr-18863.5`
  (mask-ROM vs EPROM naming for the same chips). Rather than guess which is canonical, the two
  genuinely missing files were extracted into directories MAME also searches:
  - `devnotes/roms/manxttc/` ← `epr-18643.7`, `manxttc_twin_nvran`
  - `devnotes/roms/overrev/` ← `epr-18643.7`

  Both sets now audit good. Non-destructive: delete the directories to revert. Worth settling the
  `manxttc.zip` question properly at some point.
- `segabill.zip` copied in from `Polydiver/roms` (3 KB). Forty-odd sets depend on this BIOS.

## System 22 set — `devnotes/roms/system22/` (complete 2026-08-22)

The Namco (Super) System 22 ROMs live in their own subfolder, `devnotes/roms/system22/`, passed to
`retrohost` by full path (`devnotes/roms/system22/<set>.zip`). **All 18 zips present audit complete**
against the `mame0288` definitions — the full working parent list except the two MAME itself can't run
(`ridgeracf`, incomplete dump; `ridgerac3m`, 3-monitor clone):

- **System 22** (plain, `renderscanline_poly`): `ridgerac`, `ridgera2`, `raverace`, `acedrive`,
  `victlap`, `cybrcomm`.
- **Super System 22** (`renderscanline_poly_ss22`, fog/fade/alpha/spot, sprite-in-tree):
  `alpinerd`, `alpinr2b`, `alpines`, `airco22b`, `cybrcycc`, `dirtdasha`, `dirtdashj`, `timecris`,
  `propcycl`, `tokyowar`, `aquajet`, `adillor`.

Each parent zip is self-contained (the MCU BIOS sits in the set's own `"mcu"` region; there is no shared
System 22 BIOS zip and none of these are clones needing a parent merge). `dirtdasha` / `dirtdashj` are
Dirt Dash clones but carry the full non-merged set, so they boot standalone — used because the parent
`dirtdash` (Ver.C) set was not to hand and a stale/incomplete Ver.B zip was discarded. `timecris` and
`propcycl` are the SS22 sprite-in-tree dev targets (§A.3 of `system22plan.md`); any SS22 title exercises
the fog/fade shading tail.

**Auditing the System 22 set:** there is no standalone `mamenamcos22` binary (the driver ships as the
`namcos22_libretro.dylib` core only), so `-verifyroms` is not available here. Audit offline instead —
compare each zip's central-directory CRC32 against the `ROM_LOAD` CRCs in
`src/mame/namco/namcos22.cpp`, which is exactly what MAME's loader matches on:

```sh
python3 devnotes/tools/audit-s22-roms.py   # ROM_START CRCs in namcos22.cpp vs each zip's stored CRC32
```

Prints `### <set>: OK` per set and `==> ALL GOOD` when clean (exit 0); "name?" lines are noise — a
shared ROM listed at mirrored addresses under different names, loaded by hash. Only `MISSING` lines are
real.

## System 21 sets — ALSO in `devnotes/roms/system22/`

⚠️ **The `system22/` folder is a Namco catch-all, not S22-only** — it also holds the **System 21** sets,
which run with the **`namcos21_libretro.dylib`** core (the S22 sets above run with `namcos22`). The name
is historical (the folder predates the S21 port); do not go looking for a `system21/` folder, there
isn't one. Present S21 zips include `starblad` (Star Blade), `cybsled` (Cyber Sled), `aircomb`
(Air Combat), `solvalou` (Solvalou), `driveyes` (Driver's Eyes), `winrun` / `winrungp` (Winning Run).

Run by full path, same as the S22 sets:
```sh
./devnotes/retrohost ./namcos21_libretro.dylib devnotes/roms/system22/starblad.zip 400 /tmp/o.ppm
```
`starblad` and `cybsled` are the analog-stick hand-check targets for
[analog-deadzone-reach-plan.md](analog-deadzone-reach-plan.md) — starblad an IPT_AD_STICK_X/Y aim stick,
cybsled a twin-stick (P1+P2 AD_STICK). (`solvalou` / `aircomb` are S21 too and are NOT in the `namcos22`
subtarget's driver list, so they fail to start under that core — use `namcos21`.)

## Re-auditing (Model 2)

```sh
./mamemodel2 -rompath "devnotes/roms;/Users/mcwildmacbookair/Documents/GitHub/Polydiver/roms" -verifyroms
```
