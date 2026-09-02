# Model 2 — Quest / library status checklist

All 90 GAME entries from [src/mame/sega/model2.cpp](../src/mame/sega/model2.cpp), in source order
(already grouped by board and by parent). Generated 2026-09-01 — regenerate rather than hand-edit the
ROM/title columns; write only the **Status** column.

- **Status** — yours to fill (blank = untested). Suggested shorthand: `OK` / `slow` / `no-boot` /
  `gfx` / `sound` / `skip`.
- **Rel** — parent, or which set it's a clone of. You'll usually only test parents.
- **MAME status** — *upstream MAME's* driver flag, **not ours**. `NOT_WORKING` / `imperf.sound` is how
  stock MAME ships the driver; several driving/sim titles carry it yet render fine through the Vulkan
  seam (and some may not). Treat it as a hint about where trouble is likely, nothing more.

⚠️ Reachable ROMs are whatever is in `devnotes/roms/` — this table is the full driver list, not what's
installed. `manxtt`/`von`/`hotd` sets are present as `manxttc`/`vonj`/`hotdo` (see CLAUDE.md run gotcha #2).

## Model 2 / 2O (original)

| Status | ROM | Year | Title | Rel | MAME status |
|---|---|---|---|---|---

daytona-works great
desert-works great
vcop -  testing now - works great - threaded sound / sound link timing

## Model 2A

vf2-works great
manxttc - threaded sound, fast sound link, drive board off
segarallyc- works great, threaded sound, fast sound link ON, drive board OFF
vcop2- great - threaded sound and fast sund link
skytargt- works great - threaded osound, fast soundlink
doaa- works great - threaded, fast sound
zeroguna - runs great - threaded, soundlink
motoraid - runs great - threaded, soundlink, drive board off
dynamcop - runs great -threaded, soundlink
pltkidsa - great - threaded, sndlink

## Model 2B

rchase2-cabinet billboard OFF - can that be no by default
vstrikeri
fvipers
gunblade
indy500
von
schamp/sfight
lastbrnx - threaded, soundlink, marquee off
sgt24h
powsled
dynabb
overrevb
zerogun - works great

## Model 2C
skisuprg
stcc - slow
waverunr - bad
bel
hotdo - fun, playable
overrev
segawsky
topskatr
