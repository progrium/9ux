//! The desktop mockup (assets/mockups/desktop.png), drawn for real:
//! Finder with the mockup's volumes, the plan9 folder in icon view over
//! the sources folder in list view, an rc window, and the menu bar with
//! its clock. The golden is the real thing to hold against the mockup;
//! rio, libux and Finder should keep drawing it. Build rio, libux,
//! resfs, uxmeta and finder first (scripts/test.sh builds everything).
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const gpa = std.testing.allocator;

fn waitTree(s: *harness.Session, cmd: []const u8) !void {
    s.waitRc(cmd, 15_000) catch |e| {
        s.saveScreen("mockup-fail");
        return e;
    };
}

test "finder: the desktop mockup" {
    var s = try harness.Session.start(.{ .name = "mockup", .rio_args = &.{"-u"} });
    defer s.stop();
    gpa.free(try s.rc("echo clock at 'Tue 09:41' >/dev/ux/ctl"));

    // a legacy rc window, as rio draws it, where the mockup has one
    try s.openWindow(rect(24, 380, 424, 570), "cat /mnt/term/cwd/test/finder/mockup.txt; sleep 1000000");

    try s.openWindow(rect(100, 100, 600, 400), "rc /mnt/term/cwd/test/finder/mockup.rc");
    try waitTree(s, "test -e /srv/ux.$user.finder");
    s.rcQuiet("mkdir -p /tmp/f && mount /srv/ux.$user.finder /tmp/f") catch {};
    try waitTree(s, "! grep -s '^win=0 ' /dev/ux/desktop/state");

    // both folder windows open, plan9 in front with README selected,
    // and the sources list with its README row selected
    const icons = "`{for(w in /tmp/f/windows/*) if(grep -s 'label=Notes ' $w/controls/icons/items) echo $w}";
    const list = "`{for(w in /tmp/f/windows/*) if(test -e $w/controls/list) echo $w}";
    try waitTree(s, "~ " ++ icons ++ " /tmp/*");
    try waitTree(s, "~ " ++ list ++ " /tmp/*");
    try waitTree(s, "grep -s '^README' " ++ list ++ "^/controls/list/items");
    gpa.free(try s.rc("w=" ++ list ++ "; echo select `{grep -n '^README' $w/controls/list/items | sed 's/:.*//; s/$/-1/' | hoc} >$w/controls/list/ctl"));
    gpa.free(try s.rc("w=" ++ icons ++ "; echo front >$w/ctl; echo select `{grep 'label=README ' $w/controls/icons/items | awk '{print $2}'} >$w/controls/icons/ctl"));
    try waitTree(s, "w=" ++ icons ++ "; grep -s '^i[0-9]+$' $w/controls/icons/selection");
    try s.settle();
    s.saveScreen("finder-mockup");
    try s.expectGolden("test/finder/golden/desktop.png", .{});
}
