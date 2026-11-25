//! uxkit drawing a uitree client (test/uxkit/demo.rc) with libux: the
//! layout's bounds come back through $ui, a click on a card comes back
//! as uitree's click event with the card's tag, the client's answer to
//! it is drawn, and screenshots go to
//! .local/test/artifacts/uxkit-*.png. Build libux, resfs and uxkit first.
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const gpa = std.testing.allocator;

/// Dumps what a hung or failed client left: uxkit's stderr, the event
/// log, the processes and a screenshot.
fn dump(s: *harness.Session, name: []const u8) void {
    const shot = std.fmt.allocPrint(gpa, "{s}-fail", .{name}) catch return;
    defer gpa.free(shot);
    s.saveScreen(shot);
    const out = s.rc("echo '--- uxkit.err'; cat $work/uxkit.err; echo '--- uxkit.events'; cat $work/uxkit.events; echo '--- ps'; ps; echo '--- srv'; ls /srv") catch return;
    defer gpa.free(out);
    std.debug.print("{s}\n", .{out});
}

/// Runs uxkit.rc with a client in a window, mounts its tree at /n/ui and
/// waits for the client's event loop.
fn launch(s: *harness.Session, name: []const u8, r: harness.Rect, cmd: []const u8) !void {
    try s.openWindow(r, cmd);
    gpa.free(try s.waitWindows(1));
    s.waitRc("test -e /srv/uitree.$user", 20_000) catch |e| {
        dump(s, name);
        return e;
    };
    _ = try s.rc("mkdir -p /n/ui && mount /srv/uitree.$user /n/ui");
    // the client's last step: its event log exists once the tree is built
    s.waitRc("test -e $work/uxkit.events", 30_000) catch |e| {
        dump(s, name);
        return e;
    };
    try s.settle();
}

fn bounds(s: *harness.Session, path: []const u8) ![4]i32 {
    const cmd = try std.fmt.allocPrint(gpa, "cat /n/ui/win/main/{s}/bounds", .{path});
    defer gpa.free(cmd);
    const out = try s.rc(cmd);
    defer gpa.free(out);
    var r: [4]i32 = undefined;
    var it = std.mem.tokenizeAny(u8, out, " \n");
    for (&r) |*v| v.* = try std.fmt.parseInt(i32, it.next() orelse return error.BadBounds, 10);
    return r;
}

test "uxkit: a uitree mail screen drawn by libux, and a click" {
    var s = try harness.Session.start(.{ .name = "uxkit-mail", .rio_args = &.{"-u"} });
    defer s.stop();
    try launch(s, "uxkit", rect(40, 40, 900, 600), "rc /mnt/term/cwd/test/uxkit/uxkit.rc");
    s.saveScreen("uxkit-mail");

    // the sidebar keeps w=176; the cards stack in main, m1 above m2
    const side = try bounds(s, "side");
    try std.testing.expectEqual(@as(i32, 176), side[2]);
    const m1 = try bounds(s, "main/m1");
    const m2 = try bounds(s, "main/m2");
    try std.testing.expect(m2[1] > m1[1] + m1[3] - 1);
    try std.testing.expect(m1[0] >= side[0] + side[2]);

    // a click on the second card's subject line is the card's click.
    // Bounds are window-local; the window's inside is 1 px in and 4
    // down from rio's rectangle (the title bar sits above it).
    const ids = try s.windows();
    defer gpa.free(ids);
    const wr = try s.windowRect(ids[0]);
    const sub = try bounds(s, "main/m2/subj");
    const at = harness.pt(wr.min.x + 1 + sub[0] + 10, wr.min.y + 4 + sub[1] + @divTrunc(sub[3], 2));
    s.click(at, .left);
    s.waitRc("grep -s '^click open:m2 main/m2 ' $work/uxkit.events", 5_000) catch |e| {
        const dbg = try s.rc("cat $work/uxkit.events; cat $work/uxkit.err");
        defer gpa.free(dbg);
        std.debug.print("at {d},{d} win {f}\n{s}\n", .{ at.x, at.y, wr, dbg });
        return e;
    };
    // demo.rc answers by selecting the card
    try s.waitRc("grep -s 'bg=accent' /n/ui/win/main/main/m2/style", 5_000);
    try s.settle();
    s.saveScreen("uxkit-mail-clicked");
}

fn clickNode(s: *harness.Session, wr: harness.Rect, path: []const u8) !void {
    const b = try bounds(s, path);
    s.click(harness.pt(wr.min.x + 1 + b[0] + @divTrunc(b[2], 2), wr.min.y + 4 + b[1] + @divTrunc(b[3], 2)), .left);
}

