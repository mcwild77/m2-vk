#!/usr/bin/env bash
#
# ============================================================================================
# 🚫 RETIRED 2026-09-04 — SAVESTATES ARE DISABLED CORE-WIDE. THIS HARNESS CANNOT PASS.
#
# retro_serialize_size() now returns 0 for every family, so every arm below fails for the trivial
# reason. Do not run this, do not cite it, do not gate a change on it. Kept for the method only.
# See the savestates section of CLAUDE.md and the banner at the top of devnotes/reference/savestates.md.
# ============================================================================================
#

echo "state.sh is RETIRED: savestates are disabled core-wide (retro_serialize_size returns 0)." >&2
echo "Nothing here can pass. See CLAUDE.md, section 'Savestates are DISABLED'." >&2
echo "Set STATE_SH_I_KNOW=1 to run it anyway." >&2
[ -n "$STATE_SH_I_KNOW" ] || exit 2
#
# The savestate harness. devnotes/reference/savestates.md §3 step 3 is what this is for.
#
#   ./devnotes/state.sh <game> [frames] [savepoint] [outdir]
#   VK=1 ./devnotes/state.sh vf2 3000 1500 /tmp/state      # same test through the Vulkan path
#   SCRIPT="600:select:20,..." ./devnotes/state.sh daytona  # override the dirtying input
#
# 🚨 The obvious savestate test is VACUOUS and this one is built to avoid it. "Save at N, load at N
# in the same run, digests match" would pass with a retro_unserialize that restored nothing at all —
# the state being loaded is the state that was already there. So this loads a state taken from a
# DIFFERENT machine history and asks whether the future matches THAT history's future:
#
#   N  clean history, no load, digest from N+1        -- the negative control
#   D  dirty history (scripted input), save at N AND digest from N+1
#   C  CLEAN history, load D's dirty state at N, digest from N+1
#   E  a SECOND dirty history, digest from N+1        -- the determinism control
#
#   PASS requires BOTH:   C == D   (the state carried the whole machine)
#                 and     N != D   (the script actually diverged the machine)
#
# ⚠️ Without the second check the first is worthless, and it is not hypothetical: a script whose
# buttons the game ignores at that moment leaves N == D, at which point C == D is guaranteed however
# broken the savestate is. That is the failure this file exists to make impossible to miss.
#
# 🚨 D SAVES THE STATE AND SUPPLIES THE REFERENCE FUTURE IN ONE RUN, and that is load-bearing rather
# than a tidy-up. It used to be two runs — a short `B` that wrote the state file and a separate `D`
# that produced the digest — and the whole comparison silently assumed those two runs were the same
# machine history. On 2026-07-29 that assumption was measured and it is false: two runs of one
# identical command can land on DIFFERENT emulated frames (see the E control below), at which point C
# is asked to reproduce a future that belongs to a history its state never came from, and no
# savestate however perfect can pass. Do not split these apart again.
#
# 🚨 E is the control that catches what is left, and it is the run you expect to be boring. If
# E != D then the game's own future is not reproducible run-to-run, so a C != D verdict means
# nothing at all — it is a coin flip being read as a savestate bug. This script therefore reports
# NONDETERMINISTIC rather than FAIL in that case.
#
# Two more runs cover the weaker property that a save must not PERTURB the machine — presave and
# postload callbacks run real device code:
#
#   A  clean, no savestate activity at all
#   R  clean, serialize + immediately unserialize the same bytes at N
#   PASS requires  A == R
#
# Every run gets its own M2_SAVE_DIR: two runs sharing one NVRAM tree start from different emulated
# state and it reads exactly like a savestate bug (the P3 step 4 gotcha, worth 17127 pixels once).

set -u

