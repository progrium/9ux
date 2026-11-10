#!/bin/sh
# Run h9 at the pinned revision: scripts/h9.sh [h9 args...]
#
# Uses $H9 if set. Otherwise clones h9 at H9_REV into .local/hyper9,
# builds it, and builds its 9front root (with /sys/src) on first use.
# Needs what h9's DEVELOPING.md lists for this host (Zig, and QEMU for
# the root).
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
. "$top/scripts/pins.sh"

if [ -z "${H9:-}" ]; then
	src="$top/.local/hyper9"
	if [ ! -d "$src/.git" ]; then
		mkdir -p "$top/.local"
		git clone -q "$H9_REPO" "$src"
	fi
	if [ "$(git -C "$src" rev-parse HEAD)" != "$H9_REV" ]; then
		git -C "$src" fetch -q origin "$H9_REV"
		git -C "$src" checkout -q "$H9_REV"
		rm -f "$src/.9ux-built"
	fi
	if [ ! -e "$src/.9ux-built" ]; then
		(
			cd "$src"
			. scripts/env.sh
			./scripts/fetch.sh
			zig build ${H9_BUILD_FLAGS:-}
			[ -d .local/9front/root ] || zig build mkroot -- --src
		) >&2
		touch "$src/.9ux-built"
	fi
	H9="$src/zig-out/bin/h9"
fi
exec "$H9" "$@"
