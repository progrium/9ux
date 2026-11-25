#!/bin/sh
# Run uitree's conformance suite against uxkit, in h9.
#
#	test/conformance/run.sh [run args...]     e.g. -t 120 14-events
#
# UITREE is the uitree directory of a progrium/scratch checkout (default
# ../scratch/uitree beside this repository). The suite is copied to
# .local/conformance so its out/ stays out of that checkout. Build uxkit
# first (scripts/test.sh build, or h9 --rc 'cd uxkit && mk').
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
uitree=${UITREE:-$top/../scratch/uitree}
suite=$uitree/tests/conformance
if [ ! -f "$suite/run" ]; then
	echo "run.sh: no conformance suite at $suite (set UITREE)" >&2
	exit 2
fi
work=$top/.local/conformance
mkdir -p "$work"
cp -R "$suite"/. "$work"/
cp "$top/uxkit/6.out" "$work/uxkit"
cat >"$work/uxkit-headless" <<EOS
#!/bin/sh
# uitree-headless for the runner: uxkit CMD ARGS in h9, here.
exec "$top/scripts/h9.sh" --cwd "$work" --rc "./uxkit \$*"
EOS
chmod +x "$work/uxkit-headless"
cd "$work"
UITREE_HEADLESS=$work/uxkit-headless exec ./run "$@"