GAME=${1:-vf2}
FRAMES=${2:-3000}
SAVEPOINT=${3:-1500}
OUT=${4:-/tmp/state}

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
# CORE defaults to the Model 2 build; override for the Namco cores, e.g.
#   CORE=$PWD/modelizer_libretro.dylib ./devnotes/state.sh winrun   (.dll on Windows, .so on Linux)
. "$HERE/hostenv.sh"
CORE=${CORE:-$ROOT/modelizer_libretro$CORE_EXT}
HOST=$HERE/retrohost$EXE
# The System 21/22 sets live in roms/system22/ (which also carries their C67/C68 BIOS, so the
# containing dir becoming the rompath self-satisfies them). Fall back to it when the flat dir misses.
ROM=$HERE/roms/$GAME.zip
[ -f "$ROM" ] || ROM=$HERE/roms/system22/$GAME.zip
[ -f "$ROM" ] || ROM=$HERE/roms/model1/$GAME.zip

# The second rompath, which daytona and doaa need (CLAUDE.md gotcha 2). Harmless when unused.
: "${M2_SYSTEM_DIR:=/tmp/m2sys}"
mkdir -p "$M2_SYSTEM_DIR"
M2_SYSTEM_DIR=$(hostpath "$M2_SYSTEM_DIR")
export M2_SYSTEM_DIR

# Coin, start, start again, then two face buttons — enough to take a game out of attract and put it
# somewhere with its own state. Override with SCRIPT= for a game this does not move.
: "${SCRIPT:=600:select:20,900:start:20,1100:start:20,1300:a:20,1400:b:20}"

# ⚠️ Not an array. macOS ships bash 3.2, where expanding an EMPTY array under `set -u` is an
# "unbound variable" error, so the usual VKFLAG=() / "${VKFLAG[@]}" idiom aborts every software-path
# run on this machine.
USE_VK=0
[ "${VK:-0}" != "0" ] && USE_VK=1

[ -f "$ROM" ]  || { echo "no rom: $ROM" >&2; exit 2; }
[ -x "$HOST" ] || { echo "no retrohost: $HOST (run devnotes/build-retrohost.sh)" >&2; exit 2; }
mkdir -p "$OUT"

DIGEST_FROM=$((SAVEPOINT + 1))
STATE=$OUT/$GAME-dirty.state
REPORT=$OUT/$GAME.txt

