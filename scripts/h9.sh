#!/bin/sh
# Run h9 at the pinned revision: scripts/h9.sh [h9 args...]
#
# Uses $H9 if set. Otherwise clones h9 at H9_REV into .local/hyper9 and
# builds it, then provides a 9front root tree (with /sys/src) on first use:
#
#   Linux x86_64 (no KVM; CI): the release's amd64 ISO, unpacked with h9's
#     isoroot tool. h9 runs it on the native backend (--backend native is
#     added for you unless you pass --backend).
#   macOS arm64 and Linux with KVM: `zig build mkroot -- --src`, which
#     needs QEMU (see h9's DEVELOPING.md).
#
# Needs Zig (ZIG=/path/to/zig, or zig on PATH; scripts/zig.sh fetches the
# pinned one into .local/zig if neither is there).
#
#   scripts/h9.sh --setup     build h9 and the root only, run nothing
#   scripts/h9.sh --root-dir  print the root tree's path
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
. "$top/scripts/pins.sh"

src="$top/.local/hyper9"
os=$(uname -s)
arch=$(uname -m)

# isoroot: the native backend runs amd64 binaries with no hypervisor, which
# is what CI has; everything else builds the root the way h9 documents.
want_iso=0
if [ "$os" = Linux ] && [ "$arch" = x86_64 ] && [ ! -w /dev/kvm ]; then
	want_iso=1
fi
[ "${H9_ROOT_FROM:-}" = iso ] && want_iso=1
[ "${H9_ROOT_FROM:-}" = mkroot ] && want_iso=0

build_h9() {
	if [ ! -d "$src/.git" ]; then
		mkdir -p "$top/.local"
		git clone -q "$H9_REPO" "$src"
	fi
	if [ "$(git -C "$src" rev-parse HEAD)" != "$H9_REV" ]; then
		git -C "$src" fetch -q origin "$H9_REV"
		git -C "$src" checkout -q "$H9_REV"
		rm -f "$src/.9ux-built"
	fi
	[ -e "$src/.9ux-built" ] && return 0
	zig=$("$top/scripts/zig.sh" --path)
	(
		cd "$src"
		export PATH="$(dirname "$zig"):$PATH"
		. scripts/env.sh
		if [ $want_iso = 0 ]; then
			./scripts/fetch.sh src image
		fi
		zig build ${H9_BUILD_FLAGS:-}
	) >&2
	touch "$src/.9ux-built"
}

# The release ISO's tree, plus the mount points the kernel supplies
# (h9's tools/mkroot.zig lists them).
iso_root() {
	nine="$src/.local/9front"
	dest="$nine/root-$NINEFRONT_RELEASE-iso"
	[ -e "$dest/.9ux-root" ] && return 0
	iso="$nine/9front-$NINEFRONT_RELEASE.amd64.iso"
	mkdir -p "$nine"
	if [ ! -f "$iso" ]; then
		echo "h9.sh: fetching the 9front $NINEFRONT_RELEASE amd64 ISO" >&2
		curl -fL --retry 3 -s -S -o "$iso.gz" "$NINEFRONT_ISO_URL/9front-$NINEFRONT_RELEASE.amd64.iso.gz"
		gunzip -c "$iso.gz" >"$iso.tmp"
		echo "$NINEFRONT_AMD64_ISO_SHA256  $iso.tmp" | sha256sum -c --quiet >&2
		mv "$iso.tmp" "$iso"
		rm -f "$iso.gz"
	fi
	zig=$("$top/scripts/zig.sh" --path)
	rm -rf "$dest"
	(cd "$src" && . scripts/env.sh && "$zig" build isoroot -- "$iso" "$dest") >&2
	for d in bin boot dev env fd mnt mnt/temp mnt/term mnt/term/cwd mnt/wsys \
		n net net.alt proc root shr srv tmp; do
		mkdir -p "$dest/$d"
	done
	# Installer payloads, not part of a running system.
	rm -rf "$dest/dist"
	touch "$dest/.9ux-root"
	ln -sfn "root-$NINEFRONT_RELEASE-iso" "$nine/root"
	# The ISO is only needed again to rebuild the tree.
	[ -n "${H9_KEEP_ISO:-}" ] || rm -f "$iso"
}

mkroot() {
	[ -d "$src/.local/9front/root/sys/src" ] && return 0
	zig=$("$top/scripts/zig.sh" --path)
	(cd "$src" && . scripts/env.sh && "$zig" build mkroot -- --src) >&2
}

if [ -z "${H9:-}" ]; then
	build_h9
	H9="$src/zig-out/bin/h9"
	if [ -z "${H9ROOT:-}" ]; then
		if [ $want_iso = 1 ]; then iso_root; else mkroot; fi
	fi
fi
root=${H9ROOT:-$src/.local/9front/root}

case "${1:-}" in
--setup) exit 0 ;;
--root-dir) echo "$root"; exit 0 ;;
esac

if [ $want_iso = 1 ]; then
	case " $* " in
	*" --backend"*) ;;
	*) set -- --backend native "$@" ;;
	esac
fi
exec "$H9" "$@"