fn waitEvent(s: *harness.Session, pattern: []const u8) !void {
    const cmd = try std.fmt.allocPrint(gpa, "grep -s '{s}' $work/uxkit.events", .{pattern});
    defer gpa.free(cmd);
    s.waitRc(cmd, 5_000) catch |e| {
        const dbg = try s.rc("cat $work/uxkit.events; cat $work/uxkit.err");
        defer gpa.free(dbg);
        std.debug.print("waiting for {s}\n{s}\n", .{ pattern, dbg });
        return e;
    };
}

test "uxkit: form, list, table and an alert dialog drawn with libux controls" {
    var s = try harness.Session.start(.{ .name = "uxkit-form", .rio_args = &.{"-u"} });
    defer s.stop();
    try launch(s, "uxkit-form", rect(40, 40, 760, 560), "rc /mnt/term/cwd/test/uxkit/uxkit.rc form.rc");
    s.saveScreen("uxkit-form");

    const ids = try s.windows();
    defer gpa.free(ids);
    const wr = try s.windowRect(ids[0]);

    // typing in a field is a change with the new value, mirrored in value
    try clickNode(s, wr, "main/name/f");
    try s.settle();
    try s.typeText("Ken");
    try waitEvent(s, "^change name main/name/f .*value=Ken");
    try s.waitRc("grep -s '^Ken$' /n/ui/win/main/main/name/f/value", 5_000);

    // a checkbox click is a change to off
    try clickNode(s, wr, "main/notify");
    try waitEvent(s, "^change notify main/notify .*value=off");

    // a list click selects an item by its id
    const fl = try bounds(s, "side/folders");
    s.click(harness.pt(wr.min.x + 1 + fl[0] + 30, wr.min.y + 4 + fl[1] + 24), .left);
    try waitEvent(s, "^select - side/folders .*value=sent");

    // Save is a click; the client answers in the status line
    try clickNode(s, wr, "main/buttons/save");
    try waitEvent(s, "^click save main/buttons/save");
    try s.waitRc("grep -s 'Saved Ken' /n/ui/win/main/main/status/text", 5_000);

    // Delete opens the alert dialog as a second window; Enter confirms it
    try clickNode(s, wr, "main/buttons/delete");
    gpa.free(try s.waitWindows(2));
    try s.settle();
    s.saveScreen("uxkit-form-dialog");
    try s.key('\n');
    try waitEvent(s, "^confirm confirm-delete over/confirm");
    try s.waitRc("grep -s 'Account deleted' /n/ui/win/main/main/status/text", 5_000);
    gpa.free(try s.waitWindows(1));
    try s.settle();
    s.saveScreen("uxkit-form-done");
}

fn ptIn(wr: harness.Rect, b: [4]i32, dx: i32, dy: i32) harness.Point {
    return harness.pt(wr.min.x + 1 + b[0] + dx, wr.min.y + 4 + b[1] + dy);
}

test "uxkit: tabs, select, radios, slider, switch, popover and context menu" {
    var s = try harness.Session.start(.{ .name = "uxkit-widgets", .rio_args = &.{"-u"} });
    defer s.stop();
    try launch(s, "uxkit-widgets", rect(40, 40, 620, 480), "rc /mnt/term/cwd/test/uxkit/uxkit.rc widgets.rc");
    s.saveScreen("uxkit-widgets");

    const ids = try s.windows();
    defer gpa.free(ids);
    const wr = try s.windowRect(ids[0]);

    // the select pops up its menu below it; the second item is French
    const sel = try bounds(s, "tabs/general/lang/f");
    s.click(ptIn(wr, sel, 10, @divTrunc(sel[3], 2)), .left);
    try s.settle();
    s.saveScreen("uxkit-widgets-select");
    s.click(ptIn(wr, sel, 30, sel[3] + 1 + 18 + 9), .left);
    try waitEvent(s, "^change lang tabs/general/lang/f .*value=fr");

    // the third radio button
    const rg = try bounds(s, "tabs/general/when/f");
    s.click(ptIn(wr, rg, 6, 2 * 20 + 10), .left);
    try waitEvent(s, "^change when tabs/general/when/f .*value=manual");

    // the slider's far end is its max; the release commits
    const sl = try bounds(s, "tabs/general/size/f");
    s.click(ptIn(wr, sl, sl[2] - 2, @divTrunc(sl[3], 2)), .left);
    try waitEvent(s, "^change size tabs/general/size/f .*value=24");
    try waitEvent(s, "^commit size tabs/general/size/f");

    // the switch
    try clickNode(s, wr, "tabs/general/sound");
    try waitEvent(s, "^change sound tabs/general/sound .*value=on");

    // More opens its popover
    try clickNode(s, wr, "tabs/general/foot/more");
    try waitEvent(s, "^open - over/info");
    try s.settle();
    s.saveScreen("uxkit-widgets-popover");
    try clickNode(s, wr, "tabs/general/foot/more");
    try waitEvent(s, "^close - over/info");

    // a right click on the box is its context menu; Paste is the second item
    const box = try bounds(s, "tabs/general/foot/box");
    const p = ptIn(wr, box, 20, 10);
    s.click(p, .right);
    try s.settle();
    s.saveScreen("uxkit-widgets-menu");
    s.click(harness.pt(p.x + 20, p.y + 1 + 18 + 9), .left);
    try waitEvent(s, "^action paste over/cm");
    try s.waitRc("grep -s 'Chose paste' /n/ui/win/main/status/text", 5_000);

    // the Appearance tab
    const tb = try bounds(s, "tabs");
    s.click(ptIn(wr, tb, 4 + 60 + 30, 11), .left);
    try waitEvent(s, "^change tabs tabs .*value=look");
    try s.settle();
    s.saveScreen("uxkit-widgets-tab");
}