# run <tag> <frames> <dirty:0|1> [env assignments...]
#
# Env assignments are passed to `env` as SEPARATE WORDS. Building them as one string and expanding it
# unquoted does not word-split in zsh, so the assignments silently vanish and the run measures the
# default — which looks exactly like the change under test having no effect (CLAUDE.md gotcha 7).
run() {
	local tag=$1 frames=$2 dirty=$3; shift 3
	local log=$OUT/$GAME-$tag.log
	local -a args
	if [ "$USE_VK" = "1" ]; then
		args=("$HOST" --vk "$CORE" "$ROM" "$frames" "$OUT/$GAME-$tag.ppm")
	else
		args=("$HOST" "$CORE" "$ROM" "$frames" "$OUT/$GAME-$tag.ppm")
	fi
	[ "$dirty" = "1" ] && args[${#args[@]}]=$SCRIPT
	# 🚨 Keyed on the GAME as well as the tag. Without the game in the path, running two games into
	# one output directory gives vcop2's run-A the NVRAM daytona's run-A left behind, so the machine
	# boots from someone else's saved state — and it reads exactly like an incomplete savestate
	# registry. That is CLAUDE.md gotcha 7, and this file quoted it in its own header and then got it
	# wrong anyway: it produced three false FAILs before it was caught.
	env M2_SAVE_DIR="$(hostpath "$OUT/save-$GAME-$tag")" "$@" "${args[@]}" >"$log" 2>&1
	# The core's own options line, because a run labelled one thing and configured another has
	# passed unnoticed here before.
	grep -q "\[model2\] options:" "$log" || echo "  ⚠️  $tag: no options line in the log" >&2
	sed -n 's/^digest: \([0-9a-f]*\).*/\1/p' "$log" | tail -1
}

{
echo "savestate harness: $GAME, $FRAMES frames, save point $SAVEPOINT, digest from $DIGEST_FROM"
echo "renderer path: $([ "$USE_VK" = "1" ] && echo 'vulkan (--vk)' || echo software)"
echo "dirtying script: $SCRIPT"
echo

# D first — C depends on its state file, and taking both from THIS run is the point (see the header).
D=$(run D "$FRAMES" 1 M2VK_HOST_DIGEST_FROM="$DIGEST_FROM" M2VK_HOST_SAVE_AT="$SAVEPOINT:$(hostpath "$STATE")")
if [ ! -s "$STATE" ]; then
	echo "FAIL: no state file was written — read $OUT/$GAME-D.log"
	echo "exit 1"
	exit 1
fi
echo "state file: $(wc -c <"$STATE" | tr -d ' ') bytes"
echo

A=$(run A "$FRAMES" 0)
R=$(run R "$FRAMES" 0 M2VK_HOST_ROUNDTRIP_AT="$SAVEPOINT")
N=$(run N "$FRAMES" 0 M2VK_HOST_DIGEST_FROM="$DIGEST_FROM")
E=$(run E "$FRAMES" 1 M2VK_HOST_DIGEST_FROM="$DIGEST_FROM")
C=$(run C "$FRAMES" 0 M2VK_HOST_DIGEST_FROM="$DIGEST_FROM" M2VK_HOST_LOAD_AT="$SAVEPOINT:$(hostpath "$STATE")")

echo "A  clean, no savestate activity           $A"
echo "R  clean, round-trip in place at $SAVEPOINT      $R"
echo "N  clean, digest from $DIGEST_FROM               $N   (negative control)"
echo "D  dirty, saved at $SAVEPOINT, digest from $DIGEST_FROM  $D"
echo "E  dirty again, digest from $DIGEST_FROM         $E   (determinism control)"
echo "C  clean + dirty state loaded at $SAVEPOINT      $C"
echo

fail=0
if [ -z "$A" ] || [ -z "$C" ] || [ -z "$D" ] || [ -z "$E" ] || [ -z "$N" ] || [ -z "$R" ]; then
	echo "FAIL: a run produced no digest — read the logs in $OUT"
	fail=1
elif [ "$E" != "$D" ]; then
	# 🚨 Not a FAIL, and saying FAIL here is the mistake this branch exists to prevent. Two runs of
	# the identical dirty command produced different futures, so "C did not reproduce D" carries no
	# information about the savestate — C would have to guess which of the game's own two futures D
	# happened to take. Fix the nondeterminism first; the verdict below is unreadable until then.
	echo "🚨 NONDETERMINISTIC: two runs of the same dirty command disagree (D != E)."
	echo "   The reference future is not reproducible, so no verdict about the savestate is possible."
	echo "   C == D would be luck and C != D would be meaningless. Do not record either."
	fail=1
else
	echo "ok   the dirty history is reproducible (D == E)"
	if [ "$N" = "$D" ]; then
		echo "🚨 VACUOUS: the dirtying script changed nothing (N == D), so C == D proves nothing."
		echo "   Pick a SCRIPT= this game responds to before believing any result below."
		fail=1
	else
		echo "ok   the script diverged the machine (N != D)"
	fi
	if [ "$C" = "$D" ]; then
		echo "PASS the loaded state reproduced the dirty history's future exactly (C == D)"
	else
		echo "FAIL the loaded state did NOT reproduce the dirty future (C != D)"
		echo "     something the machine depends on is not in the save registry."
		fail=1
	fi
	if [ "$A" = "$R" ]; then
		echo "ok   saving does not perturb the machine (A == R)"
	else
		echo "FAIL saving perturbs the machine (A != R) — presave/postload has a side effect"
		fail=1
	fi
fi

echo
echo "exit $fail"
} 2>&1 | tee "$REPORT"

grep -q "^exit 0" "$REPORT"
