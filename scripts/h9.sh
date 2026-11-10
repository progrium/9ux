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
		# Not clone: the root may already be in .local/hyper9/.local
		# (CI restores it from its own cache).
		mkdir -p "$src"
		git -C "$src" init -q
		git -C "$src" remote add origin "$H9_REPO"
	fi
	if [ "$(git -C "$src" rev-parse -q --verify HEAD || true)" != "$H9_REV" ]; then
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

# Directories the ISO's tree lacks: the mount points the kernel supplies
# (h9's tools/mkroot.zig lists them), and the empty ones a 9front install
# has under /mnt and /n (sys/lib/sysconfig/proto/portproto), which the
# ISO leaves out. Cheap, so done on every run: an older tree gets them too.
mountpoints() {
	for d in bin boot dev env fd mnt mnt/temp mnt/term mnt/term/cwd mnt/wsys \
		n net net.alt proc root shr srv tmp \
		mnt/acme mnt/apeselect mnt/apm mnt/cd mnt/cons mnt/consoles mnt/doc \
		mnt/exportfs mnt/factotum mnt/lp mnt/plumb mnt/rdb mnt/vmware \
		n/9 n/9fat n/a n/c n/d n/a: n/c: n/d: n/boot n/dump n/ftp n/kfs \
		n/sid n/tapefs; do
		[ -d "$1/$d" ] || mkdir -p "$1/$d"
	done
}

# The release ISO's tree.
iso_root() {
	nine="$src/.local/9front"
	dest="$nine/root-$NINEFRONT_RELEASE-iso"
	if [ -e "$dest/.9ux-root" ]; then
		mountpoints "$dest"
		return 0
	fi
	iso="$nine/9front-$NINEFRONT_RELEASE.amd64.iso"
	mkdir -p "$nine"
	if [ ! -f "$iso" ]; then
		echo "h9.sh: fetching the 9front $NINEFRONT_RELEASE amd64 ISO" >&2
		# 9front.org is one small server: give up on a stalled
		# transfer rather than hang, and resume on retry.
		url="$NINEFRONT_ISO_URL/9front-$NINEFRONT_RELEASE.amd64.iso.gz"
		i=0
		until curl -fL -sS -C - --connect-timeout 30 --speed-limit 20000 --speed-time 60 -o "$iso.gz" "$url"; do
			i=$((i + 1))
			[ $i -lt 5 ] || { echo "h9.sh: can't fetch $url" >&2; exit 1; }
			echo "h9.sh: retrying $url" >&2
			sleep $((i * 10))
		done
		gunzip -c "$iso.gz" >"$iso.tmp"
		echo "$NINEFRONT_AMD64_ISO_SHA256  $iso.tmp" | sha256sum -c --quiet >&2
		mv "$iso.tmp" "$iso"
		rm -f "$iso.gz"
	fi
	zig=$("$top/scripts/zig.sh" --path)
	rm -rf "$dest"
	(cd "$src" && . scripts/env.sh && "$zig" build isoroot -- "$iso" "$dest") >&2
	mountpoints "$dest"
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
