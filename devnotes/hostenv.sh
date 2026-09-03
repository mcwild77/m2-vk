# Sourced by the harness scripts (ab.sh, res.sh, perf.sh, state.sh).  Not executable on its own.
#
# One thing lives here: the two names that differ between the hosts this harness runs on.  The
# scripts were written on macOS and said ".dylib" in four places; that is the only reason they did
# not run on Windows, and a copy of this case statement in each of them is three copies too many.
#
#   CORE_EXT   the core's shared-library extension, .dylib / .dll / .so
#   EXE        the host executable suffix, "" or ".exe"
#
# ⚠️ EXE matters for the `[ -x ... ]` guards, not for running the thing: MSYS2's bash appends .exe
# when it execs a command, but the test builtin does not, so a guard written without it reports "no
# retrohost" about a retrohost that is sitting right there.

case "$(uname -s)" in
	Darwin)
		CORE_EXT=.dylib
		EXE=
		;;
	MINGW*|MSYS*|CYGWIN*)
		CORE_EXT=.dll
		EXE=.exe
		;;
	*)
		CORE_EXT=.so
		EXE=
		;;
esac

# hostpath <posix path> -> the form the harness binaries actually understand.
#
# 🚨 This exists because of one silent failure.  retrohost and the core are NATIVE Windows programs
# run from an MSYS2 shell, and MSYS2 rewrites POSIX paths in COMMAND-LINE ARGUMENTS but not in
# ENVIRONMENT VARIABLES.  So `M2_SAVE_DIR=/tmp/ab/save-vf2` arrives at the core as that literal
# string, native fopen resolves it against the current drive's root (E:\tmp\...), and the write
# fails -- with no error anywhere except a state file that is not there.  The run still completes
# and still prints a digest, which is exactly the kind of green run this project has been burned by.
#
# Command-line arguments do not need this.  Anything going into an env var that the core or
# retrohost will open does.
hostpath() {
	if [ -n "$EXE" ] && command -v cygpath >/dev/null 2>&1; then
		cygpath -m "$1"
	else
		printf '%s' "$1"
	fi
}
