//! The devices rio multiplexes for each window: /dev/snarf, /dev/kbd,
//! /dev/mouse and /dev/cons.
const std = @import("std");
const harness = @import("harness");
const c = @import("common.zig");
const gpa = c.gpa;
const rect = harness.rect;
const pt = harness.pt;

test "/dev/snarf written in one window reads back in another" {
    var s = try harness.Session.start(.{ .name = "dev-snarf" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 300), "echo -n from window one >/dev/snarf; sleep 1000");
    gpa.free(try s.waitWindows(1));
    harness.sleepMs(300);
    try s.openWindow(rect(500, 100, 900, 300), "cat /dev/snarf >$work/snarf.tmp && mv $work/snarf.tmp $work/snarf; sleep 1000");
    const got = try s.waitWork("snarf", 20_000);
    defer gpa.free(got);
    try std.testing.expectEqualStrings("from window one", got);
}

test "button 2 snarf copies selected text into /dev/snarf" {
    var s = try harness.Session.start(.{ .name = "dev-snarf-menu" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 600, 400), "echo selectme; sleep 1000");
    const id = try c.onlyWindow(s);
    var p: [64]u8 = undefined;
    try s.waitRc(try std.fmt.bufPrint(&p, "grep -s selectme /dev/wsys/{d}/text", .{id}), 10_000);
    // Double-click selects the word; the text starts at the window's
    // top left, inside the border and scroll bar.
    const r = try s.windowRect(id);
    const word = pt(r.min.x + 4 + 12 + 4 + 20, r.min.y + 4 + 1 + 8);
    s.click(word, .left);
    s.click(word, .left);
    // snarf is the third item of the button 2 menu.
    s.menuPick(word, .middle, 2, 0);
    try s.waitRc("~ `{cat /dev/snarf} selectme", 10_000);
}

test "/dev/kbd gives a window's reader key presses and releases" {
    var s = try harness.Session.start(.{ .name = "dev-kbd" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 600, 400), "dd -quiet 1 -bs 64 -count 1 -if /dev/kbd -of $work/kbd.tmp && mv $work/kbd.tmp $work/kbd; sleep 1000");
    const id = try c.onlyWindow(s);
    try c.waitState(s, id, "current");
    harness.sleepMs(500);
    var waited: u32 = 0;
    const got = while (true) : (waited += 200) {
        if (try s.readWork("kbd")) |d| break d;
        if (waited > 20_000) return error.WaitTimeout;
        try s.key('q');
        harness.sleepMs(200);
    };
    defer gpa.free(got);
    // kbdfs's format: 'k' and the keys now held, each message ending
    // in a NUL.
    try std.testing.expectEqualStrings("kq\x00", got);
}

test "/dev/mouse gives a window's reader screen coordinates and buttons" {
    var s = try harness.Session.start(.{ .name = "dev-mouse" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 600, 400), "dd -quiet 1 -bs 49 -count 1 -if /dev/mouse -of $work/mouse.tmp && mv $work/mouse.tmp $work/mouse; sleep 1000");
    const id = try c.onlyWindow(s);
    try c.waitState(s, id, "current");
    harness.sleepMs(500);
    var waited: u32 = 0;
    var x: i32 = 300;
    const got = while (true) : (waited += 200) {
        if (try s.readWork("mouse")) |d| break d;
        if (waited > 20_000) return error.WaitTimeout;
        x += 1;
        s.move(pt(x, 250));
        harness.sleepMs(200);
    };
    defer gpa.free(got);
    try std.testing.expectEqual(49, got.len);
    try std.testing.expectEqual('m', got[0]);
    var it = std.mem.tokenizeScalar(u8, got[1..], ' ');
    const gx = try std.fmt.parseInt(i32, it.next().?, 10);
    const gy = try std.fmt.parseInt(i32, it.next().?, 10);
    const b = try std.fmt.parseInt(i32, it.next().?, 10);
    try std.testing.expect(gx > 300 and gx <= x);
    try std.testing.expectEqual(250, gy);
    try std.testing.expectEqual(0, b);
}

test "typing into a window reaches its /dev/cons" {
    var s = try harness.Session.start(.{ .name = "dev-cons" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 600, 400), "read >$work/cons.tmp && mv $work/cons.tmp $work/cons; sleep 1000");
    const id = try c.onlyWindow(s);
    try c.waitState(s, id, "current");
    harness.sleepMs(300);
    try s.typeText("hello, cons\n");
    const got = try s.waitWork("cons", 20_000);
    defer gpa.free(got);
    try std.testing.expectEqualStrings("hello, cons\n", got);
}
