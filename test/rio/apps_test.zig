//! Programs that run inside rio's windows: a nested rio, acme and sam.
const std = @import("std");
const harness = @import("harness");
const c = @import("common.zig");
const gpa = c.gpa;
const rect = harness.rect;

const Count = struct {
    r: harness.Rect,
    color: u32,
    min: usize,
    fn ok(self: Count, img: harness.Image) bool {
        return img.count(self.r, self.color) >= self.min;
    }
};

test "rio runs nested in a window, with its own windows" {
    var s = try harness.Session.start(.{ .name = "app-rio" });
    defer s.stop();
    try s.rcQuiet(
        \\{
        \\    echo '#!/bin/rc'
        \\    echo 'window -m -r 20 20 300 200 rc -c ''cat /dev/winid >$work/inner.tmp; mv $work/inner.tmp $work/inner; sleep 1000'''
        \\} >$work/nested.rc; chmod +x $work/nested.rc
    );
    const r = rect(100, 100, 800, 600);
    try s.openWindow(r, "rio -i $work/nested.rc");
    const outer = try c.onlyWindow(s);
    // The inner rio's first window is its window 1, and the outer rio
    // still has only the one window.
    const inner = try c.workTrimmed(s, "inner");
    defer gpa.free(inner);
    try std.testing.expectEqualStrings("1", inner);
    const ids = try s.windows();
    defer gpa.free(ids);
    try std.testing.expectEqualSlices(u32, &.{outer}, ids);
    // The inner rio paints its background inside the outer window.
    try c.waitScreen(s, Count{ .r = r.inset(c.border), .color = c.color.background, .min = 100_000 }, Count.ok);
}

test "acme starts in a window" {
    var s = try harness.Session.start(.{ .name = "app-acme" });
    defer s.stop();
    const r = rect(50, 50, 650, 550);
    try s.openWindow(r, "acme");
    gpa.free(try s.waitWindows(1));
    try c.waitScreen(s, Count{ .r = r, .color = c.color.acme_body, .min = 20_000 }, Count.ok);
}

test "sam starts in a window" {
    var s = try harness.Session.start(.{ .name = "app-sam" });
    defer s.stop();
    const r = rect(50, 50, 650, 550);
    try s.openWindow(r, "sam");
    gpa.free(try s.waitWindows(1));
    try c.waitScreen(s, Count{ .r = r, .color = c.color.sam_cmd, .min = 10_000 }, Count.ok);
}
