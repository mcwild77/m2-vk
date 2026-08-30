#!/usr/bin/env python3
# Offline audit of the System 22 ROM set (devnotes/roms/system22/).
#
# There is no standalone namcos22 MAME binary here (the driver ships as the libretro core only), so
# -verifyroms is not available. This does the same job offline: compare each zip's central-directory
# CRC32 against the ROM_LOAD CRCs in src/mame/namco/namcos22.cpp — which is what MAME's loader matches
# on. Prints "### <set>: OK" per set and "==> ALL GOOD" when clean.
#
# "name?" lines are NOT problems: MAME lists a shared ROM at mirrored addresses under different names,
# and it loads by hash, so one copy satisfies all of them. Only "MISSING" lines are real.
#
# Run from the repo root:  python3 devnotes/tools/audit-s22-roms.py

import os, re, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src/mame/namco/namcos22.cpp")
ROMDIR = os.path.join(ROOT, "devnotes/roms/system22")

text = open(SRC, encoding="utf-8", errors="replace").read()
sets = {m.group(1): m.group(2)
        for m in re.finditer(r"ROM_START\(\s*([A-Za-z0-9_]+)\s*\)(.*?)ROM_END", text, re.S)}

def parse_roms(body):
    roms = []
    tokens = list(re.finditer(r'\bROM[X]?_LOAD[0-9A-Z_]*\(\s*"([^"]+)"', body))
    for i, t in enumerate(tokens):
        seg = body[t.end():(tokens[i+1].start() if i+1 < len(tokens) else len(body))]
        crcm = re.search(r'CRC\(\s*([0-9a-fA-F]{8})\s*\)', seg)
        roms.append((t.group(1), crcm.group(1).lower() if crcm else None))
    return roms

def zip_crcs(path):
    with zipfile.ZipFile(path) as z:
        return {zi.filename: "%08x" % (zi.CRC & 0xffffffff) for zi in z.infolist() if not zi.is_dir()}

present = sorted(f[:-4] for f in os.listdir(ROMDIR) if f.endswith(".zip"))
print(f"{len(present)} zips in {ROMDIR}\n")
ok = True
for setname in present:
    if setname not in sets:
        print(f"### {setname}: NOT a driver set at this tag !!!"); ok = False; continue
    req = [(fn, crc) for fn, crc in parse_roms(sets[setname]) if crc]
    zc = zip_crcs(os.path.join(ROMDIR, setname + ".zip"))
    zvals = set(zc.values())
    znames = {n.lower(): c for n, c in zc.items()}
    missing = [(fn, crc) for fn, crc in req if crc not in zvals]
    namewarn = [(fn, crc) for fn, crc in req if crc in zvals and znames.get(fn.lower()) != crc]
    if missing: ok = False
    print(f"### {setname}: {'OK' if not missing else 'MISSING ROMS'}  "
          f"(requires {len(req)} dumped roms, zip has {len(zc)} files)")
    for fn, crc in missing:  print(f"    MISSING  {fn}  CRC {crc}")
    for fn, crc in namewarn: print(f"    name?    {fn}  (that CRC is stored under a different filename; loads by hash)")
print("\n==> ", "ALL GOOD" if ok else "PROBLEMS ABOVE")
sys.exit(0 if ok else 1)
