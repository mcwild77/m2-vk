#!/usr/bin/env bash
# Build the analog stick tester. Needs Homebrew's sdl2 (`brew install sdl2`).
set -euo pipefail
cd "$(dirname "$0")"
cc -O2 -Wall -o sticktest sticktest.c $(sdl2-config --cflags --libs) -lm
echo "built ./devnotes/tools/sticktest"
