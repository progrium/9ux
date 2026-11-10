#!/bin/sh
# A remote volume (acceptance criterion 7, without the UI): a second h9
# serves a directory over 9P with --serve; Finder's core attaches it as
# a volume (addr=), works on it, then the server stops and the volume
# must report it's gone. Run on the host: finder/test/remote.sh
# Needs finder built (scripts/h9.sh --rc 'cd finder && mk').
set -u
top=$(cd "$(dirname "$0")/../.." && pwd)
h9="$top/scripts/h9.sh"
work="$top/.local/finder-remote.$$"
rm -rf "$work"
mkdir -p "$work/share/docs" "$work/sync"
echo remote > "$work/share/docs/a.txt"
fail=0
cleanup() {
	[ -n "${spid:-}" ] && kill "$spid" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT

# the server: a machine serving its cwd (the share), writes reaching the host
(cd "$work/share" && exec "$h9" --serve 'tcp!127.0.0.1!0' --serve-root /mnt/term/cwd) >"$work/serve.log" 2>&1 &
spid=$!
addr=
for i in $(seq 1 100); do
	addr=$(sed -n 's/^serve: //p' "$work/serve.log" | head -1)
	[ -n "$addr" ] && break
	sleep 0.1
done
if [ -z "$addr" ]; then
	echo "remote.sh: the server didn't start" >&2
	cat "$work/serve.log" >&2
	exit 1
fi

# the client: Finder's core in another machine, allowed to dial only the server
rel=${work#$top/}
(cd "$top" && "$h9" --net-allow "$addr" --rc "rc finder/test/remote.rc '$addr' /mnt/term/cwd/$rel/sync" ) >"$work/client.out" 2>&1 &
cpid=$!
for i in $(seq 1 300); do
	[ -e "$work/sync/ready" ] && break
	sleep 0.1
done
kill "$spid" 2>/dev/null
wait "$spid" 2>/dev/null
spid=
touch "$work/sync/stopped"
wait "$cpid"
out="$work/client.out"

has() {
	if grep -Eq -- "$1" "$out"; then :; else
		echo "FAIL: client output lacks: $1" >&2
		fail=1
	fi
}
lacks() {
	if grep -Eq -- "$1" "$out"; then
		echo "FAIL: client output has: $1" >&2
		fail=1
	fi
}
has '^remote /tmp/rvol online desktop=no addr='
has '^a\.txt - 7 '
has '^kind document$'
has '^rename yes$'
has "^'/tmp/rvol/docs/b copy\.txt'$"
has '^/tmp/rvol/b\.txt$'
has "^'/tmp/rvol/untitled folder'$"
has '^error: exists: /tmp/rvol/docs/b\.txt$'
has '^error: not supported: 9P can.t move between folders'
has '^error: not empty: /tmp/rvol/docs'
has "^remote /tmp/rvol offline .*'err=gone: remote: "
has '^error: gone: '
lacks '^error: (failed|permission)'

# what the client did reached the server's files
for f in "docs/b.txt" "docs/b copy.txt" "untitled folder/b.txt"; do
	[ -e "$work/share/$f" ] || { echo "FAIL: $f not on the server" >&2; fail=1; }
done
[ -e "$work/share/docs/a.txt" ] && { echo "FAIL: docs/a.txt still on the server" >&2; fail=1; }
[ -e "$work/share/b.txt" ] && { echo "FAIL: b.txt wasn't moved" >&2; fail=1; }

if [ $fail != 0 ]; then
	echo '--- client output' >&2
	cat "$out" >&2
	echo "remote.sh: FAIL" >&2
	exit 1
fi
echo "remote.sh: ok"
