//! rio's drag and drop broker, /dev/ux/drag (docs/protocols/devux.md,
//! "Drag and drop"): a drag following the pointer from window to
//! window, accepting, dropping, the payload carried by rio both when
//! the source put it up front and when it answers fetches, and drags
//! that end without a drop.
//!
//! Sources are dragsrc.rc in a window, or rc in the agent naming its
//! window with win=; targets are played by the test writing drag/ctl.
const std = @import("std");
const harness = @import("harness");
const gpa = std.testing.allocator;
const rect = harness.rect;
const pt = harness.pt;
const Session = harness.Session;

fn start(name: []const u8) !*Session {
    return Session.start(.{ .name = name, .rio_args = &.{"-u"} });
}

/// Waits until work file `name` contains `needle`.
fn waitFor(s: *Session, name: []const u8, needle: []const u8) !void {
    var waited: u64 = 0;
    while (true) : (waited += 50) {
        if (try s.readWork(name)) |d| {
            defer gpa.free(d);
            if (std.mem.indexOf(u8, d, needle) != null) return;
            if (waited >= 10_000) {
                std.debug.print("{s} has no '{s}':\n{s}\n", .{ name, needle, d });
                s.saveScreen(name);
                return error.WaitTimeout;
            }
        } else if (waited >= 10_000) {
            std.debug.print("no {s}\n", .{name});
            return error.WaitTimeout;
        }
        harness.sleepMs(50);
    }
}

fn workHas(s: *Session, name: []const u8, needle: []const u8) !bool {
    const d = (try s.readWork(name)) orelse return false;
    defer gpa.free(d);
    return std.mem.indexOf(u8, d, needle) != null;
}

/// Moves the pointer to `to` in steps, as a hand would.
fn glide(s: *Session, from: harness.Point, to: harness.Point) void {
    const steps = 6;
    for (1..steps + 1) |i| {
        const k: i32 = @intCast(i);
        s.move(.{
            .x = from.x + @divTrunc((to.x - from.x) * k, steps),
            .y = from.y + @divTrunc((to.y - from.y) * k, steps),
        });
    }
}

fn rcf(s: *Session, comptime fmt: []const u8, args: anytype) !void {
    const cmd = try std.fmt.allocPrint(gpa, fmt, args);
    defer gpa.free(cmd);
    try s.rcQuiet(cmd);
}

fn rcOut(s: *Session, comptime fmt: []const u8, args: anytype) ![]u8 {
    const cmd = try std.fmt.allocPrint(gpa, fmt, args);
    defer gpa.free(cmd);
    return s.rc(cmd);
}

test "version has drag" {
    var s = try start("drag-version");
    defer s.stop();
    const v = try s.readFile("/dev/ux/version");
    defer gpa.free(v);
    try std.testing.expect(std.mem.startsWith(u8, v, "devux 1."));
    try std.testing.expect(std.mem.indexOf(u8, v, "\nfeature drag\n") != null);
    const h = try s.readFile("/dev/ux/drag/ctl");
    defer gpa.free(h);
    try std.testing.expect(std.mem.indexOf(u8, h, "start -\n") != null);
}

test "a drag needs a button down" {
    var s = try start("drag-nobutton");
    defer s.stop();
    try s.openWindow(rect(100, 100, 400, 400), "sleep 1000");
    const w = try s.waitWindows(1);
    defer gpa.free(w);
    const cmd = try std.fmt.allocPrint(gpa, "echo start 'win={d}' 'types=text' >/dev/ux/drag/ctl", .{w[0]});
    defer gpa.free(cmd);
    const r = try s.exec(cmd, 10_000);
    defer r.deinit(gpa);
    try std.testing.expect(!r.ok());
    try std.testing.expect(std.mem.indexOf(u8, r.out, "no button down") != null);
}

test "text put up front is dropped on a window that accepts it" {
    var s = try start("drag-put");
    defer s.stop();
    try s.rcQuiet("cat /dev/ux/drag/events >$work/drag &");
    try s.openWindow(rect(100, 100, 400, 400), "sleep 1000");
    try s.openWindow(rect(500, 100, 900, 400), "sleep 1000");
    const w = try s.waitWindows(2);
    defer gpa.free(w);
    const a, const b = .{ w[0], w[1] };

    // Make a current, then press in it, as a person starting a drag would.
    s.click(pt(250, 250), .left);
    harness.sleepMs(200);
    s.move(pt(250, 250));
    s.press(.left);
    harness.sleepMs(100);
    // The source session: start, put the text, and stay open until told.
    try rcf(s,
        \\{{ echo start 'win={d}' 'types=text,x.test' 'ops=copy,move' '''label=Some text''' >[1=3] && {{
        \\    {{ echo put 1 text >[1=5]; echo -n 'hello, drop' >[1=5] }} >[5]/dev/ux/drag/data
        \\    echo ok >$work/put
        \\    while(! test -e $work/stop) sleep 0.1
        \\}} }} >[3]/dev/ux/drag/ctl >[2]$work/src.err &
    , .{a});
    gpa.free(try s.waitWork("put", 10_000));
    try waitFor(s, "drag", "start id=1 win=");
    try waitFor(s, "drag", "label=Some text");

    glide(s, pt(250, 250), pt(700, 250));
    var buf: [128]u8 = undefined;
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "leave id=1 win={d}", .{a}));
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "enter id=1 win={d} ", .{b}));
    try rcf(s, "echo accept 1 'win={d}' 'op=move' >/dev/ux/drag/ctl", .{b});
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "accept id=1 win={d} type=text op=move", .{b}));
    const st = try s.readFile("/dev/ux/drag/state");
    defer gpa.free(st);
    try std.testing.expect(std.mem.indexOf(u8, st, try std.fmt.bufPrint(&buf, "state=over target={d} type=text op=move", .{b})) != null);

    s.release(.left);
    // b's drawable area starts Borderwidth (4) inside its rectangle.
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "drop id=1 win={d} pt=196,146 type=text op=move", .{b}));
    const got = try s.rc("{ echo fetch 1 text >[1=5]; cat <[0=5] } <>[5]/dev/ux/drag/data");
    defer gpa.free(got);
    try std.testing.expectEqualStrings("hello, drop", got);
    try s.rcQuiet("echo done 1 >/dev/ux/drag/ctl");
    try waitFor(s, "drag", "end id=1");
    try std.testing.expect(try workHas(s, "drag", try std.fmt.bufPrint(&buf, "done id=1 win={d} op=move", .{b})));
    try std.testing.expect(!try workHas(s, "drag", "fetch id=1"));
    try s.rcQuiet("touch $work/stop");
}

