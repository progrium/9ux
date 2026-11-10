//! Drag and drop end to end (workplan S9's "done when"): Finder's files
//! dragged between its own folder windows (a move within a volume, a
//! copy onto another volume), and onto another app, the libux sample,
//! whose Notes field takes a dropped file's contents through rio's
//! drag broker.
//!
//! Build everything first (scripts/test.sh does).
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const pt = harness.pt;
const Session = harness.Session;
const gpa = std.testing.allocator;

const c = @import("common.zig");

/// A drag as a hand makes it: press, a pause, small steps with time
/// between them, a pause over the target, then release.
fn slowDrag(s: *Session, from: harness.Point, to: harness.Point) void {
    s.move(from);
    harness.sleepMs(50);
    s.press(.left);
    harness.sleepMs(100);
    const steps = 16;
    for (1..steps + 1) |i| {
        const k: i32 = @intCast(i);
        s.move(.{
            .x = from.x + @divTrunc((to.x - from.x) * k, steps),
            .y = from.y + @divTrunc((to.y - from.y) * k, steps),
        });
        harness.sleepMs(30);
    }
    harness.sleepMs(300);
    s.release(.left);
}

/// Makes a Finder window current with a click in its status line (as
/// in stock rio, the click that makes a window current goes no further).
fn activate(s: *Session, win: []const u8) !void {
    const r = try c.two(s, "cat /tmp/f/windows/{s}/rect", .{win});
    s.click(pt(r.x + 300, r.y + 12), .left);
    c.waitRcf(s, 5_000, "grep -s current /tmp/f/windows/{s}/state", .{win}) catch |e| {
        const d = s.rc("cat /dev/ux/drag/state /dev/ux/windows; cat /tmp/f/windows/*/state") catch null;
        if (d) |x| {
            std.debug.print("rio and Finder:\n{s}\n", .{x});
            gpa.free(x);
        }
        return e;
    };
    try s.settle();
}

fn hostExists(s: *Session, rel: []const u8) bool {
    const d = (s.readWork(rel) catch return false) orelse return false;
    gpa.free(d);
    return true;
}

fn waitHost(s: *Session, rel: []const u8, want: bool) !void {
    var waited: u64 = 0;
    while (hostExists(s, rel) != want) : (waited += 50) {
        if (waited >= 10_000) {
            std.debug.print("e2e-drag: {s} should {s}exist\n", .{ rel, if (want) "" else "not " });
            s.saveScreen("e2e-drag-fail");
            return error.WaitTimeout;
        }
        harness.sleepMs(50);
    }
}

