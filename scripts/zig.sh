#!/bin/sh
# Run the pinned Zig: scripts/zig.sh [zig args...]
#
# Uses $ZIG, else zig on PATH if it's ZIG_VERSION, else fetches that
# release from ziglang.org into .local/zig (checked against the index's
# hash). `scripts/zig.sh --path` prints the binary's path.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
. "$top/scripts/pins.sh"

find_zig() {
	if [ -n "${ZIG:-}" ]; then echo "$ZIG"; return; fi
	if command -v zig >/dev/null 2>&1 && [ "$(zig version)" = "$ZIG_VERSION" ]; then
		command -v zig; return
	fi
	case "$(uname -s)-$(uname -m)" in
	Linux-x86_64) plat=x86_64-linux ;;
	Linux-aarch64) plat=aarch64-linux ;;
	Darwin-arm64) plat=aarch64-macos ;;
	Darwin-x86_64) plat=x86_64-macos ;;
	*) echo "zig.sh: no Zig release for $(uname -s) $(uname -m); set ZIG" >&2; exit 1 ;;
	esac
	dir="$top/.local/zig/zig-$plat-$ZIG_VERSION"
	if [ ! -x "$dir/zig" ]; then
		echo "zig.sh: fetching Zig $ZIG_VERSION for $plat" >&2
		mkdir -p "$top/.local/zig"
		info=$(curl -fsSL https://ziglang.org/download/index.json |
			python3 -c 'import json,sys; d=json.load(sys.stdin)[sys.argv[1]][sys.argv[2]]; print(d["tarball"], d["shasum"])' "$ZIG_VERSION" "$plat")
		set -- $info
		tmp="$top/.local/zig/zig.tar.xz"
		curl -fsSL --retry 3 -o "$tmp" "$1"
		if command -v sha256sum >/dev/null 2>&1; then
			echo "$2  $tmp" | sha256sum -c --quiet >&2
		else
			echo "$2  $tmp" | shasum -a 256 -c --quiet >&2
		fi
		tar -xJf "$tmp" -C "$top/.local/zig"
		rm -f "$tmp"
	fi
	echo "$dir/zig"
}

z=$(find_zig)
if [ "${1:-}" = --path ]; then echo "$z"; exit 0; fi
exec "$z" "$@"
