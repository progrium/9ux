#!/bin/sh
# A real 9front 11952 VM for the checks h9 can't do (docs/vm.md):
#
#   scripts/vm.sh fetch     download the release's qcow2 image for this host
#   scripts/vm.sh boot      boot it on a copy-on-write overlay, console on
#                           this terminal (serial), the 9/UX checkout served
#                           to it over 9P (mount it with `9ux` in the VM)
#   scripts/vm.sh reset     throw the overlay away (back to the release)
#
# Needs QEMU (qemu-system-x86_64 or -aarch64, and qemu-img). Uses KVM or
# HVF when there is one, else plain emulation (slow, but it works).
# Forwards host ports 17019 (rcpu/drawterm), 567 (auth) and 564 (9fs) to
# the VM, offset by VM_PORT_BASE (default 20000: 37019, 20567, 20564;
# VM_PORT_BASE=0 forwards the real ports, which drawterm and factotum
# assume). VM_PLAN9INI adds plan9.ini settings on amd64, such as
# VM_PLAN9INI=service=cpu. The arm64 path hasn't been run yet.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
. "$top/scripts/pins.sh"

dir="$top/.local/vm"
base=${VM_PORT_BASE:-20000}
case "$(uname -m)" in
x86_64|amd64) arch=amd64 ;;
arm64|aarch64) arch=arm64 ;;
*) echo "vm.sh: no 9front image for $(uname -m)" >&2; exit 1 ;;
esac
img="$dir/9front-$NINEFRONT_RELEASE.$arch.qcow2"
overlay="$dir/overlay.$arch.qcow2"

fetch() {
	mkdir -p "$dir"
	[ -f "$img" ] && return 0
	curl -fL --retry 3 -o "$img.gz" "$NINEFRONT_ISO_URL/9front-$NINEFRONT_RELEASE.$arch.qcow2.gz"
	gunzip "$img.gz"
}

accel() {
	if [ -w /dev/kvm ]; then echo kvm
	elif [ "$(uname -s)" = Darwin ]; then echo hvf
	else echo tcg
	fi
}

boot() {
	fetch
	[ -f "$overlay" ] || qemu-img create -q -f qcow2 -F qcow2 -b "$img" "$overlay"

	# The checkout, served read-write over 9P on the host's loopback;
	# the VM reaches the host as 10.0.2.2.
	port=$((base + 5640))
	(cd "$top" && exec "$top/scripts/h9.sh" --serve "tcp!127.0.0.1!$port" --serve-root /mnt/term/cwd) \
		>"$dir/serve.log" 2>&1 &
	spid=$!
	trap 'kill $spid 2>/dev/null' EXIT INT TERM
	cat <<EOF >&2
vm.sh: the checkout is served at tcp!10.0.2.2!$port; in the VM:
	srv tcp!10.0.2.2!$port 9ux && mount -c /srv/9ux /n/9ux
vm.sh: host ports: rcpu $((base + 17019)), auth $((base + 567)), 9fs $((base + 564))
vm.sh: quit QEMU with ctl-a x
EOF
	fwd="hostfwd=tcp:127.0.0.1:$((base + 17019))-:17019,hostfwd=tcp:127.0.0.1:$((base + 567))-:567,hostfwd=tcp:127.0.0.1:$((base + 564))-:564"
	a=$(accel)
	case $arch in
	amd64)
		# The kernel is loaded directly (it's multiboot), so plan9.ini
		# can come from here: the console on the serial line.
		kernel="$("$top/scripts/h9.sh" --root-dir)/amd64/9pc64"
		qemu-system-x86_64 -accel "$a" -m 2G -smp 2 -nographic \
			-kernel "$kernel" \
			-append "console=0 nobootprompt=local!/dev/sdC0/fs user=glenda ${VM_PLAN9INI:-}" \
			-drive file="$overlay",if=ide,index=0 \
			-nic user,model=e1000,"$fwd"
		;;
	arm64)
		# Untested: see docs/vm.md.
		cpu=cortex-a72
		[ "$a" = tcg ] || cpu=host
		echo "vm.sh: arm64 boots through the image's U-Boot; set console=0 in plan9.ini for a serial console" >&2
		qemu-system-aarch64 -accel "$a" -M virt,highmem=off,gic-version=3 -cpu "$cpu" -m 2G -smp 2 -nographic \
			-bios "${VM_BIOS:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}" \
			-drive file="$overlay",if=none,id=disk -device virtio-blk-pci,drive=disk \
			-nic user,model=virtio-net-pci,"$fwd"
		;;
	esac
}

case "${1:-}" in
fetch) fetch ;;
boot) boot ;;
reset) rm -f "$overlay" ;;
*) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
