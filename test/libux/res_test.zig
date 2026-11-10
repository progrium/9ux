//! libux drawing what the sample app's resources (apps/sample/sample.res,
//! through resfs) describe: icons in an icon view from the app's and the
//! system's ICN#/icl8 families, and an alert built from ALRT/DITL with a
//! DITL icon. Build libux, resfs and apps/sample first:
//! `scripts/h9.sh --rc 'cd libux && mk test'` does all three.
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const gpa = std.testing.allocator;

const start = "rc /mnt/term/cwd/test/libux/sample.rc";

test "libux: icons and an alert from resources" {
    var s = try harness.Session.start(.{ .name = "libux-res" });
    defer s.stop();
    try s.openWindow(rect(20, 20, 520, 440), start);
    gpa.free(try s.waitWindows(1));
    try s.waitRc("test -e /srv/ux.$user.sample", 10_000);
    _ = try s.rc("mkdir -p /tmp/sample && mount /srv/ux.$user.sample /tmp/sample");
    // a selected icon is drawn darkened through its mask
    _ = try s.rc("echo select sample >/tmp/sample/windows/1/controls/icons/ctl");
    try s.expectGolden("test/libux/golden/sample.png", .{ .rect = rect(20, 20, 520, 440) });

    // About Sample…: ALRT 129, with the system's note icon (ID 1)
    _ = try s.rc("echo invoke about-sample >/tmp/sample/menus/file/ctl");
    const ids = try s.waitWindows(2);
    gpa.free(ids);
    try s.waitRc("test -e /tmp/sample/windows/2/controls/item3", 10_000);
    try s.expectGolden("test/libux/golden/alert.png", .{ .rect = rect(150, 150, 490, 290) });
}
