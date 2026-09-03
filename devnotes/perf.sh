#!/usr/bin/env bash
# The performance harness — performance.md §8's row, which P3 step 8 was assigned and did not build.
#
# bash, not zsh, for the same reason ab.sh is: `env $VAR ...` does not word-split in zsh and a run
# labelled `software` once silently ran `vulkan`.  See CLAUDE.md gotcha 7.
#
#   ./devnotes/perf.sh <game> [frames] [skip] [outdir]
#
# Three configurations per game, three repeats each, and the spread reported — §7 says a wall-clock
# number without a variance estimate cannot tell contention from signal, so this never prints a
# single figure.  The three are chosen so that each pair differs in exactly one thing:
#
#   emul   renderer=software, no --vk, M2VK_NO_3D=1   the emulation thread ALONE.  §6 answered the
#                                                     gate — MAME interprets the i960, there is no
#                                                     recompiler — so this is the ceiling nothing
#                                                     in §4 can raise, and it is taken first.
#   sw     renderer=software, no --vk                 the above plus MAME's software rasterizer.
#                                                     emul→sw is what P3 removed from the CPU.
#   vk     renderer=vulkan,   --vk                    the shipping path.  sw→vk is P3's real result.
#
# There is deliberately no `renderer=software` **under** `--vk`: with that option the core never
# declares RETRO_HW_CONTEXT_VULKAN at all (no `hw render:` line, and RetroArch likewise never logs
# SET_HW_RENDER), so the frontend's device is never created and the run is identical to plain `sw`.
# [measured] — it produced the same three timings to the millisecond and a gpuwait of exactly 0.
# That is convenient rather than a limitation: it means `sw` carries no frontend cost for `vk` to be
# unfairly charged against, so sw→vk needs no correction term.
#
# `skip` drops the boot from the headline: every game runs some number of frames before it submits
# geometry (vf2 ~990), and averaging those in measures the boot rather than the renderer.  The
# default is 1000.  ⚠️ It is a guess until it is checked — read the per-bucket table in the saved
# log and confirm the skip lands past the plateau, or the headline describes the wrong half of the
# run.  The table prints for the whole run regardless, which is what makes the check possible.
#
# ⚠️ **Two speed columns, and quoting one of them is wrong.**  This harness reads every frame back
# off the GPU and so cannot overlap CPU and GPU at all; a real frontend does.  `serial%` is
# core+gpuwait, the lower bound this harness achieves.  `pipe%` is max(core, gpuwait), the upper
# bound a pipelined frontend would reach.  The truth is between them and nearer `pipe%`, because
# gpuwait also covers our own whole-frame image→buffer copy.  On the serial figure alone the Vulkan
# path reads as *slower* than the software rasterizer on vcop2, which it is not.  Both columns
# exclude the read-back copy and the whole-run digest — ~0.55 ms/frame at 496x384, which no frontend
# pays; that is what `host_ms` is.  See the perf-timer comment in retrohost.c.
#
# ⚠️ Before running: `ps` for retrohost / RetroArch / make.  Everything ab.sh measures is
# deterministic pixel output and immune to contention; nothing here is.  §7.

set -u

GAME=${1:?usage: perf.sh <game> [frames] [skip] [outdir]}
FRAMES=${2:-2500}
SKIP=${3:-1000}
OUT=${4:-/tmp/perf}
. "$(dirname "$0")/hostenv.sh"
CORE=${CORE:-./modelizer_libretro$CORE_EXT}
ROMS=${ROMS:-devnotes/roms}
REPEATS=${REPEATS:-3}
MODE=${MODE:-}

mkdir -p "$OUT"
REPORT="$OUT/$GAME.txt"

# "is anything else running" — the process listing differs per host, and on Windows the interesting
# processes are native ones that MSYS2's own `ps` does not see without -W.
case "$(uname -s)" in
	Darwin)               proclist='ps -Ac -o comm=' ;;
	MINGW*|MSYS*|CYGWIN*) proclist='ps -W' ;;
	*)                    proclist='ps -A -o comm=' ;;
esac
busy=$($proclist 2>/dev/null | grep -Ec 'retrohost|RetroArch|mamemodel|modelizer|make' || true)
{
	echo "game        $GAME"
	echo "frames      $FRAMES  (headline over the last $((FRAMES - SKIP)), skipping $SKIP)"
	echo "repeats     $REPEATS"
	echo "mode        ${MODE:-(none)}"
	echo "core        $CORE"
	echo "date        $(date '+%Y-%m-%d %H:%M:%S')"
	echo "head        $(git rev-parse --short HEAD 2>/dev/null || echo '?')"
	echo "load        $(uptime 2>/dev/null | sed 's/.*averages*: //' || echo '(no uptime on this host)')"
	[ "$busy" -gt 0 ] && echo "⚠️  OTHER RUNS IN FLIGHT ($busy) — these numbers are contended, see performance.md §7"
	echo
	printf '%-6s %-9s %-9s %-9s %-9s %-9s %-9s %s\n' \
		config serial% pipe% core_ms gpuwait_ms host_ms spread% "(pipe%, n=$REPEATS)"
} > "$REPORT"

