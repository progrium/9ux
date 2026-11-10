//! rio's /dev/ux (docs/protocols/devux.md): the menu bar following
//! focus, menu commands by click, by Command key and by file write, the
//! desktop layer getting the clicks nothing else takes, and mode off.
//!
//! The windows run app.rc (an app with File and Edit menus) and desk.rc
//! (a desktop client) from this directory.
const std = @import("std");
const harness = @import("harness");
const gpa = std.testing.allocator;
const rect = harness.rect;
const pt = harness.pt;
const Session = harness.Session;

/// rio's Command key (Kmod4 in keyboard.h).
const Kmod4: u21 = 0xF868;

/// The bar with the default font (vga, 8x16): 16 + 2*2 + 1 pixels.
const bar_h = 21;
/// Title positions for the app "Hello" with menus File and Edit:
/// rio at 8, then each title 2*8 pixels after the one before.
const hello_title = pt(68, 10);
const file_title = pt(118, 10);
const edit_title = pt(166, 10);
/// Items in a drop-down are 20 pixels high, from y 22.
fn item(title: harness.Point, n: i32) harness.Point {
    return pt(title.x + 10, bar_h + 1 + 20 * n + 10);
}

fn start(name: []const u8) !*Session {
    return Session.start(.{ .name = name, .rio_args = &.{"-u"} });
}

/// Waits until work file `name` contains `needle`.
fn waitFor(s: *Session, name: []const u8, needle: []const u8) !void {
    var waited: u64 = 0;
    while (true) : (waited += 50) {
        if (try s.readWork(name)) |d| {
            defer gpa.free(d);
            if (std.mem.indexOf(u8, d, needle) != null) return;
            if (waited >= 10_000) {
                std.debug.print("{s} has no '{s}':\n{s}\n", .{ name, needle, d });
                s.saveScreen(name);
                return error.WaitTimeout;
            }
        } else if (waited >= 10_000) {
            std.debug.print("no {s}\n", .{name});
            return error.WaitTimeout;
        }
        harness.sleepMs(50);
    }
}

fn workHas(s: *Session, name: []const u8, needle: []const u8) !bool {
    const d = (try s.readWork(name)) orelse return false;
    defer gpa.free(d);
    return std.mem.indexOf(u8, d, needle) != null;
}

/// The window ids, in order of creation.
fn ids(s: *Session, n: usize) ![]u32 {
    return s.waitWindows(n);
}

/// Opens the Hello app's window (app hello-1) and waits until it's registered.
fn openHello(s: *Session, r: harness.Rect) !void {
    try s.openWindow(r, "rc /mnt/term/cwd/test/devux/app.rc hello-1 Hello");
    const d = try s.waitWork("hello-1.ready", 20_000);
    gpa.free(d);
}

/// Waits until menubar/menus says the bar shows `focus`.
fn waitFocus(s: *Session, focus: []const u8) !void {
    var waited: u64 = 0;
    while (true) : (waited += 50) {
        const m = try s.rc("sed 1q /dev/ux/menubar/menus");
        defer gpa.free(m);
        if (std.mem.eql(u8, std.mem.trim(u8, m, " \n"), focus)) return;
        if (waited >= 10_000) {
            std.debug.print("bar focus: want '{s}', have '{s}'\n", .{ focus, m });
            return error.WaitTimeout;
        }
        harness.sleepMs(50);
    }
}

fn blackIn(img: harness.Image, r: harness.Rect) usize {
    return img.count(r, 0x000000);
}

test "version and ctl" {
    var s = try start("devux-version");
    defer s.stop();
    const v = try s.readFile("/dev/ux/version");
    defer gpa.free(v);
    try std.testing.expectEqualStrings("devux 1.0\nfeature focus\nfeature desktop\nfeature menubar\n", v);
    const c = try s.readFile("/dev/ux/ctl");
    defer gpa.free(c);
    try std.testing.expectEqualStrings("mode on\ncmdkey mod4\n", c);
}

