//! Desktop under the rio fork: it claims the desktop, shows its volume
//! there, opens a folder window on a double click, moves an icon by
//! dragging (and saves where it went in uxmeta), and selects by rubber
//! band. Build rio, libux, resfs, uxmeta and desktop first
//! (scripts/test.sh builds everything).
const std = @import("std");
const harness = @import("harness");
const rect = harness.rect;
const pt = harness.pt;
const gpa = std.testing.allocator;

const start = "rc /mnt/term/cwd/test/desktop/desktop.rc";

fn waitTree(s: *harness.Session, path: []const u8, needle: []const u8) !void {
    var buf: [512]u8 = undefined;
    const cmd = try std.fmt.bufPrint(&buf, "grep -s -- '{s}' /tmp/f/{s}", .{ needle, path });
    s.waitRc(cmd, 10_000) catch |e| {
        var b2: [256]u8 = undefined;
        const got = s.rc(try std.fmt.bufPrint(&b2, "cat /tmp/f/{s}", .{path})) catch null;
        if (got) |g| {
            std.debug.print("{s} lacks '{s}':\n{s}\n", .{ path, needle, g });
            gpa.free(g);
        }
        s.saveScreen("desktop-fail");
        return e;
    };
}

/// Two numbers from the output of an rc command.
fn two(s: *harness.Session, cmd: []const u8) !harness.Point {
    const out = try s.rc(cmd);
    defer gpa.free(out);
    var it = std.mem.tokenizeAny(u8, out, " \n");
    const x = try std.fmt.parseInt(i32, it.next() orelse return error.NoNumber, 10);
    const y = try std.fmt.parseInt(i32, it.next() orelse return error.NoNumber, 10);
    return pt(x, y);
}

/// The window with that kind in Desktop's tree: the desktop is main,
/// the others KIND-N.
fn winOf(s: *harness.Session, kind: []const u8, buf: []u8) ![]const u8 {
    var b: [256]u8 = undefined;
    const cmd = if (std.mem.eql(u8, kind, "desktop"))
        "for(w in /tmp/f/win/main) if(test -d $w) basename $w"
    else
        try std.fmt.bufPrint(&b, "for(w in /tmp/f/win/{s}-*) if(test -d $w) basename $w", .{kind});
    var wb: [512]u8 = undefined;
    try s.waitRc(try std.fmt.bufPrint(&wb, "~ `{{{s}}} ?*", .{cmd}), 10_000);
    const out = try s.rc(cmd);
    defer gpa.free(out);
    const w = std.mem.trim(u8, out, " \n");
    @memcpy(buf[0..w.len], w);
    return buf[0..w.len];
}

/// A window's icon view, from /tmp/f: a folder's is in its scroll view.
fn iconsOf(win: []const u8, buf: []u8) ![]const u8 {
    if (std.mem.eql(u8, win, "main"))
        return std.fmt.bufPrint(buf, "win/main/icons", .{});
    return std.fmt.bufPrint(buf, "win/{s}/scroll/icons", .{win});
}

/// Where an item's icon centre is on the screen: the window's
/// rectangle (debug/windows), its border (the window's width minus the
/// drawable width the icon view fills, halved), the icon view's top,
/// and the item's pos.
fn itemAt(s: *harness.Session, win: []const u8, label: []const u8) !harness.Point {
    var b: [512]u8 = undefined;
    const r = try two(s, try std.fmt.bufPrint(&b, "awk '$1 == \"{s}\" {{print $3, $4}}' /tmp/f/debug/windows", .{win}));
    const rmax = try two(s, try std.fmt.bufPrint(&b, "awk '$1 == \"{s}\" {{print $5, $6}}' /tmp/f/debug/windows", .{win}));
    // The icon view's top, and the window's drawable width: a folder
    // window's scroll view is that outset by 1; the desktop's icon view is it.
    const cv = try two(s, try std.fmt.bufPrint(&b, "c=/tmp/f/win/{s}; if(test -e $c/scroll){{ awk '{{print $2}}' $c/scroll/icons/bounds; awk '{{print $1+$3-1}}' $c/scroll/bounds }}; if not {{ awk '{{print $2}}' $c/icons/bounds; awk '{{print $1+$3}}' $c/icons/bounds }}", .{win}));
    const top = cv.x;
    const cw = cv.y;
    const border = @divTrunc((rmax.x - r.x) - cw, 2);
    var ib: [128]u8 = undefined;
    const p = try two(s, try std.fmt.bufPrint(&b, "grep 'label={s} ' /tmp/f/{s}/items | sed 's/.*pos=([0-9]+),([0-9]+).*/\\1 \\2/'", .{ label, try iconsOf(win, &ib) }));
    return pt(r.x + border + p.x, r.y + border + top + p.y);
}

