//! Changing windows: wctl messages written to /dev/wsys/N/wctl (the same
//! file a window sees as /dev/wctl), and the button-3 menu's Resize,
//! Move, Delete and Hide.
const std = @import("std");
const harness = @import("harness");
const c = @import("common.zig");
const gpa = c.gpa;
const rect = harness.rect;
const pt = harness.pt;

fn wctl(s: *harness.Session, id: u32, msg: []const u8) !void {
    var p: [64]u8 = undefined;
    try s.writeFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/wctl", .{id}), msg);
}

fn twoWindows(s: *harness.Session) ![2]u32 {
    try s.openWindow(rect(100, 100, 400, 300), "sleep 1000");
    gpa.free(try s.waitWindows(1));
    try s.openWindow(rect(500, 100, 900, 400), "sleep 1000");
    const ids = try s.waitWindows(2);
    defer gpa.free(ids);
    return .{ ids[0], ids[1] };
}

test "wctl resize, move and their -dx/-dy forms" {
    var s = try harness.Session.start(.{ .name = "wctl-geometry" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 300), "sleep 1000");
    const id = try c.onlyWindow(s);
    try wctl(s, id, "resize -r 50 60 450 360");
    try c.waitRect(s, id, rect(50, 60, 450, 360));
    try wctl(s, id, "move -minx 200 -miny 210");
    try c.waitRect(s, id, rect(200, 210, 600, 510));
    try wctl(s, id, "resize -dx 300 -dy 200");
    try c.waitRect(s, id, rect(200, 210, 500, 410));
    try wctl(s, id, "resize -maxx 700");
    try c.waitRect(s, id, rect(200, 210, 700, 410));
    // The screen follows: the old place is background again.
    try s.settle();
    const img = try s.screenshot();
    defer img.deinit(gpa);
    try std.testing.expectEqual(c.color.background, img.at(120, 120));
    try std.testing.expectEqual(c.color.border_current, img.at(201, 300));
}

test "wctl hide and unhide" {
    var s = try harness.Session.start(.{ .name = "wctl-hide" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 300), "sleep 1000");
    const id = try c.onlyWindow(s);
    try c.waitState(s, id, "visible");
    try wctl(s, id, "hide");
    try c.waitState(s, id, "hidden");
    try s.settle();
    {
        const img = try s.screenshot();
        defer img.deinit(gpa);
        try std.testing.expectEqual(0, img.count(rect(100, 100, 400, 300), c.color.text_bg));
    }
    try wctl(s, id, "unhide");
    try c.waitState(s, id, "visible");
    try c.waitRect(s, id, rect(100, 100, 400, 300));
}

test "wctl current, top and bottom" {
    var s = try harness.Session.start(.{ .name = "wctl-current" });
    defer s.stop();
    const w = try twoWindows(s);
    // The newest window is current.
    try c.waitState(s, w[1], "current");
    try c.waitState(s, w[0], "notcurrent");
    try wctl(s, w[0], "current");
    try c.waitState(s, w[0], "current");
    try c.waitState(s, w[1], "notcurrent");
    // Only the current window can be moved or resized through wctl.
    var cmd: [96]u8 = undefined;
    const r = try s.exec(try std.fmt.bufPrint(&cmd, "echo move -minx 200 -miny 150 >/dev/wsys/{d}/wctl", .{w[1]}), 10_000);
    defer r.deinit(gpa);
    try std.testing.expect(std.mem.indexOf(u8, r.out, "window not current") != null);
    // Overlap them, then raise and lower the first.
    try wctl(s, w[1], "current");
    try c.waitState(s, w[1], "current");
    try wctl(s, w[1], "move -minx 200 -miny 150");
    try c.waitRect(s, w[1], rect(200, 150, 600, 450));
    try wctl(s, w[0], "top");
    try s.settle();
    {
        const img = try s.screenshot();
        defer img.deinit(gpa);
        // The first window's right border is on top of the second.
        try std.testing.expect(img.at(398, 250) != c.color.text_bg);
    }
    try wctl(s, w[0], "bottom");
    try s.settle();
    {
        const img = try s.screenshot();
        defer img.deinit(gpa);
        try std.testing.expectEqual(c.color.text_bg, img.at(398, 250));
    }
}

test "wctl delete removes the window" {
    var s = try harness.Session.start(.{ .name = "wctl-delete" });
    defer s.stop();
    const w = try twoWindows(s);
    try wctl(s, w[0], "delete");
    const left = try s.waitWindows(1);
    defer gpa.free(left);
    try std.testing.expectEqual(w[1], left[0]);
}

test "a bad wctl message is an error" {
    var s = try harness.Session.start(.{ .name = "wctl-bad" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 300), "sleep 1000");
    const id = try c.onlyWindow(s);
    var cmd: [96]u8 = undefined;
    const r = try s.exec(try std.fmt.bufPrint(&cmd, "echo frobnicate >/dev/wsys/{d}/wctl", .{id}), 10_000);
    defer r.deinit(gpa);
    try std.testing.expect(!r.ok());
    // And a rectangle too small for a window is refused.
    const r2 = try s.exec(try std.fmt.bufPrint(&cmd, "echo resize -r 0 0 10 10 >/dev/wsys/{d}/wctl", .{id}), 10_000);
    defer r2.deinit(gpa);
    try std.testing.expect(!r2.ok());
    try c.waitRect(s, id, rect(100, 100, 400, 300));
}

test "button 3 Resize sweeps a new rectangle for the window pointed to" {
    var s = try harness.Session.start(.{ .name = "menu-resize" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 300), "sleep 1000");
    const id = try c.onlyWindow(s);
    s.menuPick(pt(700, 600), .right, 1, 0);
    s.click(pt(250, 200), .right);
    s.drag(pt(500, 400), pt(800, 700), .right);
    try c.waitRect(s, id, rect(500, 400, 801, 701));
}

test "button 3 Move drags the window pointed to" {
    var s = try harness.Session.start(.{ .name = "menu-move" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 300), "sleep 1000");
    const id = try c.onlyWindow(s);
    s.menuPick(pt(700, 600), .right, 2, 0);
    s.drag(pt(250, 200), pt(450, 350), .right);
    try c.waitRect(s, id, rect(300, 250, 600, 450));
}

test "button 3 Hide, then the window's menu entry, unhides it" {
    var s = try harness.Session.start(.{ .name = "menu-hide" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 300), "sleep 1000");
    const id = try c.onlyWindow(s);
    var p: [64]u8 = undefined;
    try s.writeFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/label", .{id}), "hideme");
    s.menuPick(pt(700, 600), .right, 4, 0);
    s.click(pt(250, 200), .right);
    try c.waitState(s, id, "hidden");
    // Hidden windows are listed after Exit: item 6. The menu opens on
    // the last item picked, Hide.
    s.menuPick(pt(700, 600), .right, 6, 4);
    try c.waitState(s, id, "visible");
}

test "button 3 Delete deletes the window pointed to" {
    var s = try harness.Session.start(.{ .name = "menu-delete" });
    defer s.stop();
    const w = try twoWindows(s);
    s.menuPick(pt(700, 600), .right, 3, 0);
    s.click(pt(700, 250), .right);
    const left = try s.waitWindows(1);
    defer gpa.free(left);
    try std.testing.expectEqual(w[0], left[0]);
}
