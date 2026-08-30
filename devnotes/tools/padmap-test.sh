#!/usr/bin/env bash
#
# Headless smoke test for padmap.html — the layout editor's logic, without a browser.
#
# It exists because the editor's job is to make a layout row that CANNOT be wrong, and the two rules it
# enforces (no numbered button on the D-pad of a set with a joystick; no numbered button on a trigger
# threshold of a set with pedals) are exactly the two that are invisible until somebody plays the game.
# A rule that silently stops firing is worse than no rule, so the rules are tested rather than trusted.
#
# It found two real problems on its first run: relabelFromRow was not idempotent — it appended to the
# analog label slots without clearing them, so a second render turned daytona's steering into
# "Steering / Steering / Steering / Stick X" — and the seeded row's slot coverage was not what the plan
# had assumed. Neither is visible by reading.
#
# jsc is macOS's own JavaScriptCore shell and is always present; there is no node here. The shim below is
# the smallest DOM that lets render() run, so the render path is exercised too and not just the pure
# functions.
#
# usage:  ./devnotes/tools/padmap-test.sh

set -eu

here="$(cd "$(dirname "$0")/../.." && pwd)"
jsc="/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc"
tool="$here/devnotes/tools/padmap.html"
data="$here/devnotes/tools/padmap-data.js"
tests="$here/devnotes/tools/padmap-test.js"

for f in "$jsc" "$tool" "$data" "$tests"; do
	[ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The tool's script block, the data, and the tests are concatenated into ONE file and run as global code.
# Separate eval()s do not work: padmap.html's script opens with "use strict", which makes its top-level
# const declarations invisible to a later eval in the same scope — the tests would report every name as
# undefined and read as a broken tool. Concatenated, the directive is no longer a prologue and every
# declaration lands in one shared scope.
python3 - "$tool" "$data" "$tests" "$work/combined.js" <<'PY'
import re, sys
tool, data, tests, out = sys.argv[1:5]
blocks = re.findall(r'<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>', open(tool).read(), re.S)
if not blocks:
	sys.exit("no inline <script> found in " + tool)
script = max(blocks, key=len)

shim_and_test = open(tests).read()
marker = "// --- the real data, then the real script ---"
if marker not in shim_and_test:
	sys.exit("marker missing from " + tests)
head, tail = shim_and_test.split(marker, 1)
tail = tail.split("\n", 3)[3]          # drop the two eval lines the marker introduces

open(out, "w").write("\n".join([head, open(data).read(), script, tail]))
PY

output="$("$jsc" "$work/combined.js" 2>&1)"
echo "$output"
case "$output" in
	*"all checks passed"*) exit 0 ;;
	*) exit 1 ;;
esac