run_config() {
	local tag=$1 vk=$2 renderer=$3 extra=$4
	local serials=() pipes=() cores=() gpus=() hosts=()

	for i in $(seq 1 "$REPEATS"); do
		local SAVE; SAVE=$(mktemp -d)
		local LOG="$OUT/$GAME-$tag-$i.log"

		local ENV=()
		[ -n "$MODE" ] && read -r -a ENV <<< "$MODE"
		[ -n "$extra" ] && ENV+=("$extra")
		ENV+=(M2VK_HOST_PERF=1 "M2VK_HOST_PERF_SKIP=$SKIP" "M2_SAVE_DIR=$(hostpath "$SAVE")"
		      "M2OPT_model2_renderer=$renderer")

		# `--vk` or nothing, spelled without an empty array: bash 3.2 is what /bin/bash is here and
		# `"${ARGS[@]}"` on an empty array is an unbound-variable error under `set -u`.
		local status
		if [ "$vk" = vk ]; then
			env "${ENV[@]}" ./devnotes/retrohost --vk "$CORE" "$ROMS/$GAME.zip" \
				"$FRAMES" /dev/null > "$LOG" 2>&1
			status=$?
		else
			env "${ENV[@]}" ./devnotes/retrohost "$CORE" "$ROMS/$GAME.zip" \
				"$FRAMES" /dev/null > "$LOG" 2>&1
			status=$?
		fi
		rm -rf "$SAVE"

		if [ $status -ne 0 ]; then
			echo "$GAME $tag repeat $i: retrohost failed (status $status), see $LOG" | tee -a "$REPORT"
			exit 1
		fi
		# The run's own account of which renderer ran.  A config that silently ran the other one
		# looks exactly like the result this harness exists to find.
		if ! grep -q "model2_renderer=$renderer" "$LOG"; then
			echo "$GAME $tag repeat $i: options line does not say renderer=$renderer" | tee -a "$REPORT"
			exit 1
		fi

		serials+=("$(sed -n 's/^perf: speed \([0-9.]*\) % serial.*/\1/p' "$LOG")")
		pipes+=("$(sed -n 's/^perf: speed.*\.\. \([0-9.]*\) % pipelined.*/\1/p' "$LOG")")
		cores+=("$(sed -n 's/^perf:   core \([0-9.]*\) ms.*/\1/p' "$LOG")")
		gpus+=("$(sed -n 's/^perf:.*gpuwait \([0-9.]*\) ms\/frame   host.*/\1/p' "$LOG")")
		hosts+=("$(sed -n 's/^perf:.*host \([0-9.]*\) ms\/frame$/\1/p' "$LOG")")
	done

	# Mean, and the spread as (max-min)/mean over the *pipelined* figure.  §7's protocol: the spread
	# is what says whether the mean is worth quoting, so it is printed next to it and never alone.
	python3 - "$tag" "${serials[*]}" "${pipes[*]}" "${cores[*]}" "${gpus[*]}" "${hosts[*]}" \
		<<-'PY' >> "$REPORT"
		import sys
		tag = sys.argv[1]
		cols = [[float(v) for v in a.split()] for a in sys.argv[2:7]]
		p = cols[1]
		spread = 100.0 * (max(p) - min(p)) / (sum(p) / len(p)) if p else 0.0
		m = [sum(c) / len(c) for c in cols]
		print(f"{tag:<6} {m[0]:<9.1f} {m[1]:<9.1f} {m[2]:<9.3f} {m[3]:<9.3f} {m[4]:<9.3f} "
		      f"{spread:<9.1f}[{' '.join(f'{v:.1f}' for v in p)}]")
	PY
}

run_config emul sw software M2VK_NO_3D=1
run_config sw   sw software ""
run_config vk   vk vulkan   ""

{
	echo
	echo "emul is the ceiling: two interpreted i960s, the copro DSPs and a 68000, with no renderer"
	echo "of any kind.  Nothing in performance.md §4 touches it.  Read the per-bucket table in"
	echo "$OUT/$GAME-vk-1.log and check the skip of $SKIP lands past the boot plateau."
} >> "$REPORT"

cat "$REPORT"
