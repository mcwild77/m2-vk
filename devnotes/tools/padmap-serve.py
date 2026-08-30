#!/usr/bin/env python3
"""Serve padmap.html with a Save-to-core button, so authoring is one click instead of three steps.

    ./devnotes/tools/padmap-serve.py            # opens the editor in a browser
    ./devnotes/tools/padmap-serve.py --no-open  # just serve; print the URL

Without this, getting an edit into the core is: download the JSON into ~/Downloads, move it over
src/osd/libretro_m2/input_layouts.json, run padmap-gen.py, rebuild.  Four steps, three of which are
mechanical, and the download step is the one that silently goes wrong — a browser that keeps
input_layouts(2).json leaves you moving a stale file and wondering why the change did not take.

The editor still works straight off file:// with the Download button; nothing here is required.  When it
is loaded from this server it notices (GET /api/state answers) and grows two buttons instead.

WHY A SERVER AND NOT THE FILE SYSTEM ACCESS API: showSaveFilePicker() can write the JSON in place, but it
cannot run padmap-gen.py, and the generator is the half that matters — it is what refuses a broken table
and what derives the labels from the assignments.  A save that writes the JSON and leaves the .ipp stale
is worse than no button, because --check is then the only thing that knows and nobody runs it by reflex.

Local by construction: binds 127.0.0.1, refuses a POST without the X-Padmap header (so nothing a browser
can be tricked into submitting cross-origin reaches the writing endpoints), and serves devnotes/tools/
only.  It writes exactly one file and runs exactly three commands, all named below.

The third is RetroArch: a mapping cannot be checked any other way (CLAUDE.md bans scripted button-press
testing, so the exit criterion for every row is a person holding a pad), and the two ways of getting
there by hand both have a silent failure mode.  ~/Desktop/Model 2.app plays whatever core is installed in
RetroArch's cores directory, which is a symlink that has twice reverted to a stale copy; and a hand-typed
-L line is where an M2VK_* switch left over from a harness run beats the options menu without saying so.
So /api/play launches the repo's own dylib by path, with the M2VK_/M2OPT_ environment stripped, and
refuses outright when the built core is older than the table it is supposed to be carrying.
"""

import argparse
import errno
import http.server
import json
import os
import shutil
import subprocess
import sys
import threading
import urllib.request
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "devnotes/tools"
JSON_PATH = ROOT / "src/osd/libretro_m2/input_layouts.json"
IPP_PATH = ROOT / "src/osd/libretro_m2/input_layouts.ipp"
GEN = TOOLS / "padmap-gen.py"

BUILD_JOBS = os.cpu_count() or 4

def build_cmd(subtarget):
    return ["make", f"SUBTARGET={subtarget}", "OSD=libretro_m2", "NOWERROR=1", f"-j{BUILD_JOBS}"]

RETROARCH = Path("/Applications/RetroArch.app/Contents/MacOS/RetroArch")

# One editor, four driver families — so the ROM dir to search depends on which family the selected set
# belongs to.  Family is resolved from the set name via padmap-data.js (set_families()); the shared .ipp
# is compiled into the one core.
#
# Since the 2026-08-27 unification all four families build from ONE subtarget (modelizer) into ONE core
# (modelizer_libretro.dylib) — the per-family subtargets these used to name no longer exist as build
# scripts, so every row below points at the same core/subtarget; only the ROM dir differs.
#
# Model 1 and Model 2's dir is devnotes/roms, the ONE dir the harness and the Desktop app also read (so a
# set that plays here plays under ab.sh).  System 22's zips live together under devnotes/roms/system22 —
# a clone finds its parent in the same directory, which is the core's first rompath.
_CORE = ROOT / "modelizer_libretro.dylib"
_SUBTARGET = "modelizer"
FAMILIES = {
    "model1":   {"core": _CORE, "roms": ROOT / "devnotes/roms", "subtarget": _SUBTARGET},
    "model2":   {"core": _CORE, "roms": ROOT / "devnotes/roms", "subtarget": _SUBTARGET},
    "system22": {"core": _CORE, "roms": ROOT / "devnotes/roms/system22", "subtarget": _SUBTARGET},
    # System 21 sets were dropped in alongside the S22 ones, so they share devnotes/roms/system22.
    "system21": {"core": _CORE, "roms": ROOT / "devnotes/roms/system22", "subtarget": _SUBTARGET},
}
DEFAULT_FAMILY = "model2"

PLAY_LOG = Path("/tmp/m2vk-play.log")


def set_families():
    """{set name: family} read from padmap-data.js drivers[]; empty if the file is absent or unreadable.

    The single source of truth for which driver a set belongs to — the same file the editor filters its
    tabs on — so the server never keeps its own list to drift.  A set not found here is treated as the
    default family, which is what a checkout without padmap-data.js falls back to.
    """
    try:
        text = (TOOLS / "padmap-data.js").read_text()
        data = json.loads(text[text.index("{"):text.rindex("}") + 1])
    except (OSError, ValueError):
        return {}
    return {d["set"]: d.get("driver", DEFAULT_FAMILY) for d in data.get("drivers", [])}


