//! Run Command as an xterm (libux/vt.c through uxconsvt): rc sees
//! TERM=xterm-256color and the size; escape sequences colour and edit
//! the text in place instead of showing; raw keys go as xterm sends
//! them; a full-screen program gets the alternate screen, drawn in the
//! fixed-width font, and leaves rc's text as it was.
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

fn waitRc(s: *harness.Session, cmd: []const u8) !void {
    s.waitRc(cmd, 15_000) catch |e| {
        s.saveScreen("runcmd-xterm-fail");
        s.rcQuiet("cat " ++ t ++ "/value; cat " ++ t ++ "/screen; cat $work/runcmd.err") catch {};
        return e;
    };
}

fn waitValue(s: *harness.Session, re: []const u8) !void {
    const cmd = try std.fmt.allocPrint(gpa, "grep -s '{s}' {s}/value", .{ re, t });
    defer gpa.free(cmd);
    try waitRc(s, cmd);
}

test "runcmd: an xterm for the programs that want one" {
    var s = try harness.Session.start(.{ .name = "runcmd-xterm", .rio_args = &.{"-u"} });
    defer s.stop();
    const win = rect(40, 40, 720, 540);
    try s.openWindow(win, start);
    gpa.free(try s.waitWindows(1));
    try s.waitRc("test -e /srv/ux.$user.runcmd", 15_000);
    _ = try s.rc("mkdir -p /tmp/rc && mount /srv/ux.$user.runcmd /tmp/rc");
    const term = try s.rc("cat " ++ t ++ "/term");
    defer gpa.free(term);
    try std.testing.expectEqualStrings("xterm-256color\n", term);
    try waitValue(s, "^% ");
    s.click(pt(300, 200), .left);
    harness.sleepMs(200);

    // rc's environment says what the terminal is, and its size, as vt(1)'s does
    try s.typeText("echo term $TERM $LINES $COLS $WINCH\n");
    try waitValue(s, "^term xterm-256color [1-9][0-9]* [1-9][0-9]* 1$");

    // colour: the sequences act, and only the text stays
    try s.typeText("awk 'BEGIN { printf \"\\033[1;31mred\\033[0m and \\033[38;5;27mblue\\033[0m\\n\" }'\n");
    try waitValue(s, "^red and blue$");

    // carriage return and erase-line rewrite a line in place, as progress meters do
    try s.typeText("awk 'BEGIN { printf \"copying 10%%\\rcopying 100%%\\033[K\\n\" }'\n");
    try waitValue(s, "^copying 100%$");
    try waitRc(s, "! grep -s '^copying 10%' " ++ t ++ "/value");

    // raw keys go as xterm sends them: the up arrow is ESC [ A
    try s.typeText("{echo rawon; read -c 3 </dev/cons >/tmp/k} >/dev/consctl; xd -1x /tmp/k\n");
    harness.sleepMs(500);
    try s.key(Key.up);
    try waitValue(s, " 1b 5b 41$");

    // clear(1)'s sequences empty the window
    try s.typeText("awk 'BEGIN { printf \"\\033[H\\033[2J\\033[3J\" }'\n");
    try waitRc(s, "~ `{cat " ++ t ++ "/value} '%'");

    // a full-screen program: the alternate screen, in the fixed-width font
    try s.typeText("echo before; rc /mnt/term/cwd/test/runcmd/xterm.rc; echo after\n");
    try waitRc(s, "grep -s 'xterm-256color' " ++ t ++ "/screen");
    try waitRc(s, "grep -s '│ line drawing ┌─┬─┐' " ++ t ++ "/screen");
    try waitRc(s, "grep -s 'cpu3 \\[[|][|]' " ++ t ++ "/screen");
    try waitRc(s, "! grep -s 'cpu3' " ++ t ++ "/value");
    try s.settle();
    s.saveScreen("runcmd-xterm");
    try s.typeText("q");
    try waitValue(s, "^after$");
    try waitRc(s, "! grep -s cpu " ++ t ++ "/screen");
    try waitRc(s, "~ `{cat /tmp/xtermkey} q");
    try waitValue(s, "^before$");

    // a resize is counted in /env/WINCH, with the new size (rc's own
    // variables are what it read when it started, as under vt(1))
    s.drag(pt(win.max.x - 8, win.max.y - 8), pt(win.max.x + 92, win.max.y + 42), .left);
    try waitRc(s, "~ `{awk '{print $1+$3}' " ++ sv ++ "/bounds} 7[5-9][0-9]");
    harness.sleepMs(300);
    try s.typeText("echo winch `{cat /env/WINCH} `{cat /env/COLS}\n");
    try waitValue(s, "^winch [2-9][0-9]* [1-9][0-9]*$");

    // a screen for people: rc's text in the body font, with a program's colours
    _ = try s.rc("echo clear >" ++ t ++ "/ctl");
    try s.typeText("cat /mnt/term/cwd/test/runcmd/xtermls.txt\n");
    try waitValue(s, "README.md");
    try s.typeText("echo Hello from Run Command\n");
    try waitValue(s, "^Hello from Run Command$");
    try s.settle();
    s.saveScreen("runcmd-xterm-colour");
}
