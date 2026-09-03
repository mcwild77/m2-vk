#!/usr/bin/env bash
# The resolution-invariance harness, P4 step 2.  One game, the Vulkan renderer at 1x and at each
# supersample scale, and one report per scale.
#
# What it is asking.  poly.vert's header claims the depth path is resolution-invariant: the
# draw-order key is per polygon and carries no screen-space term, so rendering into an attachment n
# times larger should change which pixels a polygon covers and nothing about which polygon wins a
# pixel.  That was an argument.  This runs it.
#
# bash, not zsh, and every environment assignment is passed as its own word — see CLAUDE.md gotcha 7.
#
#   ./devnotes/res.sh <game> [frames] [scales] [outdir]
#   ./devnotes/res.sh vf2 2500 "2 4" /tmp/res
#   POINT=1 ./devnotes/res.sh vf2 2500 3 /tmp/res-point
#   MODE=M2VK_FORCE_SOLID=2 ./devnotes/res.sh vcop2 2500 3 /tmp/res-solid
#
# Environment:
#   POINT      1 to resolve by the centre subpixel rather than the box filter.  ODD SCALES ONLY:
#              only an odd scale has a subpixel whose centre is the 1x pixel's centre, and with one
#              the fragment shader runs at the same screen positions as the 1x render, so the two
#              pictures are comparable pixel for pixel.  That is the strong form of the check; the
#              box filter is the one the plan asked for and the one that says something about
#              coverage.
#   MODE       extra env for every run, e.g. "M2VK_FORCE_SOLID=2".  Applied to ALL of them, 1x
#              included, which is the whole point of those switches.
#   CORE       core path, default ./modelizer_libretro.{dylib,dll,so} for the host
#   ROMS       rom directory, default devnotes/roms
#
# Two things are checked before any picture is compared, and if either fails nothing after it means
# anything:
#
#   * the background reference — M2VK_NO_3D=1, the frame with the 3D layer switched off — must come
#     back BIT-IDENTICAL at every scale.  The 2D layers are uploaded at 1x and magnified by a NEAREST
#     sampler, so every subpixel of a pixel holds the same texel and any exact resolve returns it.
#     This is what validates the supersample plumbing itself: if the resolve, the viewport or the
#     upscale were wrong, the 2D would move and every 3D number below would be measuring that.
#   * the run's own `options:` line, so a run labelled vulkan is a run that ran vulkan.

set -u

GAME=${1:?usage: res.sh <game> [frames] [scales] [outdir]}
FRAMES=${2:-2500}
SCALES=${3:-2 4}
OUT=${4:-/tmp/res}
. "$(dirname "$0")/hostenv.sh"
CORE=${CORE:-./modelizer_libretro$CORE_EXT}
ROMS=${ROMS:-devnotes/roms}
MODE=${MODE:-}
POINT=${POINT:-}

mkdir -p "$OUT"
REPORT="$OUT/$GAME.txt"
SUFFIX=$([ -n "$POINT" ] && echo point || echo box)

{
	echo "game        $GAME"
	echo "frames      $FRAMES"
	echo "scales      1 $SCALES"
	echo "resolve     $SUFFIX"
	echo "mode        ${MODE:-(none)}"
	echo "core        $CORE"
	echo "date        $(date '+%Y-%m-%d %H:%M:%S')"
	echo "head        $(git rev-parse --short HEAD 2>/dev/null || echo '?')"
	echo
} > "$REPORT"

# <layer> <scale> -> writes $OUT/$GAME-<layer>-<scale>x[-point].ppm, echoes the digest
run() {
	local layer="$1" scale="$2"
	local tag="$scale x"
	local ppm log save status digest opts
	ppm="$OUT/$GAME-$layer-${scale}x$([ "$scale" != 1 ] && [ -n "$POINT" ] && echo -point).ppm"
	log="${ppm%.ppm}.log"
	save=$(mktemp -d)

	ENV=()
	[ -n "$MODE" ] && read -r -a ENV <<< "$MODE"
	[ "$layer" = bg ] && ENV+=(M2VK_NO_3D=1)
	if [ "$scale" != 1 ]; then
		ENV+=("M2VK_SS=$scale")
		[ -n "$POINT" ] && ENV+=(M2VK_SS_POINT=1)
	fi
	ENV+=("M2_SAVE_DIR=$(hostpath "$save")" "M2OPT_model2_renderer=vulkan")

	env "${ENV[@]}" ./devnotes/retrohost --vk "$CORE" "$ROMS/$GAME.zip" "$FRAMES" "$ppm" > "$log" 2>&1
	status=$?
	rm -rf "$save"

	if [ $status -ne 0 ] || [ ! -s "$ppm" ]; then
		echo "$GAME $layer ${scale}x: retrohost failed (status $status), see $log" | tee -a "$REPORT"
		exit 1
	fi

	# The core's own account of the run.  `supersample:` is absent at 1x and must be present above it:
	# a mistyped M2VK_SS is otherwise a silent 1x run that agrees with 1x perfectly.
	digest=$(sed -n 's/^digest: \([0-9a-f]*\) .*/\1/p' "$log")
	opts=$(sed -n 's/^.*\[model2\] options: //p' "$log")
	ss=$(sed -n 's/^.*vk: supersample: //p' "$log")
	if [ "$scale" != 1 ] && [ -z "$ss" ]; then
		echo "$GAME $layer ${scale}x: the core never reported a supersample — M2VK_SS did not take" \
			| tee -a "$REPORT"
		exit 1
	fi
	printf '%-3s %2sx  digest %s   %s\n' "$layer" "$scale" "${digest:-MISSING}" "${ss:-1x}" >> "$REPORT"
	[ "$layer" = bg ] && [ "$scale" = 1 ] && printf '        options: %s\n' "${opts:-?}" >> "$REPORT"
	echo "$digest"
}

