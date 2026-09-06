#!/usr/bin/env python3
"""Generate src/osd/libretro_m2/input_layouts.ipp from input_layouts.json.

The JSON is the data of record and padmap.html authors it.  This turns it into the table the core
compiles, and it does one thing that matters beyond formatting: it INVERTS the button assignments into a
flat per-control label array.

That inversion is the whole reason a generator exists.  The core needs two views of one fact — "MAME
button n is produced by control c" for reading the pad, and "control c is called L" for telling the
frontend — and for as long as those were two hand-written tables they disagreed: the shoulder-button
descriptors were inverted for months, so daytona's remap screen named GEAR 4 and VR1 the wrong way round
(devnotes/reference/input-map.md §5.1).  Deriving one from the other, once, makes that class of bug unable to occur.

The collision check falls out of the same pass for free: if two MAME buttons name one control, the
inversion has two labels for one slot and says so instead of silently keeping the last.

    ./devnotes/tools/padmap-gen.py            # write the .ipp
    ./devnotes/tools/padmap-gen.py --check    # verify the .ipp matches the .json, and validate the rows
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
JSON_PATH = HERE / "src/osd/libretro_m2/input_layouts.json"
IPP_PATH = HERE / "src/osd/libretro_m2/input_layouts.ipp"
# Both driver families feed one layout file — set names are unique across them, so the tables merge. A
# row for a System 22 cabinet names System 22 sets; without namcos22.cpp here the set-existence check
# would refuse every one of them as "not a GAME entry". Kept in step with padmap-sweep.sh's FAMILIES.
DRIVER_PATHS = [
    HERE / "src/mame/sega/model1.cpp",
    HERE / "src/mame/sega/model2.cpp",
    HERE / "src/mame/namco/namcos22.cpp",
    # System 21 GAME entries are split across three files (winrun/winrungp in namcos21.cpp; the C67
    # sets — starblad, cybsled, aircomb, solvalou — in namcos21_c67.cpp; driveyes in namcos21_de.cpp).
    HERE / "src/mame/namco/namcos21.cpp",
    HERE / "src/mame/namco/namcos21_c67.cpp",
    HERE / "src/mame/namco/namcos21_de.cpp",
    HERE / "src/mame/namco/namcos23.cpp",
]
DATA_PATH = HERE / "devnotes/tools/padmap-data.js"

NUMBERED_BUTTONS = 9

# The sixteen RetroPad ids in their own numeric order — a label array is indexed by the libretro id, so
# the descriptor builder in the core needs no mapping table at all.  Then the four analog axes, which
# have no JOYPAD id of their own.
DIGITAL_IDS = [
    "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT",
    "A", "X", "L", "R", "L2", "R2", "L3", "R3",
]
ANALOG_IDS = ["LSTICK_X", "LSTICK_Y", "RSTICK_X", "RSTICK_Y"]
LABEL_IDS = DIGITAL_IDS + ANALOG_IDS

# What a layout row may name as a button source, and the C++ expression for each.  This is exactly the
# vocabulary read_source() understands; anything else is a typo and is refused rather than emitted.
SOURCE_EXPR = {name: f"RETRO_DEVICE_ID_JOYPAD_{name}" for name in DIGITAL_IDS}
SOURCE_EXPR.update({"L2_AXIS": "SOURCE_L2_AXIS", "R2_AXIS": "SOURCE_R2_AXIS", "NONE": "SOURCE_NONE"})

# Sources that are never valid in a row, with the reason, so the message says why rather than "invalid".
FORBIDDEN_SOURCES = {
    "SELECT": "SELECT carries COIN1 through MAME's own indexed defaults — a numbered button there takes a credit on every press",
    "START": "START carries START1 through MAME's own indexed defaults — a numbered button there would start a game on every press",
}

# Where a button source lands in the label array: the two trigger thresholds share their trigger's slot,
# because a libretro descriptor is per (port, device, index, id) and the pedal and the button are one id.
SOURCE_LABEL_SLOT = {name: name for name in DIGITAL_IDS}
SOURCE_LABEL_SLOT.update({"L2_AXIS": "L2", "R2_AXIS": "R2"})


def paddle_portsets():
    """The sweep's verdict per port set: ({portset: paddle field name}, {port sets dumped at all}).

    The paddle half is the same fact m2vk_steer.h's detector asks the machine at runtime, read here from
    the dumps instead — so the label check below is against what the steering curve will actually do
    rather than against a list somebody keeps.  The second half is what stops the check being an argument
    from silence: a port set nobody dumped is unknown, not paddle-free.

    None when there is no sweep data at all, which makes the check silent rather than wrong —
    padmap-data.js is devnotes/, and a checkout without it must still be able to generate.
    """
    try:
        text = DATA_PATH.read_text()
        data = json.loads(text[text.index("{"):text.rindex("}") + 1])
    except (OSError, ValueError):
        return None, None

    known = driver_sets()
    paddles, dumped = {}, set()
    for name, dump in (data.get("dumps") or {}).items():
        if name not in known:
            continue
        portset = known[name][1]
        dumped.add(portset)
        for f in dump.get("fields") or []:
            if "PADDLE" in (f.get("token") or ""):
                paddles.setdefault(portset, f.get("name") or "Paddle")
    return paddles, dumped


def joy_shifter_portsets():
    """Per port set, the gear shift's two PORT_NAMEs when it sits on the digital joystick.

    The System 22/21 racers put the shifter on IPT_JOYSTICK_UP/DOWN rather than on a numbered button
    (namcos22.cpp: "Shift Down" is IPT_JOYSTICK_UP, "Shift Up" is IPT_JOYSTICK_DOWN — the names read
    inverted from the direction, and are honoured rather than second-guessed). The layout table only maps
    *numbered* buttons, so there is no way to put shift on a shoulder from input_layouts.json; the core
    routes it, gated on the joy_shifter flag this detects. Detected, not authored — read from the same
    sweep dumps the paddle check uses — so a new racer needs no hand edit.

    Keyed on port set exactly like paddle_portsets(): a dumped clone (dirtdashj) stands in for a parent
    with no dump of its own (dirtdash), because they share one INPUT_PORTS macro. Returns
    {portset: (up_name, down_name)}, or None when there is no sweep data at all (silent, like the paddle
    check — a checkout without padmap-data.js must still generate).
    """
    try:
        text = DATA_PATH.read_text()
        data = json.loads(text[text.index("{"):text.rindex("}") + 1])
    except (OSError, ValueError):
        return None

    known = driver_sets()
    out = {}
    for name, dump in (data.get("dumps") or {}).items():
        if name not in known:
            continue
        portset = known[name][1]
        for f in dump.get("fields") or []:
            token = f.get("token") or ""
            label = f.get("name") or ""
            if "shift" not in label.lower():
                continue
            slot = out.setdefault(portset, {})
            if token.startswith("P1_JOYSTICK_UP"):
                slot["up"] = label
            elif token.startswith("P1_JOYSTICK_DOWN"):
                slot["down"] = label
    # only a port set with BOTH halves is a real H-gate shifter
    return {ps: (v["up"], v["down"]) for ps, v in out.items() if "up" in v and "down" in v}


def mark_joy_shifters(rows, errors):
    """Set row['joy_shifter'] and inject the R/L descriptors on every racer whose dump shows the shifter
    on the joystick. The labels are derived from the dump field names, not from the .json — so the .json
    stays free of them and the two can never disagree. R carries "Shift Up" (IPT_JOYSTICK_DOWN), L carries
    "Shift Down" (IPT_JOYSTICK_UP): the natural console feel, upshift on the right shoulder."""
    portsets = joy_shifter_portsets()
    if portsets is None:
        return
    known = driver_sets()
    for row in rows:
        names = [n for n in row.get("sets") or [] if n in known]
        match = next((portsets[known[n][1]] for n in names if known[n][1] in portsets), None)
        if match is None:
            continue
        up_name, down_name = match
        row["joy_shifter"] = True
        labels = row.setdefault("labels", {})
        # R1 = IPT_JOYSTICK_DOWN (down_name, "Shift Up"); L1 = IPT_JOYSTICK_UP (up_name, "Shift Down")
        labels.setdefault("R", down_name)
        labels.setdefault("L", up_name)


def check_steering(rows, generic, claimed, errors):
    """A row that steers must be consistent with the machine under it, both ways.

    The coupling is one fact seen from two sides, invisible from either alone.  The analog steering curve
    (devnotes/reference/steering-curve.md) applies iff the machine declares an IPT_PADDLE — nothing is authored and
    nothing can be switched on per game — while the LSTICK_X label is the only place the frontend ever says
    what the stick does.  So a row on a paddle machine that does NOT label LSTICK_X "steer" hides the wheel,
    and a row labelled "Steering" on a machine with no paddle promises a curve that will never run.  Neither
    shows up in a build, a digest, or a screenshot; both show up here.

    What this does NOT do (since the 2026-08-22 System 22 bring-up): demand a row for every paddle machine.
    That completeness half blocked all partial saves for a new driver family — you could not save the first
    S22 racer until you had authored all nine.  A paddle machine left on the generic row is now a
    non-blocking note; tracking which cabinets still lack a row is Track D's compatibility matrix, not this
    gate.  The two consistency errors below still fire, so a row that IS authored can never disagree with
    its machine.
    """
    paddles, dumped = paddle_portsets()
    if paddles is None:
        return

    known = driver_sets()
    byid = {r["id"]: r for r in rows}
    steers = lambda text: "steer" in (text or "").lower()

    uncovered = []
    for name, (parent, portset) in sorted(known.items()):
        if portset not in paddles:
            continue
        rid = claimed.get(name) or (claimed.get(parent) if parent and parent != "0" else None)
        if rid is None:
            uncovered.append((name, portset))
            continue
        label = (byid[rid].get("labels") or {}).get("LSTICK_X")
        if not steers(label):
            errors.append(
                f"'{name}' declares an IPT_PADDLE (\"{paddles[portset]}\") and is shaped by the steering "
                f"curve, but row {rid} labels LSTICK_X \"{label}\" — the one place that says so does not")

    for row in rows:
        if not steers((row.get("labels") or {}).get("LSTICK_X")):
            continue
        sets = [n for n in row.get("sets") or [] if n in known]
        covered = [n for n in sets if known[n][1] in paddles]
        if not covered and sets and all(known[n][1] in dumped for n in sets):
            errors.append(
                f"{row['id']}: LSTICK_X is labelled \"{(row['labels'] or {}).get('LSTICK_X')}\" but none of "
                f"its sets declares an IPT_PADDLE, so the steering curve will not run there — the label "
                f"promises shaping the detector never applies")

    # Non-blocking coverage note: paddle cabinets still on the generic hedge, one line per port set. Not an
    # error (see the docstring) — a reminder that these racers steer but have no row yet. Track D owns the
    # real completeness accounting.
    if uncovered:
        by_portset = {}
        for name, portset in uncovered:
            by_portset.setdefault(portset, paddles[portset])
        listing = ", ".join(f"{ps} (\"{nm}\")" for ps, nm in sorted(by_portset.items()))
        print(f"note: {len(by_portset)} paddle port set(s) still on the generic steering hedge — {listing}",
              file=sys.stderr)


def cstr(s):
    """A C string literal, or nullptr for nothing.  An empty label and a missing one are the same thing to
    the core — no descriptor is sent — so both become nullptr rather than "" ."""
    if s is None or not str(s).strip():
        return "nullptr"
    out = str(s).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{out}"'


def ccomment(s):
    """A comment body that cannot end the comment it is in."""
    return re.sub(r"\s+", " ", str(s or "")).replace("*/", "* /").strip()


def driver_sets():
    """Every GAME/GAMEL entry across both driver .cpp files as {name: (parent, portset)}.

    A grep over GAME lines, which is safe in a way that parsing INPUT_PORTS would not be: a GAME entry is
    one line with fixed argument positions, while ports involve PORT_INCLUDE and PORT_MODIFY and would
    mean reimplementing MAME's own resolution.  That is what padmap-sweep.sh boots a machine to avoid.
    """
    out = {}
    for path in DRIVER_PATHS:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if not line.startswith(("GAME(", "GAMEL(")):
                continue
            args = [a.strip() for a in re.split(r"[(,]", line)]
            if len(args) < 6:
                continue
            out[args[2]] = (args[3], args[5])
    return out


def invert_labels(row, errors):
    """The inversion: a row's assignments and explicit labels become one array indexed by control.

    Explicit labels in the JSON win, because the author edited them in the tool with the button names in
    front of them.  A control named by two live buttons is an error here rather than a silent last-wins,
    which is the collision check the plan promised would come free with this pass.
    """
    labels = {k: None for k in LABEL_IDS}
    feeds = {}

    for i, b in enumerate(row["buttons"]):
        src = b["source"]
        if src == "NONE":
            continue
        slot = SOURCE_LABEL_SLOT.get(src)
        if slot is None:
            continue                      # an unknown source is reported by validate_row
        feeds.setdefault(slot, []).append(i + 1)

    for slot, buttons in feeds.items():
        if len(buttons) > 1:
            errors.append(
                f"{row['id']}: control {slot} feeds MAME buttons "
                f"{' and '.join(str(b) for b in buttons)} — one press, two buttons")

    for slot, text in (row.get("labels") or {}).items():
        if slot not in labels:
            errors.append(f"{row['id']}: label for unknown control '{slot}'")
            continue
        labels[slot] = text

    # 🚨 A CONTROL MUST BE LABELLED WITH THE WORDING OF THE BUTTON IT FEEDS, and until 2026-07-31 nothing
    # checked it — so a doa row whose labels had stopped following its assignments passed through here,
    # got compiled, built and played, and told the frontend that Y was "Kick" while Y pressed Hold.  This
    # file already knew both halves (it prints the feeding button in the comment beside every label); it
    # simply never compared them.
    #
    # Only where exactly one button feeds the control: a trigger shared with a pedal axis reads
    # "Accelerator / Button 8", which is nobody's single wording — hence the " / " split rather than an
    # equality.
    for slot, buttons in feeds.items():
        if len(buttons) != 1:
            continue
        want = (row["buttons"][buttons[0] - 1].get("label") or "").strip()
        got = labels.get(slot)
        if not want or got is None:
            continue
        if want not in [p.strip() for p in got.split("/")]:
            errors.append(
                f"{row['id']}: control {slot} feeds MAME button {buttons[0]} (\"{want}\") but is "
                f"labelled \"{got}\" — the frontend would name it something it does not do")

    return labels, feeds


def validate_row(row, known_sets, errors):
    rid = row.get("id")
    if not rid:
        errors.append("a row has no id")
        return

    buttons = row.get("buttons") or []
    if len(buttons) != NUMBERED_BUTTONS:
        errors.append(f"{rid}: {len(buttons)} button slots, expected {NUMBERED_BUTTONS}")

    for i, b in enumerate(buttons):
        src = b.get("source")
        if src not in SOURCE_EXPR:
            errors.append(f"{rid}: slot {i + 1} names '{src}', which read_source() does not understand")
        elif src in FORBIDDEN_SOURCES:
            errors.append(f"{rid}: slot {i + 1} is on {src} — {FORBIDDEN_SOURCES[src]}")

    # Every name a row claims has to exist, or the row silently never matches anything and the game plays
    # on the generic layout while the table looks right.
    for name in row.get("sets") or []:
        if name not in known_sets:
            errors.append(f"{rid}: names set '{name}', which is not a GAME entry in model2.cpp or namcos22.cpp")
    if not (row.get("sets") or []):
        errors.append(f"{rid}: names no sets, so nothing can ever resolve to it")

    # Required rather than defaulted, because a missing flag would silently mean "no gun" and a gun
    # cabinet's trigger would stop being labelled with nothing to say so.
    if not isinstance(row.get("lightgun"), bool):
        errors.append(f"{rid}: 'lightgun' must be true or false — it decides whether gun descriptors are sent")


def render(doc):
    known = driver_sets()
    errors = []

    rows = sorted(doc.get("rows") or [], key=lambda r: r.get("id") or "")

    # One name, one row.  Two rows claiming a name would leave which one wins up to the order of a
    # constexpr array, which is not somewhere a control layout should live.
    claimed = {}
    for row in rows:
        validate_row(row, known, errors)
        for name in row.get("sets") or []:
            if name in claimed:
                errors.append(f"'{name}' is claimed by both {claimed[name]} and {row['id']}")
            claimed[name] = row["id"]

    generic = doc.get("generic") or {}
    if len(generic.get("buttons") or []) != NUMBERED_BUTTONS:
        errors.append(f"the generic row has {len(generic.get('buttons') or [])} slots, expected {NUMBERED_BUTTONS}")

    mark_joy_shifters(rows, errors)
    check_steering(rows, generic, claimed, errors)

    if errors:
        return None, errors

    L = []
    w = L.append
    w("// license:BSD-3-Clause")
    w("// copyright-holders:mcwild77")
    w("//")
    w("// GENERATED by devnotes/tools/padmap-gen.py from input_layouts.json — do not edit.")
    w("// Author layouts in devnotes/tools/padmap.html, then re-run the generator.")
    w("//")
    w("// One row per port set: which RetroPad control produces each MAME button, and what every control is")
    w("// called in the frontend's Controls menu.  The label array is DERIVED from the button assignments")
    w("// rather than written beside them, which is why the two can no longer disagree — see the generator's")
    w("// docstring and devnotes/reference/input-map.md §5.1 for the bug that made this necessary.")
    w("//")
    w("// Included from libretro_m2_input.cpp inside its anonymous namespace, after the SOURCE_* constants.")
    w("")

    def emit_layout(row, name, indent):
        pad = "\t" * indent
        labels, feeds = invert_labels(row, errors)
        out = []
        out.append(f"{pad}{{")
        out.append(f"{pad}\t{cstr(row.get('id'))},")
        out.append(f"{pad}\t{name},")
        out.append(f"{pad}\t{'true' if row.get('lightgun') else 'false'},"
                   f"{'' if row.get('lightgun') else '   // no IPT_LIGHTGUN_X/Y: send no gun descriptors'}")
        out.append(f"{pad}\t{'true' if row.get('twin_ad_stick') else 'false'},"
                   f"{'  // single-pad twin-stick: OR pad 1 right stick onto player>1 IPT_AD_STICK' if row.get('twin_ad_stick') else '  // not a single-pad twin-stick cabinet'}")
        out.append(f"{pad}\t{'true' if row.get('joy_shifter') else 'false'},"
                   f"{'   // gear shift is IPT_JOYSTICK_UP/DOWN: route it onto L1/R1 too' if row.get('joy_shifter') else '  // gear shift, if any, is a numbered button (no joystick routing)'}")
        out.append(f"{pad}\t{{")
        for i, b in enumerate(row["buttons"]):
            src = b["source"]
            note = ccomment(b.get("label") or "")
            why = ccomment(b.get("why") or "")
            tail = ""
            if src == "NONE":
                tail = f"  /* BUTTON{i + 1} {note or '(unused)'}" + (f" — {why}" if why else "") + " */"
            elif note:
                tail = f"  /* BUTTON{i + 1} {note} */"
            out.append(f"{pad}\t\t{SOURCE_EXPR[src]},{tail}")
        out.append(f"{pad}\t}},")
        out.append(f"{pad}\t{{")
        for idx, slot in enumerate(LABEL_IDS):
            feed = feeds.get(slot)
            tag = f"BUTTON{feed[0]}" if feed else ""
            out.append(f"{pad}\t\t/* {idx:2d} {slot:<9}{tag:<8}*/ {cstr(labels[slot])},")
        out.append(f"{pad}\t}},")
        out.append(f"{pad}}}")
        return out

    # the sets[] arrays, one per row, null-terminated
    for row in rows:
        names = ", ".join(cstr(n) for n in row["sets"])
        w(f"char const *const SETS_{row['id']}[] = {{ {names}, nullptr }};")
    w("")

    w("// The fallback, and it is deliberately identical to the layout every set used before there were")
    w("// per-game rows: the regression check for this whole change is that a set with no row of its own is")
    w("// byte-identical to today, and that only holds if this stays exactly what BUTTON_LAYOUTS' Classic row")
    w("// was. Do not \"improve\" it.")
    if generic.get("note"):
        w(f"// {ccomment(generic['note'])}")
    generic_row = dict(generic)
    generic_row.setdefault("id", "generic")
    generic_row.setdefault("sets", [])
    w("constexpr game_layout GENERIC_LAYOUT =")
    L.extend(emit_layout(generic_row, "nullptr", 0))
    L[-1] = L[-1] + ";"
    w("")

    w("constexpr game_layout GAME_LAYOUTS[] = {")
    for n, row in enumerate(rows):
        if row.get("note"):
            for chunk in wrap_comment(row["note"], 108):
                w(f"\t// {chunk}")
        w(f"\t// verified: {ccomment(row['verified'])}" if row.get("verified")
          else "\t// verified: NOT YET MEASURED IN GAME")
        L.extend(emit_layout(row, f"SETS_{row['id']}", 1))
        L[-1] = L[-1] + ("," if n + 1 < len(rows) else "")
    w("};")
    w("")
    w(f"static_assert(std::size(GAME_LAYOUTS) == {len(rows)}, \"regenerate input_layouts.ipp\");")

    if errors:
        return None, errors
    return "\n".join(L) + "\n", []


def wrap_comment(text, width):
    words = ccomment(text).split()
    lines, cur = [], ""
    for word in words:
        if cur and len(cur) + 1 + len(word) > width:
            lines.append(cur)
            cur = word
        else:
            cur = f"{cur} {word}".strip()
    if cur:
        lines.append(cur)
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="verify the .ipp on disk matches the .json and the rows validate; write nothing")
    args = ap.parse_args()

    try:
        doc = json.loads(JSON_PATH.read_text())
    except FileNotFoundError:
        sys.exit(f"no {JSON_PATH.relative_to(HERE)} — author one in devnotes/tools/padmap.html")
    except json.JSONDecodeError as e:
        sys.exit(f"{JSON_PATH.relative_to(HERE)} is not valid JSON: {e}")

    text, errors = render(doc)
    if errors:
        print(f"{len(errors)} problem(s) in {JSON_PATH.relative_to(HERE)}:", file=sys.stderr)
        for e in errors:
            print(f"  ✗ {e}", file=sys.stderr)
        sys.exit(1)

    rows = len(doc.get("rows") or [])
    sets = sum(len(r.get("sets") or []) for r in doc["rows"])

    if args.check:
        if not IPP_PATH.exists():
            sys.exit(f"{IPP_PATH.relative_to(HERE)} does not exist — run the generator")
        if IPP_PATH.read_text() != text:
            sys.exit(f"{IPP_PATH.relative_to(HERE)} is STALE — it does not match the .json; run the generator")
        print(f"ok: {rows} row(s) naming {sets} set(s); the .ipp matches the .json")
        return

    IPP_PATH.write_text(text)
    print(f"wrote {IPP_PATH.relative_to(HERE)} — {rows} row(s) naming {sets} set(s)")


if __name__ == "__main__":
    main()
