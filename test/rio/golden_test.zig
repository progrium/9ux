//! What stock rio draws, pixel for pixel: window borders (current and
//! not), text in a window, and the button 3 menu. Golden images are in
//! golden/; `scripts/test.sh gui -- -Dupdate-golden` rewrites them.
const std = @import("std");
const harness = @import("harness");
const c = @import("common.zig");
const gpa = c.gpa;
const rect = harness.rect;
const pt = harness.pt;

test "golden: two windows with text, one current" {
    var s = try harness.Session.start(.{ .name = "golden-windows" });
    defer s.stop();
    try s.openWindow(rect(40, 40, 440, 240), "echo the quick brown fox; echo jumps over the lazy dog; sleep 1000");
    gpa.free(try s.waitWindows(1));
    try s.openWindow(rect(300, 160, 760, 420), "echo ÄÖÜ äöü ß £ ©; sleep 1000");
    const ids = try s.waitWindows(2);
    defer gpa.free(ids);
    var p: [64]u8 = undefined;
    try s.waitRc(try std.fmt.bufPrint(&p, "grep -s fox /dev/wsys/{d}/text", .{ids[0]}), 10_000);
    try s.waitRc(try std.fmt.bufPrint(&p, "grep -s äöü /dev/wsys/{d}/text", .{ids[1]}), 10_000);
    try c.waitState(s, ids[1], "current");
    try s.expectGolden("test/rio/golden/windows.png", .{ .rect = rect(0, 0, 800, 460) });
}

test "golden: the button 3 menu" {
    var s = try harness.Session.start(.{ .name = "golden-menu" });
    defer s.stop();
    const at = pt(500, 400);
    s.move(at);
    s.press(.right);
    defer {
        // Release away from the menu, which picks nothing.
        s.move(pt(1000, 750));
        s.release(.right);
    }
    try s.expectGolden("test/rio/golden/menu3.png", .{ .rect = rect(400, 340, 600, 520) });
}
