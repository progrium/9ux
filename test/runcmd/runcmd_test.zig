//! Run Command (apps/runcmd): rc in a libux console, in 9/UX mode.
//! Typing a command runs it and shows its output; output past the
//! window grows the scroll view and keeps the end in view; Delete
//! interrupts; ^D at an empty line ends rc, and the window with it.
//! Build libux, resfs and apps/runcmd first (`mk` at the top does).
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const pt = harness.pt;
const Key = harness.Key;
const gpa = std.testing.allocator;

const start = "rc /mnt/term/cwd/test/runcmd/runcmd.rc";
const sv = "/tmp/rc/win/main/scroll";
const t = sv ++ "/text";

fn waitValue(s: *harness.Session, re: []const u8) !void {
    const cmd = try std.fmt.allocPrint(gpa, "grep -s '{s}' {s}/value", .{ re, t });
    defer gpa.free(cmd);
    s.waitRc(cmd, 15_000) catch |e| {
        s.saveScreen("runcmd-fail");
        s.rcQuiet("cat " ++ t ++ "/value; cat $work/runcmd.err") catch {};
        return e;
    };
}

test "runcmd: rc in a console with a scroll bar" {
    var s = try harness.Session.start(.{ .name = "runcmd", .rio_args = &.{"-u"} });
    defer s.stop();
    const win = rect(60, 60, 620, 420);
    try s.openWindow(win, start);
    gpa.free(try s.waitWindows(1));
    try s.waitRc("test -e /srv/ux.$user.runcmd", 15_000);
    _ = try s.rc("mkdir -p /tmp/rc && mount /srv/ux.$user.runcmd /tmp/rc");
    const ty = try s.rc("cat " ++ t ++ "/type");
    defer gpa.free(ty);
    try std.testing.expectEqualStrings("console\n", ty);

    // rc's prompt, then a command typed into the window
    try waitValue(s, "^% ");
    s.click(pt(300, 200), .left);
    harness.sleepMs(200);
    try s.typeText("echo hello, world\n");
    try waitValue(s, "^hello, world$");

    // the shell's console is ours: /dev/cons reads what's typed
    try s.typeText("read </dev/cons >/tmp/line; echo got `{cat /tmp/line}\n");
    harness.sleepMs(300);
    try s.typeText("typed line\n");
    try waitValue(s, "^got typed line$");

    // more output than fits: the console grows and the end stays in view
    try s.typeText("seq 1 80\n");
    try waitValue(s, "^80$");
    try s.waitRc("~ `{awk '{print $4}' " ++ t ++ "/bounds} [0-9][0-9][0-9][0-9]*", 10_000);
    try s.waitRc("sed 1q " ++ sv ++ "/tree | grep -s 'origin=[1-9]'", 10_000);
    try s.settle();
    s.saveScreen("runcmd-scrolled");

    // Delete interrupts what's running, and rc prompts again
    _ = try s.rc("echo clear >" ++ t ++ "/ctl");
    try s.typeText("sleep 1000; echo not interrupted\n");
    try s.waitRc("ps | grep -s ' sleep$'", 10_000);
    try s.key(Key.del);
    try s.waitRc("! ps | grep -s ' sleep$'", 10_000);
    try s.typeText("echo still here\n");
    try waitValue(s, "^still here$");
    try s.waitRc("! grep -s '^not interrupted$' " ++ t ++ "/value", 1_000);

    // the Shell menu's Interrupt does the same
    try s.typeText("sleep 1000\n");
    try s.waitRc("ps | grep -s ' sleep$'", 10_000);
    _ = try s.rc("echo menu 'item=interrupt' >/tmp/rc/debug/inject");
    try s.waitRc("! ps | grep -s ' sleep$'", 10_000);

    // raw mode (rawon on consctl): keys go to the reader as typed, unshown
    try s.typeText("{echo rawon; read -c 3 </dev/cons >/tmp/k} >/dev/consctl; echo raw `{cat /tmp/k}\n");
    harness.sleepMs(500);
    try s.typeText("xyz");
    try waitValue(s, "^raw xyz$");
    try s.waitRc("! grep -s '^xyz' " ++ t ++ "/value", 1_000);

    // the grow box resizes the window, and the text fits the new width
    const before = try s.rc("cat " ++ t ++ "/bounds");
    defer gpa.free(before);
    s.drag(pt(win.max.x - 8, win.max.y - 8), pt(win.max.x + 92, win.max.y + 42), .left);
    try s.waitRc("~ `{awk '{print $1+$3}' " ++ sv ++ "/bounds} 6[5-9][0-9]", 10_000);
    harness.sleepMs(300);
    const after = try s.rc("cat " ++ t ++ "/bounds");
    defer gpa.free(after);
    try std.testing.expect(!std.mem.eql(u8, before, after));

    // a screen for people: a little session, in the proportional body font
    _ = try s.rc("echo clear >" ++ t ++ "/ctl");
    try s.typeText("cd /sys/src/cmd/rc; ls -l | sed 3q\n");
    try waitValue(s, " exec.c$");
    try s.typeText("lc /sys/src/cmd/rc\n");
    try s.typeText("wc -l *.c | sort -nr | sed 4q\n");
    try waitValue(s, "total$");
    try s.typeText("echo Hello from Run Command\n");
    try waitValue(s, "^Hello from Run Command$");
    try s.settle();
    s.saveScreen("runcmd");

    // its Shell menu, held open in the menu bar (released over the title, so nothing's chosen)
    s.move(pt(248, 9));
    s.press(.left);
    try s.settle();
    s.saveScreen("runcmd-menu");
    s.release(.left);
    try s.settle();

    // ^D at an empty line is end of file: rc exits, and the window goes
    try s.key(0x04);
    try s.waitRc("! test -e /srv/ux.$user.runcmd || ! ps | grep -s ' 6.out$| 7.out$'", 15_000);
}
