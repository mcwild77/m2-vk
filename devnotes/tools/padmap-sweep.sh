#!/usr/bin/env bash
#
# Regenerates devnotes/tools/padmap-data.js — the layout editor's data source.
#
# For every Model 2 set whose ROMs are reachable it boots the core for a few frames with
# M2VK_INPUT_DUMP set and collects the machine's own description of its controls: every ioport field,
# its IPT_ token, and the driver's own PORT_NAME.  Those names are the reason the tool needs no
# explanation of which button does which — model2.cpp already says ("VR1 (Red)", "Hand Brake",
# "GEAR N"), and this is how they reach a browser.
#
# It also extracts the driver table itself from model2.cpp — set, parent, port set, and whether the
# entry is MACHINE_NOT_WORKING — so the tool can show the whole library and padmap-gen.py can refuse a
# row naming a set that does not exist.  That extraction is a grep over GAME()/GAMEL() lines, which is
# safe in a way that parsing INPUT_PORTS would not be: a GAME line is one line with fixed argument
# positions, while ports involve PORT_INCLUDE and PORT_MODIFY and would have to reimplement MAME's own
# resolution.  That is exactly what the boot-and-dump above avoids.
#
# #!/usr/bin/env bash and not zsh, deliberately: this script passes environment assignments as separate
# words and CLAUDE.md's gotcha 8 records that `env $e` does not word-split under zsh, which silently
# dropped a core option and made a run measure the wrong renderer.
#
# Runs are SEQUENTIAL and each gets its own M2_SAVE_DIR.  Both are load-bearing rather than tidy: the
# savestate work measured four concurrent invocations reporting a failure on a fixture that passes
# alone, and two runs sharing a save directory start from different emulated state.
#
# usage:  ./devnotes/tools/padmap-sweep.sh [set ...]
#         no arguments sweeps every set with reachable ROMs

set -u

here="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$here"

host="$here/devnotes/retrohost"
out="$here/devnotes/tools/padmap-data.js"

# Four driver families now, one editor, and — since the 2026-08-27 unification — ONE compiled-in core
# for all of them (modelizer_libretro.dylib; the per-family subtargets it replaced no longer exist as
# build scripts). The editor filters the library by the "driver" field this emits per set, so the family
# tag has to travel from the GAME table into padmap-data.js. Field positions in a GAME() line are
# identical across all four drivers (name $3, parent $4, input $6), so one awk serves all of them — only
# the source and the tag differ.
core_modelizer="$here/modelizer_libretro.dylib"
driver_model1="$here/src/mame/sega/model1.cpp"
driver_model2="$here/src/mame/sega/model2.cpp"
driver_system22="$here/src/mame/namco/namcos22.cpp"
# System 21 GAME entries are split across three driver files — src_for returns all three.
driver_system21="$here/src/mame/namco/namcos21.cpp $here/src/mame/namco/namcos21_c67.cpp $here/src/mame/namco/namcos21_de.cpp"
driver_system23="$here/src/mame/namco/namcos23.cpp"

# Model 1 and Model 2 ROMs both live under devnotes/roms (Model 2 also spills into the Polydiver set —
# roms.md gotcha 2); System 22 ROMs are all together under devnotes/roms/system22, so a clone finds its
# parent in the same directory (the core's first rompath is the zip's own dir — retro_entry.cpp
# rompath_from_path).
roms_local="$here/devnotes/roms"
roms_extra="/Users/mcwildmacbookair/Documents/GitHub/Polydiver/roms"
roms_system22="$here/devnotes/roms/system22"
roms_system23="$here/devnotes/roms/system23"

FAMILIES="model1 model2 system22 system21 system23"

