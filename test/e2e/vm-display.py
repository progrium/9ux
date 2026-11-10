#!/usr/bin/env python3
"""Acceptance criterion 6 on a real 9front display: boots the 9front
11952 VM (scripts/vm.sh's image and kernel) with a VESA framebuffer,
runs the rio fork with the libux sample (test/e2e/vm-sample.rc), and
takes a QEMU screendump. The sample's window
draws ICN#/icl8 icons from its .rsrc and the system's through resfs.

    test/e2e/vm-display.py

Writes .local/test/artifacts/vm-sample-32.png, and compares the window's region with test/libux/golden/sample.png (what
the headless screen draws) and says how many pixels differ.
Needs qemu-system-x86_64, and scripts/vm.sh fetch run once. Slow
without KVM: a few minutes per depth.
"""
import os, subprocess, sys, time, socket, select

top = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
art = os.path.join(top, ".local/test/artifacts")
vmdir = os.path.join(top, ".local/vm")
os.makedirs(art, exist_ok=True)

def sh(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout.strip()

def run(depth):
    img = os.path.join(vmdir, "9front-11952.amd64.qcow2")
    if not os.path.exists(img):
        sys.exit("vm-display: no VM image; run scripts/vm.sh fetch")
    overlay = os.path.join(vmdir, "display.qcow2")
    if os.path.exists(overlay):
        os.remove(overlay)
    sh("qemu-img", "create", "-q", "-f", "qcow2", "-F", "qcow2", "-b", img, overlay)
    kernel = os.path.join(sh(os.path.join(top, "scripts/h9.sh"), "--root-dir"), "amd64/9pc64")
    port = 25641
    serve = subprocess.Popen([os.path.join(top, "scripts/h9.sh"), "--serve", "tcp!127.0.0.1!%d" % port,
                              "--serve-root", "/mnt/term/cwd"], cwd=top,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = os.path.join(vmdir, "monitor.sock")
    ovmf = os.environ.get("VM_OVMF", "/usr/share/ovmf/OVMF.fd")
    if not os.path.exists(ovmf):
        sys.exit("vm-display: no UEFI firmware at %s (install ovmf, or set VM_OVMF)" % ovmf)
    accel = "kvm" if os.access("/dev/kvm", os.W_OK) else "tcg"

    def qemu(direct):
        if os.path.exists(mon):
            os.remove(mon)
        args = ["qemu-system-x86_64", "-accel", accel, "-m", "2G", "-smp", "2",
                "-display", "none", "-vga", "std", "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % mon,
                "-drive", "file=%s,if=ide,index=0" % overlay,
                "-nic", "user,model=e1000"]
        if direct:
            args += ["-kernel", kernel, "-append", "console=0 nobootprompt=local!/dev/sdC0/fs user=glenda"]
        else:
            args += ["-bios", ovmf]
        return subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # First boot, kernel loaded directly: put 9front's EFI loader on the
    # image's 9fat and set plan9.ini so it boots from its own disk with
    # the console on serial, and keep glenda's
    # profile from starting stock rio on the screen. The second boot is
    # through UEFI firmware, because the amd64 kernel has no realmode, so
    # neither 9boot on BIOS nor aux/vga can set a VESA mode: the EFI
    # loader hands the kernel the firmware's framebuffer (*bootscreen),
    # 32 bits deep at the firmware's resolution.
    q = qemu(True)
    out = b""
    log = open(os.path.join(art, "vm-serial-%s.log" % depth), "wb")

    def wait(pats, secs):
        """Waits for one of pats (a string or a tuple); returns the
        one seen (truthy), or None (falsy) at the deadline."""
        nonlocal out
        if isinstance(pats, str):
            pats = (pats,)
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([q.stdout], [], [], 1)
            if r:
                b = os.read(q.stdout.fileno(), 4096)
                if not b:
                    break
                out += b
                log.write(b)
                log.flush()
            for pat in pats:
                if pat.encode() in out:
                    out = out[out.index(pat.encode()) + len(pat):]
                    return pat
        return None

    def send(line):
        q.stdin.write(line.encode() + b"\r")
        q.stdin.flush()

    try:
        if not wait("term% ", 600):
            sys.exit("vm-display: no prompt from the VM (see %s)" % log.name)
        send("9fs 9fat && grep -v '^(console|nobootprompt|user|monitor|vgasize|mouseport|bootargs)=' /n/9fat/plan9.ini >/tmp/ini && cat /tmp/ini")
        wait("term% ", 60)
        for line in ["console=0", "nobootprompt=local!/dev/sdC0/fs", "user=glenda", "mouseport=ps2"]:
            send("echo '%s' >>/tmp/ini" % line)
            wait("term% ", 30)
        send("mkdir -p /n/9fat/efi/boot && cp /386/bootx64.efi /n/9fat/efi/boot/bootx64.efi && cp /tmp/ini /n/9fat/plan9.ini && sed 's/^	rio -i riostart$/	status=()/' /usr/glenda/lib/profile >/tmp/prof && cp /tmp/prof /usr/glenda/lib/profile && echo EDI^TED")
        if not wait("EDITED", 60):
            sys.exit("vm-display: couldn't edit plan9.ini")
        wait("term% ", 10)
        send("fshalt && echo HAL^TED")
        wait("HALTED", 120)
        q.kill()
        q.wait()
        # Second boot, from the disk through 9boot.
        q = qemu(False)
        # The kernel may still ask for the root and the user: take the defaults
        out = b""
        for _ in range(3):
            got = wait(("bootargs is", "user[", "term% "), 900)
            if got is None:
                sys.exit("vm-display: no prompt after booting from disk (see %s)" % log.name)
            if got == "term% ":
                break
            time.sleep(1)
            send("")
        send("ip/ipconfig; srv tcp!10.0.2.2!%d 9ux && mount -c /srv/9ux /n/9ux && echo MOU^NTED" % port)
        if not wait("MOUNTED", 120):
            sys.exit("vm-display: couldn't mount the checkout")
        wait("term% ", 10)
        send("cat /dev/vgactl && echo VGA^OK")
        if not wait("VGAOK", 120):
            sys.exit("vm-display: no screen (see %s)" % log.name)
        wait("term% ", 10)
        send("rm -f /env/font; /n/9ux/rio/6.out -i 'rc /n/9ux/test/e2e/vm-sample.rc'")
        time.sleep(180 if accel == "tcg" else 20)
        ppm = os.path.join(art, "vm-sample-%s.ppm" % depth)
        s = socket.socket(socket.AF_UNIX)
        s.connect(mon)
        s.sendall(("screendump %s\n" % ppm).encode())
        time.sleep(3)
        s.close()
        from PIL import Image
        im = Image.open(ppm).convert("RGB")
        png = os.path.join(art, "vm-sample-%s.png" % depth)
        im.save(png)
        os.remove(ppm)
        print("vm-display: depth %s: %s (%dx%d)" % (depth, png, im.width, im.height))
        gold = Image.open(os.path.join(top, "test/libux/golden/sample.png")).convert("RGB")
        reg = im.crop((20, 20, 20 + gold.width, 20 + gold.height))
        a, b = reg.load(), gold.load()
        diff = sum(1 for y in range(gold.height) for x in range(gold.width) if a[x, y] != b[x, y])
        print("vm-display: depth %s: %d of %d pixels differ from the headless golden" % (depth, diff, gold.width * gold.height))
        return diff
    finally:
        q.kill()
        serve.kill()
        log.close()

if __name__ == "__main__":
    run("32")
