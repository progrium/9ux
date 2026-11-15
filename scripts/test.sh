#!/bin/sh
# Build 9/UX and run its tests: scripts/test.sh [layer...] [-- zig build args]
#
# Layers, cheapest first (default: all of them, in this order):
#
#   build   mk in every component (inside h9)
#   host    `make test` in every component with a host Makefile (parsers
#           built for the host with sanitizers, such as resfs)
#   unit    `mk test` in every component, and in every test/*/ directory
#           with a mkfile (unit and rc interface tests, inside h9; 9front's
#           mkone runs a component's test/mkfile for it)
#   system  every executable */test/*_test.sh and test/*/*_test.sh, run on
#           the host from the repository root after the build (tests that
#           drive several h9 machines, such as a --serve server and a
#           client); exit status 0 is a pass
#   gui     the Zig harness's tests (test/harness: test/rio/*_test.zig, ...)
#
# Arguments after -- go to `zig build test` for the gui layer, such as
# -Dfilter=window or -Dupdate-golden.
#
# The first run sets up h9 and its root (scripts/h9.sh --setup); nothing
# else needs installing but a C compiler, curl, and python3.
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top"

layers=
while [ $# -gt 0 ]; do
	case $1 in
	--) shift; break ;;
	build|host|unit|system|gui) layers="$layers $1" ;;
	-h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "test.sh: unknown layer '$1' (build, host, unit, system, gui)" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$layers" ] || layers="build host unit system gui"

h9() { "$top/scripts/h9.sh" "$@"; }

# A unit run that hangs (a server that broke and holds a mount its test
# then waits on forever) fails after this many seconds instead of
# running into the CI job's timeout.
# (macOS has no timeout(1) unless coreutils is installed; without it,
# no limit.)
unit_timeout=${UNIT_TIMEOUT:-600}
limit() {
	if command -v timeout >/dev/null; then
		timeout -k 10 "$unit_timeout" "$@"
	else
		"$@"
	fi
}

failed=
passed=
note() { printf '\n=== %s\n' "$*"; }
result() { # name status
	if [ "$2" -eq 0 ]; then passed="$passed $1"; else failed="$failed $1"; fi
}

note "setup: h9 at the pinned revision, and its root"
if ! h9 --setup; then
	echo "test.sh: h9 setup failed" >&2
	exit 1
fi

for layer in $layers; do
	case $layer in
	build)
		note "build: mk"
		h9 --rc 'mk'
		result build $?
		;;
	host)
		for mf in */Makefile; do
			[ -e "$mf" ] || continue
			d=$(dirname "$mf")
			grep -q '^test:' "$mf" || continue
			note "host: make -C $d test"
			make -C "$d" test
			result "host:$d" $?
		done
		;;
	unit)
		for mf in */mkfile test/*/mkfile; do
			[ -e "$mf" ] || continue
			d=$(dirname "$mf")
			case $d in test/harness) continue ;; esac
			grep -q '^test\|mkone\|mkmany' "$mf" || continue
			note "unit: mk test in $d"
			limit "$top/scripts/h9.sh" --rc "cd $d && mk test"
			st=$?
			[ $st -eq 124 ] && echo "test.sh: unit:$d still running after ${unit_timeout}s; killed" >&2
			result "unit:$d" $st
		done
		;;
	system)
		for t in */test/*_test.sh test/*/*_test.sh; do
			[ -x "$t" ] || continue
			note "system: $t"
			"$t"
			result "system:$t" $?
		done
		;;
	gui)
		note "gui: zig build test (test/harness)"
		(cd test/harness && "$top/scripts/zig.sh" build test --summary failures "$@")
		result gui $?
		;;
	esac
done

note "summary"
for p in $passed; do echo "ok    $p"; done
for f in $failed; do echo "FAIL  $f"; done
[ -z "$failed" ]
