#!/usr/bin/env bash
# The A/B harness, as of P3 step 7.  Four runs per game and one report.
#
# bash, not zsh, and every environment assignment is passed as its own word: `env $VAR ...` does not
# word-split in zsh, so a run labelled `software` silently ran `vulkan` once.  See CLAUDE.md gotcha 7.
#
#   ./devnotes/ab.sh <game> [frames] [outdir]
#
# Environment:
#   MODE       extra env for all four runs, e.g. "M2VK_OPAQUE_ONLY=1" (step 4's opaque-path guard)
#              or "M2VK_FORCE_SOLID=2" (step 3's untextured numbers).  Applied to BOTH renderers,
#              which is the whole point of those switches.
#   CORE       core path, default ./modelizer_libretro.dylib
#   ROMS       rom directory, default devnotes/roms
#
# Writes <outdir>/<game>-{bg,3d}-{software,vulkan}.ppm, <game>-heat.png and <game>.txt, and prints
# the report.  Each run gets its own M2_SAVE_DIR: retrohost defaults to a shared ./retrohost-save
# and two runs in flight at once cross each other's NVRAM, which reads exactly like a renderer bug.

set -u

GAME=${1:?usage: ab.sh <game> [frames] [outdir]}
FRAMES=${2:-2500}
OUT=${3:-/tmp/ab}
CORE=${CORE:-./modelizer_libretro.dylib}
ROMS=${ROMS:-devnotes/roms}
MODE=${MODE:-}

mkdir -p "$OUT"
REPORT="$OUT/$GAME.txt"

{
	echo "game        $GAME"
	echo "frames      $FRAMES"
	echo "mode        ${MODE:-(none — whole frame, step 5+)}"
	echo "core        $CORE"
	echo "date        $(date '+%Y-%m-%d %H:%M:%S')"
	echo "head        $(git rev-parse --short HEAD 2>/dev/null || echo '?')"
	echo
} > "$REPORT"

for layer in bg 3d; do
	for r in software vulkan; do
		SAVE=$(mktemp -d)
		PPM="$OUT/$GAME-$layer-$r.ppm"
		LOG="$OUT/$GAME-$layer-$r.log"

		ENV=()
		[ -n "$MODE" ] && read -r -a ENV <<< "$MODE"
		[ "$layer" = bg ] && ENV+=(M2VK_NO_3D=1)
		ENV+=("M2_SAVE_DIR=$SAVE" "M2OPT_model2_renderer=$r")

		env "${ENV[@]}" \
			./devnotes/retrohost --vk "$CORE" "$ROMS/$GAME.zip" "$FRAMES" "$PPM" > "$LOG" 2>&1
		status=$?
		rm -rf "$SAVE"

		if [ $status -ne 0 ] || [ ! -s "$PPM" ]; then
			echo "$GAME $layer $r: retrohost failed (status $status), see $LOG" | tee -a "$REPORT"
			exit 1
		fi

		# Read the run's own account of itself rather than assuming it.  `options:` is the check
		# that the renderer under test is the renderer that ran.
		digest=$(sed -n 's/^digest: \([0-9a-f]*\) .*/\1/p' "$LOG")
		opts=$(sed -n 's/^.*\[model2\] options: //p' "$LOG")
		printf '%-3s %-9s digest %s   options: %s\n' "$layer" "$r" "${digest:-MISSING}" "${opts:-?}" \
			>> "$REPORT"
		eval "DIGEST_${layer}_${r}=\$digest"
	done
done

# Both renderers must produce the background reference bit-identically.  Every measurement below
# defines "covered" as "differs from that reference", so if this fails nothing after it means
# anything at all.
#
# Check the whole-run digest as well as the last frame, and not as belt-and-braces: the last-frame
# cmp passed on a lastbrnx run whose digests differed, so the frame the PPM happens to hold is a
# weaker test than every frame of the run.
echo >> "$REPORT"
if ! cmp -s "$OUT/$GAME-bg-software.ppm" "$OUT/$GAME-bg-vulkan.ppm"; then
	echo "BACKGROUND REFERENCE DIFFERS (last frame) — nothing below is meaningful" >> "$REPORT"
	cat "$REPORT"
	exit 1
fi
if [ "${DIGEST_bg_software:-a}" != "${DIGEST_bg_vulkan:-b}" ]; then
	echo "BACKGROUND REFERENCE DIFFERS (whole-run digest, last frame agrees) — rerun before" >> "$REPORT"
	echo "believing it; this has been seen to not reproduce.  Nothing below is meaningful." >> "$REPORT"
	cat "$REPORT"
	exit 1
fi
echo "background reference identical across renderers, last frame and whole-run digest" >> "$REPORT"
echo >> "$REPORT"

./devnotes/ppmdiff.py report \
	"$OUT/$GAME-bg-software.ppm" "$OUT/$GAME-3d-software.ppm" "$OUT/$GAME-3d-vulkan.ppm" \
	"$OUT/$GAME-heat.png" >> "$REPORT"
status=$?

cat "$REPORT"
exit $status
