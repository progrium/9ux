//! Shared by the end-to-end tests: the remote volume, Desktop's tree
//! through uxd, and finding things on the screen.
const std = @import("std");
const builtin = @import("builtin");
const harness = @import("harness");
const rect = harness.rect;
const pt = harness.pt;
const Session = harness.Session;
const gpa = std.testing.allocator;

/// The menu bar's height (assets/README.md).
pub const bar_h = 20;

// ---- The remote volume: a second h9 serving a directory over 9P ----

pub const Remote = struct {
    child: std.process.Child,
    addr_buf: [64]u8 = undefined,
    addr: []const u8 = "",
    dir: [:0]u8,

    /// Serves `.local/test/e2e-remote/share` (made fresh, with a few
    /// files) from a second h9 on a loopback port.
    pub fn start(io: std.Io) !*Remote {
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
        // The served tree is the server's cwd, as in desktop/test/remote_test.sh.
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

    pub fn stop(r: *Remote, io: std.Io) void {
        if (builtin.os.tag != .windows) if (r.child.id) |pid| std.posix.kill(pid, .KILL) catch {};
        r.child.kill(io);
    }

    pub fn deinit(r: *Remote, io: std.Io) void {
        r.stop(io);
        gpa.free(r.dir);
        gpa.destroy(r);
    }
};

// ---- Desktop's tree, through uxd (bound at /tmp/f) ----

pub fn rcf(s: *Session, comptime fmt: []const u8, args: anytype) ![]u8 {
    const cmd = try std.fmt.allocPrint(gpa, fmt, args);
    defer gpa.free(cmd);
    return s.rc(cmd);
}

pub fn waitRcf(s: *Session, ms: u64, comptime fmt: []const u8, args: anytype) !void {
    const cmd = try std.fmt.allocPrint(gpa, fmt, args);
    defer gpa.free(cmd);
    s.waitRc(cmd, ms) catch |e| {
        s.saveScreen("e2e-fail");
        return e;
    };
}

/// Two numbers from the output of an rc command.
pub fn two(s: *Session, comptime fmt: []const u8, args: anytype) !harness.Point {
    const out = try rcf(s, fmt, args);
    defer gpa.free(out);
    var it = std.mem.tokenizeAny(u8, out, " \n");
    const x = try std.fmt.parseInt(i32, it.next() orelse return error.NoNumber, 10);
    const y = try std.fmt.parseInt(i32, it.next() orelse return error.NoNumber, 10);
    return pt(x, y);
}

/// The Desktop window `sel` picks, as an rc condition on $w (a
/// window's directory): its title, or `~ $w */main` for the desktop.
pub fn desktopWin(s: *Session, sel: []const u8, buf: []u8) ![]const u8 {
    const cmd = try std.fmt.allocPrint(gpa, "for(w in /tmp/f/win/*) if({s}) basename $w; status=()", .{sel});
    defer gpa.free(cmd);
    try waitRcf(s, 15_000, "~ `{{{s}}} ?*", .{cmd});
    const out = try s.rc(cmd);
    defer gpa.free(out);
    const w = std.mem.trim(u8, out, " \n");
    @memcpy(buf[0..w.len], w);
    return buf[0..w.len];
}

/// A window's place on the screen: x0 y0 (or x1 y1, with `max`).
pub fn winRect(s: *Session, win: []const u8, max: bool) !harness.Point {
    if (max)
        return two(s, "awk '$1 == \"{s}\" {{print $5, $6}}' /tmp/f/debug/windows", .{win});
    return two(s, "awk '$1 == \"{s}\" {{print $3, $4}}' /tmp/f/debug/windows", .{win});
}

/// A window's icon view, under /tmp/f: a folder's is in its scroll view.
pub fn iconsOf(win: []const u8, buf: []u8) ![]const u8 {
    if (std.mem.eql(u8, win, "main"))
        return std.fmt.bufPrint(buf, "win/main/icons", .{});
    return std.fmt.bufPrint(buf, "win/{s}/scroll/icons", .{win});
}

/// Where an item's icon is on the screen (as test/desktop does it).
pub fn itemAt(s: *Session, win: []const u8, label: []const u8) !harness.Point {
    const r = try winRect(s, win, false);
    const rmax = try winRect(s, win, true);
    // The icon view's top, and the window's drawable width: a folder
    // window's scroll view is that outset by 1; the desktop's icon view is it.
    const cv = try two(s, "c=/tmp/f/win/{s}; if(test -e $c/scroll){{ awk '{{print $2}}' $c/scroll/icons/bounds; awk '{{print $1+$3-1}}' $c/scroll/bounds }}; if not {{ awk '{{print $2}}' $c/icons/bounds; awk '{{print $1+$3}}' $c/icons/bounds }}", .{win});
    const border = @divTrunc((rmax.x - r.x) - cv.y, 2);
    var ib: [128]u8 = undefined;
    const p = try two(s, "grep 'label={s} ' /tmp/f/{s}/items | sed 's/.*pos=([0-9]+),([0-9]+).*/\\1 \\2/'", .{ label, try iconsOf(win, &ib) });
    return pt(r.x + border + p.x, r.y + border + cv.x + p.y);
}

pub fn doubleClick(s: *Session, p: harness.Point) void {
    s.click(p, .left);
    harness.sleepMs(60);
    s.click(p, .left);
}

pub fn waitItems(s: *Session, win: []const u8, needle: []const u8) !void {
    var ib: [128]u8 = undefined;
    try waitRcf(s, 15_000, "grep -s -- '{s}' /tmp/f/{s}/items", .{ needle, try iconsOf(win, &ib) });
}

/// The x centres of the menu bar's titles, from the screen: runs of
/// ink in the bar (what isn't its background, in either theme), apart by
/// more than a letter gap (as test/devux does).
pub fn barTitles(s: *Session) ![8]i32 {
    const img = try s.screenshot();
    defer img.deinit(gpa);
    var out = [_]i32{0} ** 8;
    var n: usize = 0;
    var first: i32 = -1;
    var last: i32 = -100;
    var x: i32 = 32;
    while (x < 1024 - 100) : (x += 1) { // the clock is at the right
        var ink = false;
        var y: i32 = 2;
        while (y < bar_h - 2) : (y += 1) {
            if (img.at(x, y) != img.at(x, 0)) ink = true; // not the bar: ink in either theme
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
pub const Changed = struct {
    before: harness.Image,
    r: harness.Rect,
    pub fn ok(c: *const Changed, img: harness.Image) bool {
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