test "desktop: the desktop, a folder window, moving and selecting icons" {
    var s = try harness.Session.start(.{ .name = "desktop", .rio_args = &.{"-u"} });
    defer s.stop();
    try s.openWindow(rect(100, 100, 600, 500), start);
    gpa.free(try s.waitWindows(1));
    try s.waitRc("test -e /srv/ux.$user.desktop", 20_000);
    s.rcQuiet("mkdir -p /tmp/f && mount /srv/ux.$user.desktop /tmp/f") catch {};

    // Desktop's window became the desktop, with the volume at the right.
    try s.waitRc("! grep -s '^win=0 ' /dev/ux/desktop/state", 10_000);
    var db: [64]u8 = undefined;
    const dw = try winOf(s, "desktop", &db);
    var ib: [128]u8 = undefined;
    var ib2: [128]u8 = undefined;
    try waitTree(s, try std.fmt.bufPrint(&ib, "{s}/items", .{try iconsOf(dw, &ib2)}), "label=scratch icon=disk");
    try s.settle();
    s.saveScreen("desktop-desktop");
    const vol = try itemAt(s, dw, "scratch");
    try std.testing.expect(vol.x > 900);

    // A double click opens the volume's folder window.
    s.click(vol, .left);
    harness.sleepMs(60);
    s.click(vol, .left);
    var fb: [64]u8 = undefined;
    const fw = try winOf(s, "folder", &fb);
    var items_b: [128]u8 = undefined;
    var fi_b: [128]u8 = undefined;
    const fi = try iconsOf(fw, &fi_b);
    const items = try std.fmt.bufPrint(&items_b, "{s}/items", .{fi});
    try waitTree(s, items, "label=notes.txt icon=doc-text");
    try s.settle();

    // Dragging notes.txt moves it, and uxmeta remembers where.
    var pb: [256]u8 = undefined;
    const np = try two(s, try std.fmt.bufPrint(&pb, "grep 'label=notes.txt ' /tmp/f/{s} | sed 's/.*pos=([0-9]+),([0-9]+).*/\\1 \\2/'", .{items}));
    const from = try itemAt(s, fw, "notes.txt");
    s.drag(from, pt(from.x + 100, from.y + 40), .left);
    var wb: [128]u8 = undefined;
    try waitTree(s, items, try std.fmt.bufPrint(&wb, "label=notes.txt icon=doc-text pos={d},{d}", .{ np.x + 100, np.y + 40 }));
    try s.waitRc("grep -s 'item=.*/vol/notes.txt x=' $work/f/user/workspace/gui.ndb", 10_000);

    // A rubber band from the empty margin left of docs (outside its
    // cell, iconview.cellw/2 = 46 either side), across to
    // below notes.txt, selects all three items.
    const docs = try itemAt(s, fw, "docs");
    const ws = try itemAt(s, fw, "notes.txt");
    s.drag(pt(docs.x - 48, docs.y - 10), pt(ws.x + 60, ws.y + 60), .left);
    var sb: [128]u8 = undefined;
    try waitTree(s, try std.fmt.bufPrint(&sb, "{s}/tree", .{fi}), "selection=''i[0-9]+ i[0-9]+ i[0-9]+''");
    s.saveScreen("desktop-folder");
}
