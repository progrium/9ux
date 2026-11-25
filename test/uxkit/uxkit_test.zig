//! uxkit drawing a uitree client (test/uxkit/demo.rc) with libux: the
//! layout's bounds come back through $ui, a click on a card comes back
//! as uitree's click event with the card's tag, the client's answer to
//! it is drawn, and screenshots go to
//! .local/test/artifacts/uxkit-*.png. Build libux, resfs and uxkit first.
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const gpa = std.testing.allocator;

fn bounds(s: *harness.Session, path: []const u8) ![4]i32 {
    const cmd = try std.fmt.allocPrint(gpa, "cat /n/ui/win/main/{s}/bounds", .{path});
    defer gpa.free(cmd);
    const out = try s.rc(cmd);
    defer gpa.free(out);
    var r: [4]i32 = undefined;
    var it = std.mem.tokenizeAny(u8, out, " \n");
    for (&r) |*v| v.* = try std.fmt.parseInt(i32, it.next() orelse return error.BadBounds, 10);
    return r;
}

test "uxkit: a uitree mail screen drawn by libux, and a click" {
    var s = try harness.Session.start(.{ .name = "uxkit-mail", .rio_args = &.{"-u"} });
    defer s.stop();
    try s.openWindow(rect(40, 40, 900, 600), "rc /mnt/term/cwd/test/uxkit/uxkit.rc");
    gpa.free(try s.waitWindows(1));
    s.waitRc("test -e /srv/uitree.$user", 15_000) catch |e| {
        s.saveScreen("uxkit-fail");
        s.rcQuiet("cat $work/uxkit.err") catch {};
        return e;
    };
    _ = try s.rc("mkdir -p /n/ui && mount /srv/uitree.$user /n/ui");
    // demo.rc's last step: its event log exists once the tree is built
    try s.waitRc("test -e $work/uxkit.events", 15_000);
    try s.settle();
    s.saveScreen("uxkit-mail");

    // the sidebar keeps w=176; the cards stack in main, m1 above m2
    const side = try bounds(s, "side");
    try std.testing.expectEqual(@as(i32, 176), side[2]);
    const m1 = try bounds(s, "main/m1");
    const m2 = try bounds(s, "main/m2");
    try std.testing.expect(m2[1] > m1[1] + m1[3] - 1);
    try std.testing.expect(m1[0] >= side[0] + side[2]);

    // a click on the second card's subject line is the card's click.
    // Bounds are window-local; the window's inside is 1 px in and 4
    // down from rio's rectangle (the title bar sits above it).
    const ids = try s.windows();
    defer gpa.free(ids);
    const wr = try s.windowRect(ids[0]);
    const sub = try bounds(s, "main/m2/subj");
    const at = harness.pt(wr.min.x + 1 + sub[0] + 10, wr.min.y + 4 + sub[1] + @divTrunc(sub[3], 2));
    s.click(at, .left);
    s.waitRc("grep -s '^click open:m2 main/m2 ' $work/uxkit.events", 5_000) catch |e| {
        const dbg = try s.rc("cat $work/uxkit.events; cat $work/uxkit.err");
        defer gpa.free(dbg);
        std.debug.print("at {d},{d} win {f}\n{s}\n", .{ at.x, at.y, wr, dbg });
        return e;
    };
    // demo.rc answers by selecting the card
    try s.waitRc("grep -s 'bg=accent' /n/ui/win/main/main/m2/style", 5_000);
    try s.settle();
    s.saveScreen("uxkit-mail-clicked");
}
