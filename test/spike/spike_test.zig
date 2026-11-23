//! The uitree spike (apps/spike): flexbox layout, svg and styled-text
//! on libux. Not a regression test yet: it checks that the layout put
//! controls where flexbox says, and saves screenshots for people to look
//! at (.local/test/artifacts/spike-*.png). Build libux, resfs and
//! apps/spike first.
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const gpa = std.testing.allocator;

const tree = "/tmp/sp/windows/1/controls";

fn ctlRect(s: *harness.Session, id: []const u8) ![4]i32 {
    const cmd = try std.fmt.allocPrint(gpa, "cat " ++ tree ++ "/{s}/rect", .{id});
    defer gpa.free(cmd);
    const out = try s.rc(cmd);
    defer gpa.free(out);
    var r: [4]i32 = undefined;
    var it = std.mem.tokenizeAny(u8, out, " \n");
    for (&r) |*v| v.* = try std.fmt.parseInt(i32, it.next() orelse return error.BadRect, 10);
    return r;
}

fn open(s: *harness.Session, win: harness.Rect, args: []const u8) !void {
    const cmd = try std.fmt.allocPrint(gpa, "rc /mnt/term/cwd/test/spike/spike.rc {s}", .{args});
    defer gpa.free(cmd);
    try s.openWindow(win, cmd);
    gpa.free(try s.waitWindows(1));
    s.waitRc("test -e /srv/ux.$user.spike", 15_000) catch |e| {
        s.saveScreen("spike-fail");
        s.rcQuiet("cat $work/spike.err") catch {};
        return e;
    };
    _ = try s.rc("mkdir -p /tmp/sp && mount /srv/ux.$user.spike /tmp/sp");
}

test "spike: a mail screen laid out by flexbox, then narrower" {
    var s = try harness.Session.start(.{ .name = "spike-mail", .rio_args = &.{"-u"} });
    defer s.stop();
    try open(s, rect(40, 40, 900, 600), "");
    try s.settle();
    s.saveScreen("spike-mail");

    // the buttons sit at the bottom right of main, Send last
    const send = try ctlRect(s, "send");
    const cancel = try ctlRect(s, "cancel");
    const main = try ctlRect(s, "main");
    try std.testing.expect(send[0] > cancel[2]);
    try std.testing.expect(main[2] - send[2] == 13); // p=12 and the 1px border
    // the sidebar keeps its w=128
    const side = try ctlRect(s, "side");
    try std.testing.expectEqual(@as(i32, 128), side[2] - side[0]);

    // narrower: main shrinks, the paragraph rewraps, the sidebar doesn't
    const ids = try s.windows();
    defer gpa.free(ids);
    var p: [64]u8 = undefined;
    try s.writeFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/wctl", .{ids[0]}), "resize -r 40 40 620 640");
    try s.waitRc("! ~ `{awk '{print $3}' " ++ tree ++ "/main/rect} " ++ "x", 5_000);
    harness.sleepMs(500);
    try s.settle();
    s.saveScreen("spike-mail-narrow");
    const side2 = try ctlRect(s, "side");
    try std.testing.expectEqual(@as(i32, 128), side2[2] - side2[0]);
    const main2 = try ctlRect(s, "main");
    try std.testing.expect(main2[2] - main2[0] < main[2] - main[0]);
}

test "spike: the svg gallery" {
    var s = try harness.Session.start(.{ .name = "spike-svg", .rio_args = &.{"-u"} });
    defer s.stop();
    try open(s, rect(40, 40, 800, 600), "-s svg");
    try s.settle();
    s.saveScreen("spike-svg");
}
