//! The light and dark themes (assets/README.md, Themes): rio's
//! `theme dark` redraws its menu bar and window chrome and tells the
//! apps, a libux app follows it as uitree's system appearance, and an
//! app that chose its own mode (theme/pref) keeps it. Build rio, libux,
//! resfs and apps/sample first: `scripts/test.sh build` does.
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const pt = harness.pt;
const gpa = std.testing.allocator;

const start = "rc /mnt/term/cwd/test/libux/sample.rc";
const win = rect(20, 40, 520, 460);
/// the window with its chrome: the title bar above it, the shadow
const framed = rect(18, 22, 524, 464);

/// The menu bar's colour, between its titles and the clock: the
/// window role (assets/palette.ndb), white in light and 292929 in dark.
fn barColor(s: *harness.Session) !u32 {
    const img = try s.screenshot();
    defer img.deinit(gpa);
    return img.at(600, 6);
}

fn waitMode(s: *harness.Session, comptime want: []const u8) !void {
    try s.waitRc("grep -s '^" ++ want ++ "$' /tmp/sample/theme/mode", 10_000);
    try s.settle();
}

test "libux: the dark theme, from rio and per app" {
    var s = try harness.Session.start(.{ .name = "libux-theme", .rio_args = &.{"-u"} });
    defer s.stop();
    gpa.free(try s.rc("echo clock at Tue 09:41 >/dev/ux/ctl"));
    try s.openWindow(win, start);
    gpa.free(try s.waitWindows(1));
    try s.waitRc("test -e /srv/ux.$user.sample", 10_000);
    gpa.free(try s.rc("mkdir -p /tmp/sample && mount /srv/ux.$user.sample /tmp/sample"));

    // light to begin with: rio says so, and so does the app
    const v = try s.rc("cat /dev/ux/version /dev/ux/ctl");
    defer gpa.free(v);
    try std.testing.expect(std.mem.indexOf(u8, v, "feature theme\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, v, "theme light\n") != null);
    try waitMode(s, "light");
    try std.testing.expectEqual(@as(u32, 0xFFFFFF), try barColor(s));

    // rio goes dark, and the app follows
    gpa.free(try s.rc("echo theme dark >/dev/ux/ctl"));
    try waitMode(s, "dark");
    try std.testing.expectEqual(@as(u32, 0x292929), try barColor(s));
    const c = try s.rc("cat /dev/ux/ctl");
    defer gpa.free(c);
    try std.testing.expect(std.mem.indexOf(u8, c, "theme dark\n") != null);
    s.saveScreen("theme-dark");
    try s.expectGolden("test/libux/golden/sample-dark.png", .{ .rect = framed });

    // an app that chose light keeps it, in dark chrome
    gpa.free(try s.rc("echo light >/tmp/sample/theme/pref"));
    try waitMode(s, "light");
    try s.expectGolden("test/libux/golden/sample-light-in-dark.png", .{ .rect = framed });

    // back to the system's: dark again, then light with rio
    gpa.free(try s.rc("echo system >/tmp/sample/theme/pref"));
    try waitMode(s, "dark");
    try s.expectGolden("test/libux/golden/sample-dark.png", .{ .rect = framed });
    gpa.free(try s.rc("echo theme light >/dev/ux/ctl"));
    try waitMode(s, "light");
    try std.testing.expectEqual(@as(u32, 0xFFFFFF), try barColor(s));
    s.saveScreen("theme-light");
}