# Per-family accessors, so the family tag is the only branch. One core for every family now — a missing
# core is still a warning, not a fatal, so a checkout with nothing built can list the library with no
# dumps. src_for may print MORE than one path (System 21 spans three driver files) — every consumer loops.
core_for()  { echo "$core_modelizer"; }
src_for()   { case $1 in model1) echo "$driver_model1";; model2) echo "$driver_model2";; system22) echo "$driver_system22";; system21) echo "$driver_system21";; system23) echo "$driver_system23";; esac; }
# The rom directories to search for a family, in order. Model 1 and Model 2: local then Polydiver.
# System 22 and System 21 both live under devnotes/roms/system22 (the S21 sets were dropped in alongside
# the S22 ones).
romdirs_for() { case $1 in
	model1)   echo "$roms_local" "$roms_extra";;
	model2)   echo "$roms_local" "$roms_extra";;
	system22) echo "$roms_system22";;
	system21) echo "$roms_system22";;
	system23) echo "$roms_system23";;
esac; }

frames=20
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

[ -e "$host" ] || { echo "missing: $host" >&2; exit 1; }
for fam in $FAMILIES; do
	c="$(core_for "$fam")"
	for s in $(src_for "$fam"); do
		[ -e "$s" ] || { echo "missing driver source for $fam: $s" >&2; exit 1; }
	done
	[ -e "$c" ] || echo "[sweep] no $fam core ($c) — its sets will list without dumps" >&2
done

# The second rompath, as retrohost wants it: M2_SYSTEM_DIR/<dir> where <dir>/model2 reaches the extra
# ROMs.  roms.md gotcha 2 — devnotes/roms alone audits 49/69 and does not have daytona.
sysdir="$work/system"
mkdir -p "$sysdir"
[ -d "$roms_extra" ] && ln -sfn "$roms_extra" "$sysdir/model2"

