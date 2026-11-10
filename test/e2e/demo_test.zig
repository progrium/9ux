//! The v0.1 end-to-end demo (design §10, "First end-to-end demo") as one
//! headless run:
//!
//!  1. rio in 9/UX mode, with the session's services: the plumber, uxd
//!     and uxmeta (services.rc).
//!  2. Finder's desktop shows a local volume and a remote one, served
//!     over 9P by a second h9 on the host (`h9 --serve`).
//!  3. Opening the remote volume lists the server's files.
//!  4. Opening a folder, then double-clicking a text file in it, plumbs
//!     it to acme.
//!  5. The libux sample app launches, and its File menu, clicked in
//!     rio's menu bar, reaches it.
//!  6. From rc, /mnt/ux/apps lists the apps, a text field's value reads
//!     back, and writing `activate` to a button's ctl presses it.
//!  7. acme and an ordinary rc window still behave as they do in stock rio.
//!
//! Build everything first (scripts/test.sh does).
const std = @import("std");
const builtin = @import("builtin");
const harness = @import("harness");
const rect = harness.rect;
const pt = harness.pt;
const Session = harness.Session;
const gpa = std.testing.allocator;

/// The menu bar's height (assets/README.md).
const bar_h = 20;

// ---- The remote volume: a second h9 serving a directory over 9P ----

const Remote = struct {
    child: std.process.Child,
    addr_buf: [64]u8 = undefined,
    addr: []const u8 = "",
    dir: [:0]u8,

    /// Serves `.local/test/e2e-remote/share` (made fresh, with a few
    /// files) from a second h9 on a loopback port.
    fn start(io: std.Io) !*Remote {
        const cwd = std.Io.Dir.cwd();
        const base = ".local/test/e2e-remote";
        cwd.deleteTree(io, base) catch {};
        try cwd.createDirPath(io, base ++ "/share/papers");
        try cwd.writeFile(io, .{ .sub_path = base ++ "/share/remote.txt", .data = "served from another machine\n" });
        try cwd.writeFile(io, .{ .sub_path = base ++ "/share/papers/plan9.txt", .data = "Plan 9 from Bell Labs\n" });
        const r = try gpa.create(Remote);
        errdefer gpa.destroy(r);
        r.* = .{ .child = undefined, .dir = try cwd.realPathFileAlloc(io, base, gpa) };
        errdefer gpa.free(r.dir);
        // The served tree is the server's cwd, as in finder/test/remote_test.sh.
        const sh = try std.fmt.allocPrint(gpa, "cd {s}/share && exec ../../../../scripts/h9.sh --serve 'tcp!127.0.0.1!0' --serve-root /mnt/term/cwd >../serve.log 2>&1", .{r.dir});
        defer gpa.free(sh);
        r.child = try std.process.spawn(io, .{ .argv = &.{ "/bin/sh", "-c", sh }, .stdin = .ignore, .stdout = .ignore, .stderr = .ignore });
        errdefer r.stop(io);
        const log = try std.fmt.allocPrint(gpa, "{s}/serve.log", .{r.dir});
        defer gpa.free(log);
        var waited: u32 = 0;
        while (waited < 30_000) : (waited += 100) {
            if (cwd.readFileAlloc(io, log, gpa, .limited(1 << 16))) |d| {
                defer gpa.free(d);
                var it = std.mem.splitScalar(u8, d, '\n');
                while (it.next()) |line| {
                    if (std.mem.startsWith(u8, line, "serve: ")) {
                        const a = std.mem.trim(u8, line[7..], " \r");
                        @memcpy(r.addr_buf[0..a.len], a);
                        r.addr = r.addr_buf[0..a.len];
                        return r;
                    }
                }
            } else |_| {}
            harness.sleepMs(100);
        }
        std.debug.print("e2e: the remote server didn't start (see {s})\n", .{log});
        return error.RemoteStartTimeout;
    }

    fn stop(r: *Remote, io: std.Io) void {
        if (builtin.os.tag != .windows) if (r.child.id) |pid| std.posix.kill(pid, .KILL) catch {};
        r.child.kill(io);
    }

    fn deinit(r: *Remote, io: std.Io) void {
        r.stop(io);
        gpa.free(r.dir);
        gpa.destroy(r);
    }
};