test "focus events and the menu bar follow the focused window" {
    var s = try start("devux-focus");
    defer s.stop();
    try s.rcQuiet("cat /dev/ux/events >$work/events &");
    try openHello(s, rect(100, 100, 500, 400));
    try s.openWindow(rect(550, 100, 950, 400), "echo -n b >/dev/label; sleep 1000");
    const w = try ids(s, 2);
    defer gpa.free(w);
    const a, const b = .{ w[0], w[1] };

    // The newest window has focus: the bar shows only rio and its label.
    var want = try std.fmt.allocPrint(gpa, "focus win={d}", .{b});
    try waitFocus(s, want);
    gpa.free(want);

    s.click(pt(300, 250), .left);
    want = try std.fmt.allocPrint(gpa, "focus app=hello-1 win={d}", .{a});
    try waitFocus(s, want);
    gpa.free(want);
    want = try std.fmt.allocPrint(gpa, "focus win={d} app=hello-1", .{a});
    try waitFor(s, "events", want);
    gpa.free(want);
    try s.settle();
    {
        const img = try s.screenshot();
        defer img.deinit(gpa);
        try std.testing.expect(blackIn(img, rect(100, 2, 136, 19)) > 0); // File
        try std.testing.expect(blackIn(img, rect(148, 2, 184, 19)) > 0); // Edit
    }

    s.click(pt(750, 250), .left);
    want = try std.fmt.allocPrint(gpa, "focus win={d}", .{b});
    try waitFocus(s, want);
    gpa.free(want);
    try s.settle();
    {
        const img = try s.screenshot();
        defer img.deinit(gpa);
        try std.testing.expectEqual(0, blackIn(img, rect(100, 2, 200, 19)));
    }
    want = try std.fmt.allocPrint(gpa, "focus app=hello-1 win={d}", .{a});
    try waitFor(s, "hello-1.events", want);
    gpa.free(want);
}

test "menu commands arrive by click, by drag, by Command key and by ctl" {
    var s = try start("devux-actions");
    defer s.stop();
    try openHello(s, rect(100, 100, 500, 400));
    const w = try ids(s, 1);
    defer gpa.free(w);
    var fbuf: [64]u8 = undefined;
    try waitFocus(s, try std.fmt.bufPrint(&fbuf, "focus app=hello-1 win={d}", .{w[0]}));

    // Click File, then click Quit.
    s.click(file_title, .left);
    harness.sleepMs(100);
    s.click(item(file_title, 1), .left);
    var want = try std.fmt.allocPrint(gpa, "action app=hello-1 win={d} id=quit via=click", .{w[0]});
    try waitFor(s, "hello-1.events", want);
    gpa.free(want);

    // Press on Edit, drag to Copy, release.
    s.drag(edit_title, item(edit_title, 1), .left);
    want = try std.fmt.allocPrint(gpa, "action app=hello-1 win={d} id=copy via=click", .{w[0]});
    try waitFor(s, "hello-1.events", want);
    gpa.free(want);

    // Undo is disabled: choosing it does nothing.
    s.drag(edit_title, item(edit_title, 0), .left);

    // Command-N, and Command-Z (disabled: swallowed, no action).
    s.keyEvent(Kmod4, true);
    try s.key('z');
    try s.key('n');
    s.keyEvent(Kmod4, false);
    want = try std.fmt.allocPrint(gpa, "action app=hello-1 win={d} id=new via=key", .{w[0]});
    try waitFor(s, "hello-1.events", want);
    gpa.free(want);

    // A file write.
    try s.rcQuiet("echo invoke hello-1 copy >/dev/ux/menubar/ctl");
    try waitFor(s, "hello-1.events", "action app=hello-1 win=0 id=copy via=ctl");
    const r = try s.exec("echo invoke hello-1 undo >/dev/ux/menubar/ctl", 10_000);
    defer r.deinit(gpa);
    try std.testing.expect(!r.ok());
    try std.testing.expect(std.mem.indexOf(u8, r.out, "disabled") != null);

    try std.testing.expect(!try workHas(s, "hello-1.events", "id=undo"));
}