#-------------------------------------------------------------------------------------------------
# the driver table
#-------------------------------------------------------------------------------------------------
# GAME( year, name, parent, machine, input, class, init, monitor, company, fullname, flags )
#                   ^3      ^4       ^5
# Fields are counted after splitting on '(' and ',', so $3 is name, $4 parent, $6 input. One column
# more than before — the family — so a row carries which driver it came from all the way to the editor.
: > "$work/drivers.tsv"
for fam in $FAMILIES; do
	# A family may name more than one driver file (System 21) — sweep them all into the same table.
	for src in $(src_for "$fam"); do
		awk -F'[(,]' -v fam="$fam" '
			/^GAMEL?\(/ {
				name = $3; parent = $4; input = $6;
				gsub(/[ \t]/, "", name); gsub(/[ \t]/, "", parent); gsub(/[ \t]/, "", input);
				working = (index($0, "MACHINE_NOT_WORKING") > 0) ? "false" : "true";
				printf "%s\t%s\t%s\t%s\t%s\n", name, parent, input, working, fam;
			}' "$src" >> "$work/drivers.tsv"
	done
	n=$(awk -F'\t' -v f="$fam" '$5==f' "$work/drivers.tsv" | wc -l | tr -d ' ')
	echo "[sweep] $n GAME entries in $fam"
done

#-------------------------------------------------------------------------------------------------
# which sets to try
#-------------------------------------------------------------------------------------------------
# A set is attempted when <set>.zip exists in one of its family's rom dirs.  A clone also needs its
# parent's zip, which MAME finds on its own as long as both are on the path — so the check is on the
# set's own file and a missing parent shows up as a boot failure, recorded rather than hidden.
#
# The list is now "<set>\t<family>" rather than a bare name, because the core and rom dir to use depend
# on the family.  An explicit argument list looks its family up in the driver table.
rom_for() {  # $1 family, $2 set -> prints the zip path, or nothing
	local fam="$1" set="$2" dir
	for dir in $(romdirs_for "$fam"); do
		[ -f "$dir/$set.zip" ] && { echo "$dir/$set.zip"; return; }
	done
}

: > "$work/todo.tsv"
if [ "$#" -gt 0 ]; then
	for set in "$@"; do
		fam=$(awk -F'\t' -v s="$set" '$1==s {print $5; exit}' "$work/drivers.tsv")
		[ -z "$fam" ] && { echo "[sweep] $set: not in any driver table" >&2; continue; }
		printf '%s\t%s\n' "$set" "$fam" >> "$work/todo.tsv"
	done
else
	while IFS=$'\t' read -r name parent input working family; do
		[ -n "$(rom_for "$family" "$name")" ] && printf '%s\t%s\n' "$name" "$family" >> "$work/todo.tsv"
	done < "$work/drivers.tsv"
fi

#-------------------------------------------------------------------------------------------------
# the sweep
#-------------------------------------------------------------------------------------------------
ok=0
skipped=""
while IFS=$'\t' read -r set fam; do
	rom="$(rom_for "$fam" "$set")"
	if [ -z "$rom" ]; then
		echo "[sweep] $set: no zip in the $fam rom dir(s)"
		skipped="$skipped $set"
		continue
	fi
	core="$(core_for "$fam")"
	if [ ! -e "$core" ]; then
		echo "[sweep] $set: no $fam core built — listing without a dump"
		skipped="$skipped $set"
		continue
	fi

	dump="$work/$set.json"
	log="$work/$set.log"
	rm -rf "$work/save-$set"
	mkdir -p "$work/save-$set"

	env "M2VK_INPUT_DUMP=$dump" "M2_SYSTEM_DIR=$sysdir" "M2_SAVE_DIR=$work/save-$set" \
		"$host" "$core" "$rom" "$frames" /dev/null > "$log" 2>&1

	# The dump file existing is the test, not the exit status: a set can boot far enough to build its
	# ports and still leave retrohost unhappy about something later, and a set with missing ROMs never
	# reaches input_init at all.
	if [ ! -s "$dump" ]; then
		echo "[sweep] $set: no dump — $(grep -ioE 'device .* missing|NOT FOUND|fatal.*' "$log" | head -1)"
		skipped="$skipped $set"
		continue
	fi

	# 🚨 The zero-field case has to be caught here as well as in the core. It is what a dump taken
	# before ioport_manager::initialize() looks like, and the file is valid JSON, so nothing downstream
	# would object — see m2vk_inputdump.h's header.
	if grep -q '"fields": \[$' "$dump" && grep -q '^\s*\]$' "$dump" && ! grep -q '"token"' "$dump"; then
		echo "[sweep] $set: 🚨 ZERO fields — taken too early, not a set with no controls"
		skipped="$skipped $set"
		continue
	fi

	fields=$(grep -c '"token"' "$dump")
	echo "[sweep] $set: $fields fields"
	ok=$((ok + 1))
done < "$work/todo.tsv"

#-------------------------------------------------------------------------------------------------
# padmap-data.js
#-------------------------------------------------------------------------------------------------
# A window.* assignment loaded by a <script src>, not JSON loaded by fetch: the tool has to work from
# file://, where fetch is blocked by CORS and a script tag is not.
mkdir -p "$(dirname "$out")"
{
	echo "// Generated by devnotes/tools/padmap-sweep.sh — do not edit."
	echo "// The machine's own description of its controls, per set. Regenerate after an upstream merge."
	echo "window.M2_PADMAP_DATA = {"
	echo "  \"generated\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
	echo "  \"frames\": $frames,"
	echo "  \"drivers\": ["

	first=1
	while IFS=$'\t' read -r name parent input working family; do
		[ $first -eq 1 ] || echo ","
		first=0
		printf '    { "set": "%s", "parent": "%s", "input": "%s", "working": %s, "driver": "%s" }' \
			"$name" "$parent" "$input" "$working" "$family"
	done < "$work/drivers.tsv"
	echo ""
	echo "  ],"
	echo "  \"dumps\": {"

	first=1
	for dump in "$work"/*.json; do
		[ -s "$dump" ] || continue
		set="$(basename "$dump" .json)"
		[ $first -eq 1 ] || echo ","
		first=0
		printf '    "%s": ' "$set"
		# indented by hand rather than reformatted: the core writes it, and reformatting here would be
		# a second place the shape is described.
		sed 's/^/    /' "$dump" | sed '1s/^    //'
	done
	echo ""
	echo "  }"
	echo "};"
} > "$out"

echo "[sweep] $ok set(s) dumped -> $out"
[ -n "$skipped" ] && echo "[sweep] skipped:$skipped"
exit 0
