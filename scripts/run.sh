#!/bin/sh
# Run 9/UX in a host window, as the website's live demo does:
#
#   scripts/run.sh [-d] [-s] [h9 flags...]
#
# Builds the tree with mk (quick when nothing changed), then starts a
# session in an h9 window: the checkout is bound at /9ux and web/boot.rc
# starts the plumber, uxd and uxmeta, then the rio fork in 9/UX mode,
# Desktop with the home and Applications volumes, and the home folder.
# Like the web demo, everything runs on in-memory overlays
# (--write=ephemeral): files made or moved in Desktop are gone when the
# window closes, and the checkout isn't changed.
#
# -d and -s are boot.rc's: -d prints what's running to this terminal,
# -s leaves the home folder closed. Other arguments go to h9 before
# --gfx, such as --gfx-scale 2 on macOS.
#
# Uses scripts/h9.sh, so $H9 and $H9ROOT pick your own h9 and root.
# On Linux, h9 must be built with -Dgfx=x11 (docs/development.md).
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top"

flags=
while [ $# -gt 0 ]; do
	case $1 in
	-d | -s) flags="$flags $1"; shift ;;
	*) break ;;
	esac
done

scripts/h9.sh --rc 'mk' >&2
exec scripts/h9.sh "$@" --write=ephemeral --gfx \
	--rc "mkdir -p /9ux && bind /mnt/term/cwd /9ux && rc /9ux/web/boot.rc$flags"