def family_for(names):
    """The family of the first of `names` padmap-data.js knows, else the default."""
    fam = set_families()
    for n in names:
        if n in fam:
            return fam[n]
    return DEFAULT_FAMILY

_playing = None      # the RetroArch we launched, so a second click does not stack another one
_play_game = None

# What a dead RetroArch says about why, in the order a reader wants it.  A ROM audit failure is the
# common one and it is fatal within a few seconds of launch — the whole reason /api/playing exists.
PLAY_FAIL_MARKS = ("NOT FOUND", "WRONG CHECKSUMS", "Fatal error", "failed to start",
                   "[ERROR] [Content]", "NO GOOD DUMP", "INCORRECT LENGTH")

# An app launched from the Finder inherits launchd's PATH — /usr/bin:/bin:/usr/sbin:/sbin — and the build
# reaches for Homebrew (glslc lives there).  Prepending here rather than in the launcher means both the
# .app and a plain shell invocation get the same build environment.
BUILD_ENV = dict(os.environ)
BUILD_ENV["PATH"] = os.pathsep.join(
    ["/opt/homebrew/bin", "/usr/local/bin"] +
    [p for p in BUILD_ENV.get("PATH", "").split(os.pathsep)
     if p and p not in ("/opt/homebrew/bin", "/usr/local/bin")])


def run(cmd, **kw):
    """Run a command in the repo root and return (rc, combined output)."""
    p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors="replace", **kw)
    return p.returncode, p.stdout


def generate():
    return run([sys.executable, str(GEN)])


def ipp_is_current():
    rc, _ = run([sys.executable, str(GEN), "--check"])
    return rc == 0


def find_rom(names):
    """First (name, path, family) whose <family roms>/<name>.zip exists, walking the set then its clones.

    Searched in the set's own family first (so ridgera2 is looked for under devnotes/roms/system22, not
    the Model 2 dir), then the others as a fallback — a zip that only exists elsewhere still plays.
    """
    fams = [family_for(names)] + [f for f in FAMILIES if f != family_for(names)]
    for name in names:
        for fam in fams:
            p = FAMILIES[fam]["roms"] / f"{name}.zip"
            if p.exists():
                return name, p, fam
    return None, None, None


def all_rom_stems():
    """Every zip name across every family's rom dir — what the editor's Play button checks membership in."""
    stems = set()
    for cfg in FAMILIES.values():
        d = cfg["roms"]
        if d.is_dir():
            stems.update(p.stem for p in d.glob("*.zip"))
    return sorted(stems)


def play_failure():
    """Why the launched RetroArch died, out of its own log, or "" if it does not say.

    RetroArch exits within a few seconds when the content will not load, which is indistinguishable from
    a successful launch at the moment the button is clicked — so the page asks afterwards, and this is
    what it gets.  A ROM audit failure is by far the commonest: the core's second rompath is the
    frontend's system directory, and a clone whose parent set lives elsewhere fails there and nowhere
    else (retrohost is usually pointed at both).
    """
    try:
        text = PLAY_LOG.read_text(errors="replace")
    except OSError:
        return ""

    # 🚨 The core's own "started '<set>'" line is the discriminator, and without it this reports a game
    # you QUIT as a game that would not start: doa audits with a WRONG CHECKSUMS warning, runs perfectly,
    # and would come back "doa would not start — mpr-19324.19 WRONG CHECKSUMS:".  A machine that reached
    # that line ran; anything after it is the player closing the window.
    if "] started '" in text:
        return ""

    hits, seen = [], set()
    lines = text.splitlines()
    for ln in lines:
        s = ln.strip()
        if any(m in s for m in PLAY_FAIL_MARKS) and s not in seen:
            seen.add(s)
            hits.append(s)
    return "\n".join(hits[:8])


