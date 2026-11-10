//! rio starts on the headless screen, and a window opens.
const std = @import("std");
const harness = @import("harness");

test "rio starts and opens a window" {
    var s = try harness.Session.start(.{ .name = "boot" });
    defer s.stop();
    const none = try s.windows();
    defer std.testing.allocator.free(none);
    try std.testing.expectEqual(0, none.len);
    try s.rcQuiet("window -r 100 100 500 400 rc");
    const ids = try s.waitWindows(1);
    defer std.testing.allocator.free(ids);
    const r = try s.windowRect(ids[0]);
    try std.testing.expectEqual(harness.rect(100, 100, 500, 400), r);
}
