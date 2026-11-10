//! Shared by the stock rio regression tests: rio's colors, and small
//! helpers on top of the harness.
const std = @import("std");
const harness = @import("harness");

pub const gpa = std.testing.allocator;

/// Stock rio's colors (rio/data.c, wind.c), as the screen shows them.
pub const color = struct {
    pub const background: u32 = 0x777777;
    /// The border of the current window, and of the others.
    pub const border_current: u32 = 0x55AAAA;
    pub const border_other: u32 = 0x9EEEEE;
    pub const text_bg: u32 = 0xFFFFFF;
    /// acme's body and sam's command window: colors nothing else draws.
    pub const acme_body: u32 = 0xFFFFEA;
    pub const sam_cmd: u32 = 0xEAFFFF;
};

/// rio's window border width (Selborder).
pub const border = 4;

/// Waits for `pred` on the screen with a 20 second limit.
pub fn waitScreen(s: *harness.Session, ctx: anytype, comptime pred: fn (@TypeOf(ctx), harness.Image) bool) !void {
    try s.waitScreen(ctx, pred, 20_000);
}

/// The id of the only window, waiting for it.
pub fn onlyWindow(s: *harness.Session) !u32 {
    const ids = try s.waitWindows(1);
    defer gpa.free(ids);
    return ids[0];
}

/// Waits until window `id`'s wctl reports rectangle `r`.
pub fn waitRect(s: *harness.Session, id: u32, r: harness.Rect) !void {
    var waited: u32 = 0;
    while (true) : (waited += 50) {
        const got = try s.windowRect(id);
        if (std.meta.eql(got, r)) return;
        if (waited > 10_000) {
            std.debug.print("window {d}: want {f}, have {f}\n", .{ id, r, got });
            return error.WaitTimeout;
        }
        harness.sleepMs(50);
    }
}

/// Waits until window `id`'s state (after the rectangle in wctl)
/// contains `word`, such as "hidden" or "current".
pub fn waitState(s: *harness.Session, id: u32, word: []const u8) !void {
    var waited: u32 = 0;
    while (true) : (waited += 50) {
        const st = try s.windowState(id);
        defer gpa.free(st);
        var it = std.mem.tokenizeScalar(u8, st, ' ');
        while (it.next()) |w| if (std.mem.eql(u8, w, word)) return;
        if (waited > 10_000) {
            std.debug.print("window {d}: want {s}, state is {s}\n", .{ id, word, st });
            return error.WaitTimeout;
        }
        harness.sleepMs(50);
    }
}

/// The work file `name`'s contents, trimmed of white space.
pub fn workTrimmed(s: *harness.Session, name: []const u8) ![]u8 {
    const d = try s.waitWork(name, 20_000);
    defer gpa.free(d);
    return gpa.dupe(u8, std.mem.trim(u8, d, " \t\n"));
}