// ---- Finder's tree, through uxd (bound at /tmp/f) ----

fn rcf(s: *Session, comptime fmt: []const u8, args: anytype) ![]u8 {
    const cmd = try std.fmt.allocPrint(gpa, fmt, args);
    defer gpa.free(cmd);
    return s.rc(cmd);
}

fn waitRcf(s: *Session, ms: u64, comptime fmt: []const u8, args: anytype) !void {
    const cmd = try std.fmt.allocPrint(gpa, fmt, args);
    defer gpa.free(cmd);
    s.waitRc(cmd, ms) catch |e| {
        s.saveScreen("e2e-fail");
        return e;
    };
}

/// Two numbers from the output of an rc command.
fn two(s: *Session, comptime fmt: []const u8, args: anytype) !harness.Point {
    const out = try rcf(s, fmt, args);
    defer gpa.free(out);
    var it = std.mem.tokenizeAny(u8, out, " \n");
    const x = try std.fmt.parseInt(i32, it.next() orelse return error.NoNumber, 10);
    const y = try std.fmt.parseInt(i32, it.next() orelse return error.NoNumber, 10);
    return pt(x, y);
}

/// The Finder window whose title is `title` (or of kind desktop).
fn finderWin(s: *Session, sel: []const u8, buf: []u8) ![]const u8 {
    const cmd = try std.fmt.allocPrint(gpa, "for(w in /tmp/f/windows/*) if({s}) basename $w", .{sel});
    defer gpa.free(cmd);
    try waitRcf(s, 15_000, "~ `{{{s}}} [0-9]*", .{cmd});
    const out = try s.rc(cmd);
    defer gpa.free(out);
    const w = std.mem.trim(u8, out, " \n");
    @memcpy(buf[0..w.len], w);
    return buf[0..w.len];
}

/// Where an item's icon is on the screen (as test/finder does it).
fn itemAt(s: *Session, win: []const u8, label: []const u8) !harness.Point {
    const r = try two(s, "cat /tmp/f/windows/{s}/rect", .{win});
    const rmax = try two(s, "awk '{{print $3, $4}}' /tmp/f/windows/{s}/rect", .{win});
    const cv = try two(s, "awk '{{print $2, $3}}' /tmp/f/windows/{s}/controls/icons/rect", .{win});
    const border = @divTrunc((rmax.x - r.x) - cv.y, 2);
    const p = try two(s, "grep 'label={s} ' /tmp/f/windows/{s}/controls/icons/items | sed 's/.*pos=([0-9]+),([0-9]+).*/\\1 \\2/'", .{ label, win });
    return pt(r.x + border + p.x, r.y + border + cv.x + p.y);
}

fn doubleClick(s: *Session, p: harness.Point) void {
    s.click(p, .left);
    harness.sleepMs(60);
    s.click(p, .left);
}

fn waitItems(s: *Session, win: []const u8, needle: []const u8) !void {
    try waitRcf(s, 15_000, "grep -s -- '{s}' /tmp/f/windows/{s}/controls/icons/items", .{ needle, win });
}

/// The x centres of the menu bar's titles, from the screen: runs of
/// black in the bar, apart by more than a letter gap (as test/devux does).
fn barTitles(s: *Session) ![8]i32 {
    const img = try s.screenshot();
    defer img.deinit(gpa);
    var out = [_]i32{0} ** 8;
    var n: usize = 0;
    var first: i32 = -1;
    var last: i32 = -100;
    var x: i32 = 32;
    while (x < 1024) : (x += 1) {
        var ink = false;
        var y: i32 = 2;
        while (y < bar_h - 2) : (y += 1) {
            if (img.at(x, y) == 0x000000) ink = true;
        }
        if (!ink) continue;
        if (x - last > 8) {
            if (first >= 0 and n < out.len) {
                out[n] = @divTrunc(first + last, 2);
                n += 1;
            }
            first = x;
        }
        last = x;
    }
    if (first >= 0 and n < out.len) out[n] = @divTrunc(first + last, 2);
    return out;
}