def play_env():
    """The environment a play session wants: the harness switches taken back out.

    M2VK_* beats the matching core option by design, and M2OPT_* pins one outright, so either left in the
    environment makes the options menu silently do nothing — which is the failure this button exists to
    avoid, and it looks exactly like a mapping that did not take.  Same list ~/Desktop/Model 2.app clears,
    computed by prefix instead of typed out, so a switch added later is covered without editing this.
    """
    return {k: v for k, v in os.environ.items()
            if not k.startswith("M2VK_") and not k.startswith("M2OPT_")}


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(TOOLS), **kw)

    # quieter: one line per API call, nothing for the static files
    def log_message(self, fmt, *args):
        if self.path.startswith("/api/"):
            sys.stderr.write("%s %s\n" % (self.command, self.path))

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/playing"):
            # Read-only, so no X-Padmap: it starts nothing and writes nothing.
            if _playing is None:
                return self._json(200, {"state": "idle"})
            rc = _playing.poll()
            if rc is None:
                return self._json(200, {"state": "running", "game": _play_game})
            return self._json(200, {"state": "exited", "game": _play_game, "rc": rc,
                                    "reason": play_failure(), "log": str(PLAY_LOG)})
        if self.path.startswith("/api/state"):
            doc = None
            if JSON_PATH.exists():
                try:
                    doc = json.loads(JSON_PATH.read_text())
                except json.JSONDecodeError as e:
                    return self._json(200, {"served": True, "doc": None,
                                            "error": f"{JSON_PATH.name} is not valid JSON: {e}"})
            return self._json(200, {
                "served": True,
                "doc": doc,
                "path": str(JSON_PATH.relative_to(ROOT)),
                "generated": ipp_is_current() if doc else False,
                "build": " ".join(build_cmd("<subtarget>")),
                "canplay": RETROARCH.exists(),
                "roms": all_rom_stems(),
            })
        return super().do_GET()

    def do_POST(self):
        # A cross-origin form post cannot set a custom header, and a cross-origin fetch that sets one is
        # preflighted — which this server answers for nothing.  So this check is the whole CSRF story.
        if self.headers.get("X-Padmap") != "1":
            return self._json(403, {"error": "missing X-Padmap header"})
        if self.path.startswith("/api/save"):
            return self.do_save()
        if self.path.startswith("/api/build"):
            return self.do_build()
        if self.path.startswith("/api/play"):
            return self.do_play()
        if self.path.startswith("/api/quit"):
            # Launched from the .app there is no Dock icon and no terminal to ctrl-c, so the page has to
            # be able to stop it.  shutdown() blocks until serve_forever returns, so it cannot be called
            # from the thread handling this request.
            self._json(200, {"ok": True})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        return self._json(404, {"error": "no such endpoint"})

    def do_save(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n).decode("utf-8", "replace")
        try:
            json.loads(raw)          # parsed only to reject a truncated POST; what gets written is `raw`
        except json.JSONDecodeError as e:
            return self._json(400, {"error": f"the editor sent invalid JSON: {e}"})
        if not raw.endswith("\n"):
            raw += "\n"

        # Write, generate, and put the old file back if the generator refuses.  The tree is never left
        # holding a JSON the .ipp does not match: that pair disagreeing is the one state --check exists
        # to catch, and a save button that can create it silently is worse than the manual steps.
        backup = JSON_PATH.read_bytes() if JSON_PATH.exists() else None
        JSON_PATH.parent.mkdir(parents=True, exist_ok=True)
        JSON_PATH.write_text(raw)
        rc, out = generate()
        if rc != 0:
            if backup is None:
                JSON_PATH.unlink(missing_ok=True)
            else:
                JSON_PATH.write_bytes(backup)
            return self._json(400, {"error": out.strip() or "padmap-gen.py refused the table",
                                    "reverted": True})
        return self._json(200, {"ok": True, "output": out.strip(),
                                "json": str(JSON_PATH.relative_to(ROOT)),
                                "ipp": str(IPP_PATH.relative_to(ROOT))})

    def do_play(self):
        """Launch RetroArch on this port set's game, with the core this repo just built.

        Refusals are the point of it: launching a core older than the table it is meant to be carrying is
        the one failure that looks like a bad mapping rather than a missed rebuild.  Each one is
        overridable from the page (force), because "I know, I want to look at something else" is a real
        answer and a button that argues back is worse than no button.
        """
        global _playing, _play_game
        n = int(self.headers.get("Content-Length") or 0)
        try:
            req = json.loads(self.rfile.read(n).decode("utf-8", "replace") or "{}")
        except json.JSONDecodeError as e:
            return self._json(400, {"error": f"bad request: {e}"})
        force = bool(req.get("force"))

        # The page sends the port set first, then its GAME entries, because it is the side that knows the
        # driver table.  daytona's zip is daytona.zip; a set whose own name has no dump (rchase2a) still
        # reaches a playable clone this way.
        names = [x for x in ([req.get("set")] + list(req.get("candidates") or [])) if x]
        if not names:
            return self._json(400, {"error": "no port set selected"})

        if not RETROARCH.exists():
            return self._json(400, {"error": f"RetroArch is not at {RETROARCH}"})

        # Find the zip first: which family it belongs to decides which ROM dir to search (every family
        # now launches the same modelizer_libretro.dylib — see the FAMILIES comment above).
        name, rom, fam = find_rom(names)
        if rom is None:
            where = " or ".join(str(FAMILIES[f]["roms"]) for f in FAMILIES)
            return self._json(400, {"error": f"no zip for {'/'.join(names)} in {where}"})
        core = FAMILIES[fam]["core"]
        if not core.exists():
            return self._json(400, {"error": f"no {core.name} in the repo — click Rebuild core "
                                             f"(builds SUBTARGET={FAMILIES[fam]['subtarget']})"})

        if not force:
            if not ipp_is_current():
                return self._json(409, {"error": "input_layouts.ipp does not match input_layouts.json — "
                                                 "click Save to core first", "force": True})
            if core.stat().st_mtime < IPP_PATH.stat().st_mtime:
                return self._json(409, {"error": f"the built {core.name} is older than input_layouts.ipp — "
                                                 "click Rebuild core first", "force": True})
            if _playing is not None and _playing.poll() is None:
                return self._json(409, {"error": "RetroArch is already running from here", "force": True})

        # start_new_session so it outlives Stop server and does not take ctrl-c in the launcher's Terminal
        # window.  Output goes to a FILE and not to devnull: RetroArch exits within seconds when the
        # content will not load, and with the output discarded that is indistinguishable from a game that
        # started — which is exactly how "playing doa" got reported for a run that never drew a frame.
        _play_game = name
        log = PLAY_LOG.open("w")
        try:
            _playing = subprocess.Popen([str(RETROARCH), "-v", "-L", str(core), str(rom)],
                                        cwd=ROOT, env=play_env(), start_new_session=True,
                                        stdout=log, stderr=subprocess.STDOUT)
        finally:
            log.close()
        return self._json(200, {"ok": True, "game": name, "rom": str(rom), "core": core.name,
                                "family": fam, "pid": _playing.pid, "log": str(PLAY_LOG)})

    def do_build(self):
        """Streams make's output as it arrives, ending with a line '__EXIT__ <rc>'.

        Every family builds SUBTARGET=modelizer now, so this always rebuilds the one core — the family
        lookup only exists because the page still sends {set} (and/or {family}) and a family not in
        FAMILIES has to fall back to the default rather than KeyError.
        """
        n = int(self.headers.get("Content-Length") or 0)
        try:
            req = json.loads(self.rfile.read(n).decode("utf-8", "replace") or "{}") if n else {}
        except json.JSONDecodeError:
            req = {}
        fam = req.get("family")
        if fam not in FAMILIES:
            fam = family_for([x for x in [req.get("set")] if x])
        cmd = build_cmd(FAMILIES[fam]["subtarget"])

        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        try:
            self.wfile.write((" ".join(cmd) + "\n").encode())
            self.wfile.flush()
            p = subprocess.Popen(cmd, cwd=ROOT, env=BUILD_ENV, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1)
            for line in p.stdout:
                self.wfile.write(line.encode())
                self.wfile.flush()
            rc = p.wait()
            self.wfile.write(f"__EXIT__ {rc}\n".encode())
        except BrokenPipeError:
            pass  # the page navigated away mid-build; make keeps going, which is fine


