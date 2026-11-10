#!/bin/sh
# rescomp tests on the host: test/comp.sh path/to/rescat path/to/rescomp
#  1. every fixture that parses decompiles and recompiles to the same
#     resources (index and every raw resource identical)
#  2. src/sample.res compiles to the expected index and decoded forms
#  3. every src/bad-*.res is refused
set -u
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
abs() { case "$1" in /*) echo "$1" ;; *) echo "$(pwd)/$1" ;; esac; }
rescat=$(abs "${1:?usage: comp.sh rescat rescomp}")
rescomp=$(abs "${2:?usage: comp.sh rescat rescomp}")
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
pass=0 failn=0
ok() { pass=$((pass+1)); }
bad() { echo "FAIL: $*"; failn=$((failn+1)); }

# same resources in two files: index, then raw bytes of each
same() {
	"$rescat" "$1" >"$tmp/i1" && "$rescat" "$2" >"$tmp/i2" || return 1
	cmp -s "$tmp/i1" "$tmp/i2" || return 1
	while read -r type id rest; do
		"$rescat" "$1" "$type" "$id" >"$tmp/r1" && "$rescat" "$2" "$type" "$id" >"$tmp/r2" || return 1
		cmp -s "$tmp/r1" "$tmp/r2" || return 1
	done <"$tmp/i1"
}

for f in fixtures/*.rsrc; do
	"$rescat" "$f" >/dev/null 2>&1 || continue
	n=$(basename "$f" .rsrc)
	if ! "$rescomp" -d "$f" >"$tmp/$n.res"; then
		bad "rescomp -d $f"
	elif ! "$rescomp" -o "$tmp/$n.rsrc" "$tmp/$n.res"; then
		bad "rescomp of decompiled $f"
	elif ! same "$f" "$tmp/$n.rsrc"; then
		bad "round trip of $f changed its resources"
	else
		ok
	fi
done

# the decompiled sample keeps every text form as text
if [ "$(grep -c '^end$' "$tmp/sample.res")" -ne 9 ]; then
	bad "rescomp -d sample.rsrc: expected 9 text forms"
else
	ok
fi

if ! "$rescomp" -o "$tmp/src.rsrc" src/sample.res; then
	bad "rescomp src/sample.res"
else
	"$rescat" "$tmp/src.rsrc" >"$tmp/src.index"
	if cmp -s "$tmp/src.index" src/sample.expect; then ok; else bad "src/sample.res index differs from src/sample.expect"; fi
	check() {
		if "$rescat" "$tmp/src.rsrc" $1 >"$tmp/out" && cmp -s "$tmp/out" "golden/$2"; then ok
		else bad "src/sample.res: $1 differs from golden/$2"; fi
	}
	check "icon 128 image" icon128.image
	check "icon 128 small" icon128.small
	check "ICN# 128 image" ICN-128.image
	check "CURS 128 image" CURS128.image
	check "CURS 128 cursor" CURS128.cursor
	check "STR%20 128 text" STR128.text
	check "MENU 128 text" MENU128.text
	check "DLOG 128 text" DLOG128.text
	check "ALRT 129 text" ALRT130.text
	check "icl8 130 image" icl8-130.image
	check "zzzz -16455" zzzz.raw
	# lossless sources give sample.rsrc's bytes exactly (icons lose
	# what lies outside their masks, so only the opaque one is here)
	for r in "MENU 128" "DLOG 128" "STR%20 128" "icl8 130"; do
		"$rescat" "$tmp/src.rsrc" $r >"$tmp/a" && "$rescat" fixtures/sample.rsrc $r >"$tmp/b"
		if cmp -s "$tmp/a" "$tmp/b"; then ok; else bad "src/sample.res: $r bytes differ from fixtures/sample.rsrc"; fi
	done
fi

for f in src/bad-*.res; do
	if "$rescomp" -o "$tmp/bad.rsrc" "$f" 2>"$tmp/err"; then
		bad "rescomp $f succeeded, expected an error"
	else
		ok
	fi
done

echo "rescomp: $pass passed, $failn failed"
[ "$failn" -eq 0 ]