/// Whether more than a few pixels in `r` differ from `before`.
const Changed = struct {
    before: harness.Image,
    r: harness.Rect,
    fn ok(c: *const Changed, img: harness.Image) bool {
        var n: usize = 0;
        var y = c.r.min.y;
        while (y < c.r.max.y) : (y += 1) {
            var x = c.r.min.x;
            while (x < c.r.max.x) : (x += 1) {
                if (img.at(x, y) != c.before.at(x, y)) n += 1;
            }
        }
        return n > 500;
    }
};

test "e2e: the v0.1 demo" {
    const io = std.testing.io;
    const remote = try Remote.start(io);
    defer remote.deinit(io);

    var s = try Session.start(.{ .name = "e2e", .rio_args = &.{"-u"} });
    defer s.stop();

    // 1. The session's services, in the agent's namespace.
    gpa.free(try rcf(s, "rc /mnt/term/cwd/test/e2e/services.rc '{s}'", .{remote.addr}));

    // An ordinary rc window and acme, before any 9/UX app.
    try s.openWindow(rect(40, 420, 440, 740), "echo plain >$work/plain; rc -i");
    gpa.free(try s.waitWork("plain", 10_000));
    try s.openWindow(rect(480, 420, 1000, 740), "acme");
    try s.waitRc("ls -p /srv | grep -s '^acme\\.'", 20_000);
    // The plumber gives a message to every reader of the port, so this
    // sees what acme gets. (acme's own file server isn't reachable from
    // outside its namespace.)
    gpa.free(try s.rc("cat /mnt/plumb/edit >$work/edit &"));

    // 2. Finder takes the desktop and shows both volumes.
    try s.openWindow(rect(100, 100, 600, 400), "rc /mnt/term/cwd/test/e2e/finder.rc");
    try s.waitRc("ls -p /mnt/ux/apps | grep -s '^finder-'", 20_000);
    gpa.free(try s.rc("mkdir -p /tmp/f && bind /mnt/ux/apps/`{ls -p /mnt/ux/apps | grep '^finder-' | sed 1q} /tmp/f"));
    try s.waitRc("! grep -s '^win=0 ' /dev/ux/desktop/state", 10_000);
    var db: [32]u8 = undefined;
    const desk = try finderWin(s, "grep -s '^desktop$' $w/kind", &db);
    try waitItems(s, desk, "label=home icon=disk");
    // not attached yet, so shown offline until it's opened (the label
    // is quoted: 'label=remote (offline)')
    try waitItems(s, desk, "label=remote .offline.. icon=disk");
    try s.settle();
    s.saveScreen("e2e-desktop");

    // 3. The remote volume opens: Finder attaches it and lists the server's files.
    doubleClick(s, try itemAt(s, desk, "remote .offline.."));
    var rb: [32]u8 = undefined;
    const rwin = try finderWin(s, "grep -s '^remote$' $w/title", &rb);
    try waitItems(s, rwin, "label=remote.txt icon=doc-text");
    try waitItems(s, rwin, "label=papers ");
    // and the desktop shows it online now
    try waitItems(s, desk, "label=remote icon=disk");
    try s.settle();
    s.saveScreen("e2e-remote");

    // 4. The home volume, its docs folder, then a double click on a
    // text file plumbs it to acme.
    doubleClick(s, try itemAt(s, desk, "home"));
    var hb: [32]u8 = undefined;
    const hwin = try finderWin(s, "grep -s '^home$' $w/title", &hb);
    try waitItems(s, hwin, "label=docs ");
    try s.settle();
    doubleClick(s, try itemAt(s, hwin, "docs"));
    var fb: [32]u8 = undefined;
    const dwin = try finderWin(s, "grep -s '^docs$' $w/title", &fb);
    try waitItems(s, dwin, "label=readme.txt icon=doc-text");
    try s.settle();
    try s.settle();
    const acme_r = rect(484, 424, 996, 736);
    const before = try s.screenshot();
    defer before.deinit(gpa);
    doubleClick(s, try itemAt(s, dwin, "readme.txt"));
    try waitRcf(s, 15_000, "grep -s '/demo/home/docs/readme.txt' $work/edit", .{});
    gpa.free(try s.rc("cat $work/edit"));
    // acme opened it: a new window in its column changes what it shows
    s.waitScreen(&Changed{ .before = before, .r = acme_r }, Changed.ok, 10_000) catch |e| {
        s.saveScreen("e2e-acme");
        return e;
    };
    try s.settle();
    s.saveScreen("e2e-plumbed");

    // 5. The sample app: it registers with uxd, and a click on File, New
    // in the menu bar reaches it.
    try s.openWindow(rect(200, 60, 700, 480), "rc /mnt/term/cwd/test/e2e/sample.rc");
    try s.waitRc("ls -p /mnt/ux/apps | grep -s '^sample-'", 20_000);
    const app_out = try s.rc("ls -p /mnt/ux/apps | grep '^sample-' | sed 1q");
    defer gpa.free(app_out);
    const app = std.mem.trim(u8, app_out, " \n");
    const swin_out = try rcf(s, "ls -p /mnt/ux/apps/{s}/windows | sed 1q", .{app});
    defer gpa.free(swin_out);
    const swin = std.mem.trim(u8, swin_out, " \n");
    const ctl = try std.fmt.allocPrint(gpa, "/mnt/ux/apps/{s}/windows/{s}/controls", .{ app, swin });
    defer gpa.free(ctl);
    try waitRcf(s, 10_000, "test -e {s}/status/value", .{ctl});
    // the bar follows the sample's focus
    try waitRcf(s, 10_000, "sed 1q /dev/ux/menubar/menus | grep -s '^focus .*app={s}'", .{app});
    try s.settle();
    const t = try barTitles(s);
    // [0] the app's title, [1] File
    const file = pt(t[1], 10);
    s.click(file, .left);
    harness.sleepMs(200);
    s.click(pt(file.x + 6, bar_h + 9 + 18 * 1), .left); // New
    try waitRcf(s, 10_000, "grep -s '^new$' {s}/status/value", .{ctl});
    s.saveScreen("e2e-sample");

    // 6. From rc: the apps, a text field's value, and a button pressed through ctl.
    const apps = try s.rc("ls -p /mnt/ux/apps");
    defer gpa.free(apps);
    try std.testing.expect(std.mem.indexOf(u8, apps, "finder-") != null);
    try std.testing.expect(std.mem.indexOf(u8, apps, app) != null);
    gpa.free(try rcf(s, "echo 'set ''from rc''' >{s}/name/ctl", .{ctl}));
    try waitRcf(s, 5_000, "~ `{{cat {s}/name/value}} 'from rc'", .{ctl});
    gpa.free(try rcf(s, "cat {s}/save/events >$work/events.tmp & sleep 0.3; echo activate >{s}/save/ctl", .{ ctl, ctl }));
    try waitRcf(s, 10_000, "grep -s '^saved from rc' {s}/status/value", .{ctl});
    // the press was also seen by an independent events reader
    try waitRcf(s, 5_000, "grep -s 'activate' $work/events.tmp", .{});

    // 7. Stock behaviour still holds: acme has its windows, the rc
    // window still takes wctl and typing.
    const wins = try s.windows();
    defer gpa.free(wins);
    try std.testing.expect(wins.len >= 5);
    // stock wctl moves a window once it's current
    gpa.free(try s.rc("for(w in /dev/wsys/*) if(~ `{cat $w/label} rc*) { echo current >$w/wctl; echo move -minx 30 -miny 430 >$w/wctl; basename $w >$work/plainwin }; status=()"));
    const pw = try s.waitWork("plainwin", 5_000);
    defer gpa.free(pw);
    const pid = try std.fmt.parseInt(u32, std.mem.trim(u8, pw, " \n"), 10);
    try std.testing.expectEqual(rect(30, 430, 430, 750), try s.windowRect(pid));
    // and it still runs what's typed into it
    try s.typeText("echo typed >$work/typed\n");
    gpa.free(try s.waitWork("typed", 10_000));
    try s.settle();
    s.saveScreen("e2e-final");
}
