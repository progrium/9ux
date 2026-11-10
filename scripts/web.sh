#!/bin/sh
# The website's live demo (site/demo/): 9/UX in a browser tab, on h9's
# browser host (hyper9's design/0012-browser-host.md and 0120-browser-gfx.md).
#
#   scripts/web.sh [OUT]    build it into OUT (default .local/web/demo)
#
# The browser host runs 9front's 386 programs on v86, so this:
#
#  1. builds h9's browser host at H9_WEB_REV (scripts/pins.sh) into
#     .local/web/hyper9, with the release's 386 ISO unpacked into a root
#     tree there (zig build isoroot);
#  2. builds 9/UX with mk on h9 (scripts/h9.sh), natively first, for the
#     resources the build makes by running its own tools, then for 386
#     with the 386 root's /386 bound in (the amd64 root's compilers
#     cross-compile);
#  3. packs the demo's root archive, root.h9a: what h9's own demo archive
#     holds (its src/web/root.list), what web/root.list adds, and the
#     9/UX tree at /9ux (the binaries, resources and web/*.rc);
#  4. copies h9's page runtime, the archive, and site/demo/ into OUT.
#
# H9_WEB_SRC=/path/to/hyper9 uses that checkout as it is instead of
# cloning one. Needs Zig (scripts/zig.sh) and, on Linux x86_64 without
# KVM, everything scripts/h9.sh needs.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
. "$top/scripts/pins.sh"

out=${1:-$top/.local/web/demo}
case $out in /*) ;; *) out="$PWD/$out" ;; esac
local="$top/.local/web"
mkdir -p "$local"
zig=$("$top/scripts/zig.sh" --path)
export PATH="$(dirname "$zig"):$PATH"

# 1. h9's browser host, and the 386 root.
if [ -n "${H9_WEB_SRC:-}" ]; then
	src=$H9_WEB_SRC
else
	src="$local/hyper9"
	if [ ! -d "$src/.git" ]; then
		mkdir -p "$src"
		git -C "$src" init -q
		git -C "$src" remote add origin "$H9_REPO"
	fi
	if [ "$(git -C "$src" rev-parse -q --verify HEAD || true)" != "$H9_WEB_REV" ]; then
		git -C "$src" fetch -q --depth 1 origin "$H9_WEB_REV"
		git -C "$src" checkout -q "$H9_WEB_REV"
	fi
fi
root386="$src/.local/9front/root386"
(
	cd "$src"
	. scripts/env.sh
	scripts/fetch.sh v86 >/dev/null
	if [ ! -x "$root386/386/bin/rc" ]; then
		scripts/fetch.sh iso386 >/dev/null
		. scripts/pins.sh
		zig build isoroot -- ".local/9front/9front-$NINEFRONT_RELEASE.386.iso" "$root386"
	fi
	# No root archive from h9 (no such -Dweb-root): step 3 packs its own.
	zig build web -Doptimize=ReleaseSafe -Dweb-root=/nonexistent
) >&2

# 2. 9/UX, natively and then for 386. The 386 pass makes only each
# component's programs: mk's default targets would also run the 386
# rescomp to remake sample.rsrc, which the native pass already made.
"$top/scripts/h9.sh" --rc 'mk' >&2
# h9 shows a shared host path at /mnt/term/PATH, unless it's in the cwd.
case $root386 in
"$top"/*) g386="/mnt/term/cwd/${root386#"$top"/}/386" ;;
*) g386="/mnt/term$root386/386" ;;
esac
"$top/scripts/h9.sh" --readable "$root386/386" --rc "
	bind $g386 /386
	objtype=386
	@{ cd libux && mk } || exit 1
	for(d in rio uxd uxmeta clipd apps/sample) @{ cd \$d && mk 8.out } || exit 1
	@{ cd resfs && mk 8.resfs } || exit 1
	@{ cd finder && mk 8.finder } || exit 1
" >&2

# 3. The archive: a tree of hard links to the 386 root, with /9ux added.
stage="$local/stage"
rm -rf "$stage"
cp -al "$root386" "$stage" 2>/dev/null || cp -a "$root386" "$stage"
ux="$stage/9ux"
mkdir -p "$ux"
cp_tree() { # cp_tree PATH...: from the checkout into /9ux, keeping paths
	for p in "$@"; do
		mkdir -p "$ux/$(dirname "$p")"
		cp -R "$top/$p" "$ux/$p"
	done
}
cp_tree rio/8.out uxd/8.out uxmeta/8.out clipd/8.out resfs/8.resfs \
	finder/8.finder apps/sample/8.out apps/sample/sample.rsrc \
	assets/gen/system.rsrc assets/gen/patterns web/boot.rc web/session.rc \
	web/finder.rc web/sample.rc web/home web/apps
list="$local/root.list"
{
	cat "$src/src/web/root.list"
	cat "$top/web/root.list"
	echo 9ux
} >"$list"
"$zig" run -OReleaseSafe "$src/tools/webroot.zig" -- "$stage" "$list" "$local/root.h9a" >&2

# 4. The site's demo directory.
rm -rf "$out"
mkdir -p "$out"
w="$src/zig-out/web"
cp "$w/h9-web.js" "$w/worker.js" "$w/wasi-mem.js" "$w/h9.wasm" "$w/v86.wasm" "$w/LICENSE.v86" "$out/"
cp "$src/LICENSE" "$out/LICENSE.h9"
cp "$src/third_party/drawterm/LICENSE" "$out/LICENSE.drawterm"
cp "$local/root.h9a" "$out/"
cp -R "$top/site/demo/." "$out/"
ls -l "$out" >&2
