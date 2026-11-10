# Checks on a real 9front VM

h9 is one machine with one screen in one host process, so a few things
need real 9front ([development.md](development.md), "What still needs a
real 9front"): CPU-server sessions (`rcpu`, drawterm) running the rio
fork, real display formats, and installing into a real system. This is
the recipe, with `scripts/vm.sh` doing the QEMU part.

## Status

Run once, on Linux x86\_64 with plain emulation (no KVM), on
2026-11-10, with `dev` at 47f70d5:

| Step | Result |
|---|---|
| Boot the 11952 amd64 image with a serial console | works; about two minutes to a prompt emulated |
| Mount the checkout from `h9 --serve` and build the rio fork with the VM's own `mk` | works: `rio/` (with S2's `ux*.c`) builds with 9front's 6c/6l |
| Make the VM a CPU and auth server (below) | works: nvram key, `glenda` in keyfs, listeners on 17019 and 567 |
| `rcpu` from an h9 machine on the host into the VM | **not yet**: the connection opens and closes without a session (the client's factotum never gets the server's protocol list). Not settled whether this is h9's networking, QEMU's user-mode forwarding, or the setup; to be retried with drawterm |
| drawterm into the VM, then run the fork in the session | **not yet**: needs drawterm and a display on the host. Jeff, by hand |
| The fork on the VM's own display | works at 32 bits through UEFI (OVMF): `test/e2e/vm-display.py` runs the libux sample and matches the headless golden pixel for pixel ([release.md](release.md)) |
| `mk install` and booting into the fork | not yet |

## Booting

```sh
scripts/vm.sh fetch      # the release's qcow2 (550 MB on amd64) into .local/vm
scripts/vm.sh boot       # serial console here; quit with ctl-a x
```

`boot` runs the release image on a copy-on-write overlay
(`scripts/vm.sh reset` discards it), loads the kernel directly so the
console is the serial line, uses KVM or HVF if it can, and serves the
checkout over 9P from an h9 on the host. At the `term%` prompt:

```rc
ip/ipconfig
srv tcp!10.0.2.2!25640 9ux && mount -c /srv/9ux /n/9ux
cd /n/9ux/rio && mk
```

(25640 is 5640 plus the default `VM_PORT_BASE` of 20000.) Objects land
in your checkout, as they do from h9; they're gitignored.

On arm64 (Apple silicon) the script's QEMU line is a sketch and hasn't
been run; h9's `tools/mkroot.zig` boots the arm64 image in QEMU with its
kernel taken from the FAT partition, which is the known-good way.

## Making it a CPU and auth server

Boot with `VM_PORT_BASE=0 VM_PLAN9INI=service=cpu scripts/vm.sh boot`
(port 0 offset, so drawterm and factotum find the usual ports). With an
empty nvram the kernel asks for the host owner's key; answer:

```
authid: glenda
authdom: 9ux
secstore key: (empty)
password: ninefront9ux      (twice; any password works)
enable legacy p9sk1[no]: no
```

At the `cirno#` prompt, once:

```rc
ip/ipconfig
echo 'ipnet=qemu ip=10.0.2.0 ipmask=255.255.255.0 auth=10.0.2.15 authdom=9ux cpu=10.0.2.15' >>/lib/ndb/local
echo 'authdom=9ux auth=10.0.2.15' >>/lib/ndb/local
auth/keyfs
auth/changeuser glenda      # the same password; accept the defaults
aux/listen -q -t /rc/bin/service.auth -d /rc/bin/service.none tcp
```

After later boots the key is in nvram and the user in `/adm/keys`, but
the network and auth listener still need starting: `ip/ipconfig`,
`auth/keyfs`, and the `aux/listen` line. (The image's cpurc only starts
them for a system it finds by name in ndb; adding a `sys=cirno` entry
would make that automatic.)

## The checks

From the host, with the VM up as above:

```sh
drawterm -h 127.0.0.1 -a 127.0.0.1 -u glenda
```

In the drawterm window (the `9ux` mount works the same way from there):

1. `rio/6.out` starts and draws, and `window`, button 3 New, and
   resizing behave as in [protocols/rio.md](protocols/rio.md).
2. A second rio nested inside it, acme, and sam start.
3. From another host terminal, `rcpu -h tcp!127.0.0.1!17019 -u glenda`
   (from a 9front or an h9 with a factotum key for `dom=9ux`) gets a
   shell, and `window` there opens in the drawterm's rio when `$wsys`
   is imported.

Record each result in the table above with the date and the commit.
