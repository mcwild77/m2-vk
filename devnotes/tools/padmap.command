#!/bin/bash
#
# Double-click this in Finder to start the layout editor.  It opens a Terminal window, starts
# padmap-serve.py in it and launches the browser; ctrl-c or closing the window stops the server.
#
# The Desktop app ("Model 2 Pad Editor.app") is the same thing without the Terminal window — it runs the
# server in the background and the page's own "Stop server" button is how you stop it.  This file is the
# portable one: it lives in the repo, so a fresh clone has it and no absolute path is baked in.

cd "$(dirname "$0")/../.." || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
exec ./devnotes/tools/padmap-serve.py "$@"