test "a source in its own window answers fetches through rio" {
    var s = try start("drag-fetch");
    defer s.stop();
    try s.rcQuiet("cat /dev/ux/drag/events >$work/drag &");
    try s.openWindow(rect(100, 100, 400, 400), "rc /mnt/term/cwd/test/devux/dragsrc.rc src file");
    try s.openWindow(rect(500, 100, 900, 400), "sleep 1000");
    const w = try s.waitWindows(2);
    defer gpa.free(w);
    const a, const b = .{ w[0], w[1] };

    s.click(pt(250, 250), .left);
    harness.sleepMs(200);
    s.move(pt(250, 250));
    s.press(.left);
    harness.sleepMs(100);
    try s.rcQuiet("touch $work/src.go");
    const id = try s.waitWork("src.id", 10_000);
    defer gpa.free(id);
    const n = std.mem.trim(u8, id, " \n");
    var buf: [128]u8 = undefined;
    // With no win=, the drag comes from the source's own window.
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "start id={s} win={d} types=file", .{ n, a }));

    glide(s, pt(250, 250), pt(600, 300));
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "enter id={s} win={d} ", .{ n, b }));
    try rcf(s, "echo accept {s} 'win={d}' >/dev/ux/drag/ctl", .{ n, b });
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "accept id={s} win={d} type=file op=copy", .{ n, b }));
    s.release(.left);
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "drop id={s} win={d} pt=96,196 type=file op=copy", .{ n, b }));

    const list = try rcOut(s, "{{ echo fetch {s} file >[1=5]; cat <[0=5] }} <>[5]/dev/ux/drag/data", .{n});
    defer gpa.free(list);
    try std.testing.expectEqualStrings("lazy src file -", list);
    const item = try rcOut(s, "{{ echo fetch {s} file 'item=2' >[1=5]; cat <[0=5] }} <>[5]/dev/ux/drag/data", .{n});
    defer gpa.free(item);
    try std.testing.expectEqualStrings("lazy src file 2", item);
    try rcf(s, "echo done {s} >/dev/ux/drag/ctl", .{n});
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "end id={s}", .{n}));
    try s.rcQuiet("touch $work/src.stop");
}

test "a drop nobody accepted cancels, and the menu bar is no target" {
    var s = try start("drag-cancel");
    defer s.stop();
    try s.rcQuiet("cat /dev/ux/drag/events >$work/drag &");
    try s.openWindow(rect(100, 100, 400, 400), "sleep 1000");
    const w = try s.waitWindows(1);
    defer gpa.free(w);
    const a = w[0];

    s.click(pt(250, 250), .left);
    harness.sleepMs(200);
    s.move(pt(250, 250));
    s.press(.left);
    harness.sleepMs(100);
    try rcf(s,
        \\{{ echo start 'win={d}' 'types=text' >[1=3] && {{
        \\    echo ok >$work/started
        \\    while(! test -e $work/stop) sleep 0.1
        \\}} }} >[3]/dev/ux/drag/ctl &
    , .{a});
    gpa.free(try s.waitWork("started", 10_000));
    var buf: [128]u8 = undefined;
    glide(s, pt(250, 250), pt(250, 8));
    try waitFor(s, "drag", try std.fmt.bufPrint(&buf, "leave id=1 win={d}", .{a}));
    s.release(.left);
    try waitFor(s, "drag", "cancel id=1");
    try waitFor(s, "drag", "end id=1");
    try std.testing.expect(!try workHas(s, "drag", "drop"));
    const st = try s.readFile("/dev/ux/drag/state");
    defer gpa.free(st);
    try std.testing.expectEqualStrings("", st);

    // Pointer events reach windows again: a click in the window makes
    // it current as usual.
    try s.rcQuiet("touch $work/stop");
}

test "closing the source session cancels its drag" {
    var s = try start("drag-close");
    defer s.stop();
    try s.rcQuiet("cat /dev/ux/drag/events >$work/drag &");
    try s.openWindow(rect(100, 100, 400, 400), "sleep 1000");
    const w = try s.waitWindows(1);
    defer gpa.free(w);
    s.click(pt(250, 250), .left);
    harness.sleepMs(200);
    s.move(pt(250, 250));
    s.press(.left);
    harness.sleepMs(100);
    try rcf(s, "echo start 'win={d}' 'types=text' >/dev/ux/drag/ctl", .{w[0]});
    try waitFor(s, "drag", "cancel id=1");
    try waitFor(s, "drag", "end id=1");
    s.release(.left);
}