def already_serving(port):
    """Is one of these already on the port?  Answers True only if it is ours."""
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/state", timeout=2) as r:
            return json.loads(r.read()).get("served") is True
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8733)
    ap.add_argument("--no-open", action="store_true", help="do not launch a browser")
    args = ap.parse_args()

    if not shutil.which("make"):
        print("note: no make on PATH — the Rebuild button will fail", file=sys.stderr)

    url = f"http://127.0.0.1:{args.port}/padmap.html"

    # Starting a second one is what a double-click IS when you have forgotten the first, and it used to
    # end in a socket traceback out of the Python runtime — which reads as a broken tool, not as "it is
    # already open".  The .app probes the port for this reason; the probe belongs here instead, so the
    # .command, a shell invocation and the bundle all behave the same.
    if already_serving(args.port):
        print(f"already running on port {args.port} — opening the tab")
        if not args.no_open:
            webbrowser.open(url)
        return
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError as e:
        if e.errno != errno.EADDRINUSE:
            raise
        sys.exit(f"port {args.port} is taken by something that is not the layout editor.\n"
                 f"Free it, or start this with a different port:  {sys.argv[0]} --port {args.port + 1}")
    print(f"editor:  {url}")
    print(f"writes:  {JSON_PATH.relative_to(ROOT)}  (then {GEN.relative_to(ROOT)})")
    print(f"builds:  {' '.join(build_cmd('<model2|namcos22>'))}  (subtarget follows the selected set)")
    print("plays:   " + (f"{RETROARCH.name} -L <family core> <rom>  ["
                         + ", ".join(f"{f}: {FAMILIES[f]['core'].name}" for f in FAMILIES) + "]"
                         if RETROARCH.exists() else "(RetroArch not installed — Play button hidden)"))
    print("ctrl-c to stop")
    if not args.no_open:
        threading.Timer(0.3, webbrowser.open, [url]).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    print("stopped")


if __name__ == "__main__":
    main()