BG1=$(run bg 1)
run 3d 1 > /dev/null

for scale in $SCALES; do
	echo >> "$REPORT"
	bg=$(run bg "$scale")
	run 3d "$scale" > /dev/null

	sfx=$([ -n "$POINT" ] && echo -point)
	if ! cmp -s "$OUT/$GAME-bg-1x.ppm" "$OUT/$GAME-bg-${scale}x$sfx.ppm"; then
		echo "BACKGROUND REFERENCE MOVED AT ${scale}x (last frame) — the 2D did not survive the" >> "$REPORT"
		echo "resolve, so nothing about the 3D can be read from this run." >> "$REPORT"
		cat "$REPORT"
		exit 1
	fi
	if [ "$bg" != "$BG1" ]; then
		echo "BACKGROUND REFERENCE MOVED AT ${scale}x (whole-run digest, last frame agrees)" >> "$REPORT"
		cat "$REPORT"
		exit 1
	fi
	echo "background reference identical to 1x at ${scale}x, last frame and whole-run digest" >> "$REPORT"
	echo >> "$REPORT"

	# A is the 1x render and B the supersampled one, so `covered by A only` is a pixel the 1x render
	# drew over and the supersampled one resolved back to the background — and the reverse for B.
	# Into its own file first, so the exit-criterion check below reads THIS scale's section rather
	# than a passing line an earlier scale left in the accumulating report.
	SECTION="$OUT/$GAME-${scale}x$sfx.section"
	./devnotes/ppmdiff.py report \
		"$OUT/$GAME-bg-1x.ppm" "$OUT/$GAME-3d-1x.ppm" "$OUT/$GAME-3d-${scale}x$sfx.ppm" \
		"$OUT/$GAME-heat-${scale}x$sfx.png" > "$SECTION"
	verdict=$?
	cat "$SECTION" >> "$REPORT"

	# ppmdiff's own verdict is written for two renderers that sample at the SAME points, and a box
	# resolve does not: the 2x subpixel centres are at +-0.25 and the 1x centre at +0.5 is not among
	# them, so a sliver narrower than half a pixel can be gained OR LOST.  desert loses a one-pixel
	# mast that way — 7 pixels at 2x, 1 at 4x, 0 at 3x point.  So a box run is judged on the
	# background reference and exit criterion 1, which are unconditional, and ppmdiff's reading is
	# printed as information.  Under the point resolve the sample points ARE shared and the verdict
	# is meaningful, so it is enforced.
	if [ -n "$POINT" ]; then
		[ $verdict -ne 0 ] && FAILED=1
		echo "verdict: ppmdiff's interior test enforced (point resolve shares the 1x sample point)" \
			>> "$REPORT"
	else
		echo "verdict: ppmdiff's interior test NOT enforced — a box resolve moves the sample points," \
			>> "$REPORT"
		echo "so an isolated fringe pixel either way is expected.  Use POINT=1 with an odd scale to" >> "$REPORT"
		echo "claim invariance; this run says what supersampling looks like.  (ppmdiff said $verdict.)" >> "$REPORT"
	fi
	# Unconditional at every scale and both resolves: a pixel neither render's 3D touched must still
	# be bit-exact, or the composite has moved and nothing above is about the 3D.
	if ! grep -q "exit criterion 1 holds" "$SECTION"; then
		echo "EXIT CRITERION 1 FAILED at ${scale}x" >> "$REPORT"
		FAILED=1
	fi
	rm -f "$SECTION"
done

cat "$REPORT"
exit ${FAILED:-0}