test "uxkit: sidebar, toolbar, tooltip, breadcrumb, number input, combobox and canvas" {
    var s = try harness.Session.start(.{ .name = "uxkit-tools", .rio_args = &.{"-u"} });
    defer s.stop();
    try launch(s, "uxkit-tools", rect(40, 40, 780, 540), "rc /mnt/term/cwd/test/uxkit/uxkit.rc tools.rc");
    s.saveScreen("uxkit-tools");

    const ids = try s.windows();
    defer gpa.free(ids);
    const wr = try s.windowRect(ids[0]);

    // a sidebar item is a click with its tag
    try clickNode(s, wr, "side/work/sent");
    try waitEvent(s, "^click go:sent side/work/sent");

    // the number input's + steps the value
    const n = try bounds(s, "main/row/n");
    s.click(ptIn(wr, n, n[2] - 8, @divTrunc(n[3], 2)), .left);
    try waitEvent(s, "^change copies main/row/n .*value=6");

    // the breadcrumb's first crumb
    const cr = try bounds(s, "main/crumbs");
    s.click(ptIn(wr, cr, 8, @divTrunc(cr[3], 2)), .left);
    try waitEvent(s, "^click crumbs main/crumbs .*item=home");

    // typing in the combobox is a query; its menu offers what matches
    const cb = try bounds(s, "main/row2/c");
    s.click(ptIn(wr, cb, 20, @divTrunc(cb[3], 2)), .left);
    try s.settle();
    try s.typeText("f");
    try waitEvent(s, "^query country main/row2/c .*value=f");
    s.click(ptIn(wr, cb, cb[2] - 8, @divTrunc(cb[3], 2)), .left);
    try s.settle();
    s.saveScreen("uxkit-tools-combo");
    // the first match below the field is France (Finland is second)
    s.click(ptIn(wr, cb, cb[2] - 8 + 10, cb[3] + 1 + 9), .left);
    try waitEvent(s, "^change country main/row2/c .*value=fr");

    // a press on the canvas names the item under it, and a click follows
    const ch = try bounds(s, "main/chart");
    s.click(ptIn(wr, ch, 60, 60), .left);
    try waitEvent(s, "^mouse-down chart main/chart .*item=card");
    try waitEvent(s, "^click chart main/chart .*item=card");

    // uxd has the app, with rio's id for its window, and a click on
    // Delete through uxd reaches the client
    try s.waitRc("mkdir -p /n/ux; test -e /n/ux/windows || mount /srv/uxd.$user.admin /n/ux; grep -s 'app=tools-1' /n/ux/windows", 10_000);
    const wl = try s.rc("grep 'app=tools-1' /n/ux/windows");
    defer gpa.free(wl);
    const want = try std.fmt.allocPrint(gpa, "window win={d} app=tools-1", .{ids[0]});
    defer gpa.free(want);
    try std.testing.expectEqualStrings(want, std.mem.trim(u8, wl, " \n"));
    _ = try s.rc("echo click >/n/ux/apps/tools-1/win/main/main/tools/del/ctl");
    try waitEvent(s, "^click del main/tools/del via=ctl");

    // the pointer over Delete shows its tooltip
    const del = try bounds(s, "main/tools/del");
    s.move(ptIn(wr, del, @divTrunc(del[2], 2), @divTrunc(del[3], 2)));
    try s.waitRc("grep -s 'open' /n/ui/win/main/over/tip/tree", 5_000);
    try s.settle();
    s.saveScreen("uxkit-tools-tooltip");
}