test "e2e: dragging files between Finder windows and onto an app" {
    var s = try Session.start(.{ .name = "e2e-drag", .rio_args = &.{"-u"} });
    defer s.stop();

    // Where Finder puts the two folder windows, in its uxmeta layout.
    gpa.free(try s.rc(
        \\t=$work/demo
        \\mkdir -p $t/user/workspace
        \\echo 'folder='$t'/home x=40 y=60 width=380 height=300 view=icons' >$t/user/workspace/demo.ndb
        \\echo 'folder='$t'/home/docs x=460 y=60 width=380 height=300 view=icons' >>$t/user/workspace/demo.ndb
    ));
    gpa.free(try s.rc("rc /mnt/term/cwd/test/e2e/services.rc"));
    gpa.free(try s.rc("echo 'a third file' >$work/demo/home/third.txt"));

    try s.openWindow(rect(100, 100, 600, 400), "rc /mnt/term/cwd/test/e2e/finder.rc");
    try s.waitRc("ls -p /mnt/ux/apps | grep -s '^finder-'", 20_000);
    gpa.free(try s.rc("mkdir -p /tmp/f && bind /mnt/ux/apps/`{ls -p /mnt/ux/apps | grep '^finder-' | sed 1q} /tmp/f"));
    try s.waitRc("! grep -s '^win=0 ' /dev/ux/desktop/state", 10_000);
    var db: [32]u8 = undefined;
    const desk = try c.finderWin(s, "grep -s '^desktop$' $w/kind", &db);
    try c.waitItems(s, desk, "label=home icon=disk");
    try c.waitItems(s, desk, "label=other icon=disk");
    try s.settle();

    // home, then docs from it: two windows side by side
    c.doubleClick(s, try c.itemAt(s, desk, "home"));
    var hb: [32]u8 = undefined;
    const home = try c.finderWin(s, "grep -s '^home$' $w/title", &hb);
    try c.waitItems(s, home, "label=docs ");
    try c.waitItems(s, home, "label=notes.txt ");
    try s.settle();
    c.doubleClick(s, try c.itemAt(s, home, "docs"));
    var fb: [32]u8 = undefined;
    const docs = try c.finderWin(s, "grep -s '^docs$' $w/title", &fb);
    try c.waitItems(s, docs, "label=readme.txt ");
    try s.settle();
    s.saveScreen("e2e-drag-start");

    // 1. notes.txt from home's window into docs's: a move within the volume
    {
        try activate(s, home);
        const from = try c.itemAt(s, home, "notes.txt");
        const dr = try c.two(s, "cat /tmp/f/windows/{s}/rect", .{docs});
        slowDrag(s, from, pt(dr.x + 200, dr.y + 200));
        try waitHost(s, "demo/home/docs/notes.txt", true);
        try waitHost(s, "demo/home/notes.txt", false);
        try c.waitItems(s, docs, "label=notes.txt ");
        try c.waitRcf(s, 10_000, "! grep -s 'label=notes.txt ' /tmp/f/windows/{s}/controls/icons/items", .{home});
        try s.settle();
    }

    // 2. readme.txt onto the other volume's desktop icon: a copy across volumes
    {
        try activate(s, docs);
        const from = try c.itemAt(s, docs, "readme.txt");
        slowDrag(s, from, try c.itemAt(s, desk, "other"));
        try waitHost(s, "demo/other/readme.txt", true);
        try std.testing.expect(hostExists(s, "demo/home/docs/readme.txt"));
        try s.settle();
    }

    // 3. third.txt onto the sample app's Notes field: its contents arrive there
    try s.openWindow(rect(300, 380, 800, 760), "rc /mnt/term/cwd/test/e2e/sample.rc");
    try s.waitRc("ls -p /mnt/ux/apps | grep -s '^sample-'", 20_000);
    const app_out = try s.rc("ls -p /mnt/ux/apps | grep '^sample-' | sed 1q");
    defer gpa.free(app_out);
    const app = std.mem.trim(u8, app_out, " \n");
    const sw_out = try c.rcf(s, "ls -p /mnt/ux/apps/{s}/windows | sed 1q", .{app});
    defer gpa.free(sw_out);
    const sw = std.mem.trim(u8, sw_out, " \n");
    try c.waitRcf(s, 10_000, "test -e /mnt/ux/apps/{s}/windows/{s}/controls/notes/rect", .{ app, sw });
    try s.settle();
    // the Notes field's centre: the window's drawable origin (rio's
    // border inside the window's rect) plus the field's local rect
    const wr = try c.two(s, "cat /mnt/ux/apps/{s}/windows/{s}/rect", .{ app, sw });
    const nr = try c.two(s, "awk '{{print ($1+$3)/2, ($2+$4)/2}}' /mnt/ux/apps/{s}/windows/{s}/controls/notes/rect", .{ app, sw });
    const notes = pt(wr.x + 4 + nr.x, wr.y + 4 + nr.y);
    try activate(s, home);
    slowDrag(s, try c.itemAt(s, home, "third.txt"), notes);
    try c.waitRcf(s, 10_000, "grep -s '^dropped third.txt' /mnt/ux/apps/{s}/windows/{s}/controls/status/value", .{ app, sw });
    try c.waitRcf(s, 5_000, "grep -s '^a third file$' /mnt/ux/apps/{s}/windows/{s}/controls/notes/value", .{ app, sw });
    // a copy: the file is still in home
    try std.testing.expect(hostExists(s, "demo/home/third.txt"));
    try s.settle();
    s.saveScreen("e2e-drag-done");
}