test "Command keys reach a window that has no app" {
    var s = try start("devux-legacy-keys");
    defer s.stop();
    try s.openWindow(rect(100, 100, 500, 400), "cat /dev/kbd >$work/kbd");
    const w = try ids(s, 1);
    defer gpa.free(w);
    var fbuf: [64]u8 = undefined;
    try waitFocus(s, try std.fmt.bufPrint(&fbuf, "focus win={d}", .{w[0]}));
    harness.sleepMs(300);
    s.keyEvent(Kmod4, true);
    try s.key('q');
    s.keyEvent(Kmod4, false);
    // The window saw q go down with Command held.
    try waitFor(s, "kbd", "q");
}

test "the desktop gets clicks no window takes, and stays behind" {
    var s = try start("devux-desktop");
    defer s.stop();
    try s.rcQuiet("cat /dev/ux/desktop/events >$work/devents &");
    try s.openWindow(rect(100, 100, 500, 400), "rc /mnt/term/cwd/test/devux/desk.rc");
    gpa.free(try s.waitWork("desk.ready", 20_000));
    try s.openWindow(rect(300, 300, 700, 600), "sleep 1000");
    const w = try ids(s, 2);
    defer gpa.free(w);
    const desk, const top = .{ w[0], w[1] };

    var want = try std.fmt.allocPrint(gpa, "claim win={d}", .{desk});
    try waitFor(s, "devents", want);
    gpa.free(want);
    const st = try s.readFile("/dev/ux/desktop/state");
    defer gpa.free(st);
    want = try std.fmt.allocPrint(gpa, "win={d} rect=0,{d},1024,768\n", .{ desk, bar_h });
    try std.testing.expectEqualStrings(want, st);
    gpa.free(want);
    // It covers the screen below the bar, border outside.
    try std.testing.expectEqual(rect(-4, bar_h - 4, 1028, 772), try s.windowRect(desk));

    // A click on the desktop goes to the desktop client...
    s.click(pt(900, 700), .left);
    try waitFor(s, "desk.mouse", "900         700           1 ");
    // ...and it stays behind the other window.
    s.click(pt(500, 450), .left);
    harness.sleepMs(300);
    try std.testing.expect(!try workHas(s, "desk.mouse", "500         450"));
    const ws = try s.readFile("/dev/ux/windows");
    defer gpa.free(ws);
    want = try std.fmt.allocPrint(gpa, "window win={d} ", .{desk});
    try std.testing.expect(std.mem.startsWith(u8, ws, want));
    gpa.free(want);
    try std.testing.expect(std.mem.indexOf(u8, ws, "state=current,desktop") != null or
        std.mem.indexOf(u8, ws, "state=desktop") != null);
    _ = top;

    // wctl can't move it.
    const cmd = try std.fmt.allocPrint(gpa, "echo move -minx 10 -miny 40 >/dev/wsys/{d}/wctl", .{desk});
    defer gpa.free(cmd);
    const r = try s.exec(cmd, 10_000);
    defer r.deinit(gpa);
    try std.testing.expect(!r.ok());
}

test "mode off is stock rio: no bar, desktop hidden" {
    var s = try start("devux-mode");
    defer s.stop();
    try s.rcQuiet("cat /dev/ux/events >$work/events &");
    try s.settle();
    {
        const img = try s.screenshot();
        defer img.deinit(gpa);
        try std.testing.expectEqual(@as(u32, 0xFFFFFF), img.at(600, 5));
        try std.testing.expectEqual(@as(u32, 0x000000), img.at(600, bar_h - 1));
    }
    try s.rcQuiet("echo mode off >/dev/ux/ctl");
    try waitFor(s, "events", "mode ux=off");
    try s.settle();
    {
        const img = try s.screenshot();
        defer img.deinit(gpa);
        try std.testing.expectEqual(@as(u32, 0x777777), img.at(600, 5));
    }
    const c = try s.readFile("/dev/ux/ctl");
    defer gpa.free(c);
    try std.testing.expectEqualStrings("mode off\ncmdkey mod4\n", c);
}
