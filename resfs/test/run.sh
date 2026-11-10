#!/bin/sh
# Run the rescat golden tests on the host: test/run.sh path/to/rescat
# (In 9front or h9, mk test runs test/run.rc instead.)
set -u
rescat=${1:?usage: run.sh rescat}
case "$rescat" in /*) ;; *) rescat=$(pwd)/$rescat ;; esac
[ -x "$rescat" ] || { echo "run.sh: $rescat: not executable" >&2; exit 1; }
cd "$(dirname "$0")"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
pass=0 failn=0
while read -r name fixture args; do
	case "$name" in ''|'#'*) continue ;; esac
	# shellcheck disable=SC2086
	if [ "$name" = fail ]; then
		if "$rescat" "fixtures/$fixture" $args >"$tmp" 2>/dev/null; then
			echo "FAIL: rescat $fixture $args: succeeded, expected an error"
			failn=$((failn+1))
		else
			pass=$((pass+1))
		fi
		continue
	fi
	if ! "$rescat" "fixtures/$fixture" $args >"$tmp"; then
		echo "FAIL: rescat $fixture $args: exited with an error"
		failn=$((failn+1))
	elif ! cmp -s "$tmp" "golden/$name"; then
		echo "FAIL: rescat $fixture $args: differs from golden/$name"
		failn=$((failn+1))
	else
		pass=$((pass+1))
	fi
done <cases
echo "$pass passed, $failn failed"
[ "$failn" -eq 0 ]
