//! Making windows: the `window` command (a wctl "new"), `window -m`
//! (a /mnt/wsys attach with a "new" spec), and the button-3 menu's New;
//! and the files each window gets.
const std = @import("std");
const harness = @import("harness");
const c = @import("common.zig");
const gpa = c.gpa;
const rect = harness.rect;
const pt = harness.pt;

test "window through wctl new runs rc in a window at the given rectangle" {
    var s = try harness.Session.start(.{ .name = "win-wctl" });
    defer s.stop();
    try s.rcQuiet("window -r 100 100 500 400 rc");
    const id = try c.onlyWindow(s);
    try c.waitRect(s, id, rect(100, 100, 500, 400));
    // Its rc reads what's typed: the new window is current.
    try c.waitState(s, id, "current");
    const out = try s.workPath("typed");
    defer gpa.free(out);
    const line = try std.fmt.allocPrint(gpa, "echo typed >{s}.tmp && mv {s}.tmp {s}\n", .{ out, out, out });
    defer gpa.free(line);
    harness.sleepMs(300);
    try s.typeText(line);
    const got = try c.workTrimmed(s, "typed");
    defer gpa.free(got);
    try std.testing.expectEqualStrings("typed", got);
}

test "window -m attaches /mnt/wsys with a new spec, and the window gets winid, winname and label" {
    var s = try harness.Session.start(.{ .name = "win-attach" });
    defer s.stop();
    try s.openWindow(rect(200, 150, 700, 450),
        \\{echo `{cat /dev/winid}; cat /dev/winname; echo; cat /dev/label; echo; ls /dev} >$work/info.tmp && mv $work/info.tmp $work/info; sleep 1000
    );
    const id = try c.onlyWindow(s);
    try c.waitRect(s, id, rect(200, 150, 700, 450));
    const info = try c.workTrimmed(s, "info");
    defer gpa.free(info);
    var lines = std.mem.splitScalar(u8, info, '\n');
    const winid = lines.next().?;
    try std.testing.expectEqual(id, try std.fmt.parseInt(u32, std.mem.trim(u8, winid, " "), 10));
    // winname names the window's image, window.ID.N.
    const winname = lines.next().?;
    try std.testing.expect(std.mem.startsWith(u8, winname, try std.fmt.bufPrint(@constCast(&[_]u8{0} ** 32), "window.{d}.", .{id})));
    // window -x labels the window with the command's name.
    try std.testing.expectEqualStrings("rc", lines.next().?);
    // The window's own files are bound over /dev.
    var want = std.StringHashMap(void).init(gpa);
    defer want.deinit();
    while (lines.next()) |l| try want.put(std.fs.path.basename(l), {});
    for ([_][]const u8{ "cons", "consctl", "cursor", "kbd", "label", "mouse", "screen", "snarf", "text", "wctl", "wdir", "window", "winid", "winname", "wsys" }) |f| {
        if (!want.contains(f)) {
            std.debug.print("/dev has no {s}\n", .{f});
            return error.MissingFile;
        }
    }
}

test "button 3 New sweeps out a window running rc" {
    var s = try harness.Session.start(.{ .name = "win-menu-new" });
    defer s.stop();
    // New is the first item; the menu opens with it under the mouse.
    s.menuPick(pt(600, 600), .right, 0, 0);
    s.drag(pt(50, 60), pt(400, 300), .right);
    const id = try c.onlyWindow(s);
    const r = try s.windowRect(id);
    // sweep() includes the pixel under the release point.
    try std.testing.expectEqual(rect(50, 60, 401, 301), r);
    try c.waitState(s, id, "current");
    harness.sleepMs(300);
    const out = try s.workPath("label");
    defer gpa.free(out);
    const line = try std.fmt.allocPrint(gpa, "cat /dev/label >{s}.tmp && mv {s}.tmp {s}\n", .{ out, out, out });
    defer gpa.free(line);
    try s.typeText(line);
    const label = try c.workTrimmed(s, "label");
    defer gpa.free(label);
    try std.testing.expect(std.mem.startsWith(u8, label, "rc"));
}

test "a window's label, text and wdir files read back through /dev/wsys" {
    var s = try harness.Session.start(.{ .name = "win-files" });
    defer s.stop();
    try s.openWindow(rect(100, 100, 600, 400), "echo hello from the window; sleep 1000");
    const id = try c.onlyWindow(s);
    var p: [64]u8 = undefined;
    try s.writeFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/label", .{id}), "relabeled");
    const label = try s.readFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/label", .{id}));
    defer gpa.free(label);
    try std.testing.expectEqualStrings("relabeled", label);
    try s.waitRc(try std.fmt.bufPrint(&p, "grep -s 'hello from' /dev/wsys/{d}/text", .{id}), 10_000);
    // wdir starts as the directory the window was made in (window -m
    // passes -cd `{pwd}), and takes writes.
    const wdir = try s.readFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/wdir", .{id}));
    defer gpa.free(wdir);
    try std.testing.expectEqualStrings("/mnt/term/cwd", wdir);
    try s.writeFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/wdir", .{id}), "/tmp");
    const wdir2 = try s.readFile(try std.fmt.bufPrint(&p, "/dev/wsys/{d}/wdir", .{id}));
    defer gpa.free(wdir2);
    try std.testing.expectEqualStrings("/tmp", wdir2);
}
