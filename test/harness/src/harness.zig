//! The 9/UX GUI test harness: runs a rio build on h9's headless screen
//! (a 1024x768 screen in memory), and gives tests what a person at the
//! machine would have: a keyboard, a mouse, eyes, and an rc shell inside
//! rio's namespace.
//!
//!     var s = try harness.Session.start(.{});
//!     defer s.stop();
//!     _ = try s.rc("window -r 100 100 500 400 rc");
//!     try s.waitWindows(1);
//!     try s.typeText("echo hi\n");
//!     try s.expectGolden("test/rio/golden/hi.png", .{ .rect = r });
//!
//! See docs/testing.md for the conventions.
const std = @import("std");
const builtin = @import("builtin");
const h9 = @import("h9");
const config = @import("config");
pub const png = @import("png.zig");

const headless = h9.gfx.headless;

pub const Point = struct {
    x: i32,
    y: i32,

    pub fn add(p: Point, q: Point) Point {
        return .{ .x = p.x + q.x, .y = p.y + q.y };
    }
};

pub fn pt(x: i32, y: i32) Point {
    return .{ .x = x, .y = y };
}

pub const Rect = struct {
    min: Point,
    max: Point,

    pub fn dx(r: Rect) u32 {
        return @intCast(r.max.x - r.min.x);
    }
    pub fn dy(r: Rect) u32 {
        return @intCast(r.max.y - r.min.y);
    }
    pub fn center(r: Rect) Point {
        return .{ .x = @divTrunc(r.min.x + r.max.x, 2), .y = @divTrunc(r.min.y + r.max.y, 2) };
    }
    pub fn inset(r: Rect, n: i32) Rect {
        return .{ .min = .{ .x = r.min.x + n, .y = r.min.y + n }, .max = .{ .x = r.max.x - n, .y = r.max.y - n } };
    }
    pub fn contains(r: Rect, p: Point) bool {
        return p.x >= r.min.x and p.x < r.max.x and p.y >= r.min.y and p.y < r.max.y;
    }
    pub fn format(r: Rect, w: *std.Io.Writer) std.Io.Writer.Error!void {
        try w.print("{d} {d} {d} {d}", .{ r.min.x, r.min.y, r.max.x, r.max.y });
    }
};

pub fn rect(x0: i32, y0: i32, x1: i32, y1: i32) Rect {
    return .{ .min = .{ .x = x0, .y = y0 }, .max = .{ .x = x1, .y = y1 } };
}

/// Mouse buttons, as /dev/mouse numbers them.
pub const Button = enum(u8) { left = 1, middle = 2, right = 4 };

/// Some of 9front's special keys (/sys/include/keyboard.h).
pub const Key = struct {
    pub const enter: u21 = '\n';
    pub const backspace: u21 = 0x08;
    pub const tab: u21 = '\t';
    pub const esc: u21 = 0x1b;
    pub const del: u21 = 0x7f;
    pub const kf: u21 = 0xF000;
    pub const home: u21 = kf | 0x0D;
    pub const up: u21 = kf | 0x0E;
    pub const pgup: u21 = kf | 0x0F;
    pub const print: u21 = kf | 0x10;
    pub const left: u21 = kf | 0x11;
    pub const right: u21 = kf | 0x12;
    pub const down: u21 = 0x80;
    pub const view: u21 = 0x80;
    pub const pgdown: u21 = kf | 0x13;
    pub const ins: u21 = kf | 0x14;
    pub const end: u21 = kf | 0x18;
    pub const shift: u21 = kf | 0x16;
    pub const ctl: u21 = kf | 0x17;
    pub const alt: u21 = kf | 0x15;
};

/// A copy of the screen (or part of it): 0xRRGGBB per pixel, row-major.
pub const Image = struct {
    width: u32,
    height: u32,
    pixels: []u32,

    pub fn deinit(img: Image, gpa: std.mem.Allocator) void {
        gpa.free(img.pixels);
    }

    pub fn at(img: Image, x: i32, y: i32) u32 {
        return img.pixels[@as(usize, @intCast(y)) * img.width + @as(usize, @intCast(x))];
    }

    pub fn bounds(img: Image) Rect {
        return rect(0, 0, @intCast(img.width), @intCast(img.height));
    }

    /// A copy of `r`, which must lie inside the image.
    pub fn crop(img: Image, gpa: std.mem.Allocator, r: Rect) !Image {
        if (r.min.x < 0 or r.min.y < 0 or r.max.x > img.width or r.max.y > img.height or r.max.x <= r.min.x or r.max.y <= r.min.y)
            return error.RectOutsideImage;
        const px = try gpa.alloc(u32, @as(usize, r.dx()) * r.dy());
        for (0..r.dy()) |y| {
            const src = img.pixels[(@as(usize, @intCast(r.min.y)) + y) * img.width + @as(usize, @intCast(r.min.x)) ..][0..r.dx()];
            @memcpy(px[y * r.dx() ..][0..r.dx()], src);
        }
        return .{ .width = r.dx(), .height = r.dy(), .pixels = px };
    }

    /// How many pixels in `r` (the whole image if null) are `color`.
    pub fn count(img: Image, r: ?Rect, color: u32) usize {
        const b = r orelse img.bounds();
        var n: usize = 0;
        var y = b.min.y;
        while (y < b.max.y) : (y += 1) {
            var x = b.min.x;
            while (x < b.max.x) : (x += 1) {
                if (img.at(x, y) == color) n += 1;
            }
        }
        return n;
    }

    pub fn encodePng(img: Image, gpa: std.mem.Allocator) ![]u8 {
        return png.encode(gpa, img.width, img.height, img.pixels);
    }
};

/// What a command run in the machine produced.
pub const Result = struct {
    /// rc's $status: empty is success.
    status: []u8,
    /// stdout and stderr together.
    out: []u8,

    pub fn ok(r: Result) bool {
        return r.status.len == 0;
    }
    pub fn deinit(r: Result, gpa: std.mem.Allocator) void {
        gpa.free(r.status);
        gpa.free(r.out);
    }
};

pub const Options = struct {
    /// The rio to run, relative to the repository: null means the fork
    /// built in rio/ (rio/6.out on amd64, rio/7.out on arm64).
    rio: ?[]const u8 = null,
    /// Extra arguments for rio, after the harness's own -i.
    rio_args: []const []const u8 = &.{},
    /// The whole session's deadline.
    timeout_s: u64 = 600,
    /// How long `start` waits for rio and the agent.
    start_timeout_ms: u64 = 60_000,
    /// The name of the session's work directory, .local/test/run/NAME.
    name: []const u8 = "session",
};

pub const Error = error{
    StartTimeout,
    CommandTimeout,
    CommandFailed,
    MachineExited,
    WaitTimeout,
    GoldenMismatch,
    GoldenMissing,
};

/// Only one machine in a process can have the screen.
var active = std.atomic.Value(bool).init(false);

pub const Session = struct {
    gpa: std.mem.Allocator,
    io: std.Io,
    m: h9.Machine,
    started: bool = false,
    thread: ?std.Thread = null,
    done: std.atomic.Value(bool) = .init(false),
    status_buf: [256]u8 = undefined,
    status: []const u8 = "",
    out: std.Io.Writer.Allocating,
    err: std.Io.Writer.Allocating,
    argv: [3 + 16][]const u8 = undefined,
    argc: usize = 0,
    prog: []u8 = &.{},
    init_cmd: []u8 = &.{},
    repo: []u8 = &.{},
    root: []u8 = &.{},
    /// The work directory: on the host, and in the machine.
    work: []u8 = &.{},
    work_guest: []u8 = &.{},
    next: u32 = 0,
    /// Buttons held, for move() during a drag.
    buttons: u8 = 0,
    mouse_at: Point = .{ .x = 0, .y = 0 },

    /// Starts rio on the headless screen and waits until its agent runs
    /// commands. Call `stop` when done.
    pub fn start(opts: Options) !*Session {
        const s = try launch(opts);
        // From here on, stop() undoes everything.
        s.waitReady(opts.start_timeout_ms) catch |e| {
            s.saveScreen("start-failed");
            s.stop();
            return e;
        };
        return s;
    }

    /// Everything up to rio running on its thread.
    fn launch(opts: Options) !*Session {
        const gpa = std.testing.allocator;
        const io = std.testing.io;
        if (active.swap(true, .acq_rel)) @panic("harness: one Session at a time");
        errdefer active.store(false, .release);

        const s = try gpa.create(Session);
        errdefer gpa.destroy(s);
        s.* = .{ .gpa = gpa, .io = io, .m = undefined, .out = .init(gpa), .err = .init(gpa) };
        errdefer {
            s.out.deinit();
            s.err.deinit();
            s.freeStrings();
        }

        const cwd = std.Io.Dir.cwd();
        s.repo = try realPath(gpa, config.repo);
        s.root = realPath(gpa, config.h9_root) catch |e| {
            std.debug.print("harness: no 9front root at {s} (run scripts/h9.sh --setup): {t}\n", .{ config.h9_root, e });
            return e;
        };

        // A fresh work directory under the repository, so the machine
        // sees it at /mnt/term/cwd/.local/test/run/NAME.
        const rel = try std.fmt.allocPrint(gpa, ".local/test/run/{s}", .{opts.name});
        defer gpa.free(rel);
        s.work = try std.fs.path.join(gpa, &.{ s.repo, rel });
        s.work_guest = try std.fmt.allocPrint(gpa, "/mnt/term/cwd/{s}", .{rel});
        cwd.deleteTree(io, s.work) catch {};
        try cwd.createDirPath(io, s.work);

        const arch: h9.vmm.Arch = h9.vmm.host_arch orelse return error.WrongArch;
        const rio_rel = opts.rio orelse switch (arch) {
            .amd64 => "rio/6.out",
            .arm64 => "rio/7.out",
            else => return error.WrongArch,
        };
        s.prog = try std.fmt.allocPrint(gpa, "/mnt/term/cwd/{s}", .{rio_rel});
        {
            const host_rio = try std.fs.path.join(gpa, &.{ s.repo, rio_rel });
            defer gpa.free(host_rio);
            cwd.access(io, host_rio, .{}) catch {
                std.debug.print("harness: no {s}; build it first (scripts/h9.sh --rc 'cd rio && mk', or scripts/test.sh)\n", .{rio_rel});
                return error.FileNotFound;
            };
        }
        s.init_cmd = try std.fmt.allocPrint(gpa, "rc /mnt/term/cwd/test/harness/agent.rc {s}", .{s.work_guest});
        s.argv[0] = "rio";
        s.argv[1] = "-i";
        s.argv[2] = s.init_cmd;
        s.argc = 3;
        for (opts.rio_args) |a| {
            s.argv[s.argc] = a;
            s.argc += 1;
        }

        headless.setClipboard("");
        s.m = h9.Machine.init(gpa, io, .{
            .backend = backend(),
            .arch = arch,
            .root = .{ .dir = s.root },
            .cwd = s.repo,
            .policy = .{ .timeout_ns = opts.timeout_s * std.time.ns_per_s },
            .gfx = .{},
            .stdio = .{ .out = &s.out.writer, .err = &s.err.writer },
        }) catch |e| {
            std.debug.print("harness: can't make a machine on backend {t}: {t}\n", .{ backend(), e });
            return e;
        };
        s.started = true;
        s.thread = std.Thread.spawn(.{}, runRio, .{s}) catch |e| {
            s.m.deinit();
            s.started = false;
            return e;
        };
        return s;
    }

    fn waitReady(s: *Session, timeout_ms: u64) !void {
        const ready = try std.fs.path.join(s.gpa, &.{ s.work, "ready" });
        defer s.gpa.free(ready);
        var waited: u64 = 0;
        while (waited < timeout_ms) : (waited += 20) {
            if (!s.running()) {
                s.dump();
                return error.MachineExited;
            }
            if (std.Io.Dir.cwd().access(s.io, ready, .{})) |_| return else |_| {}
            sleepMs(20);
        }
        s.dump();
        return error.StartTimeout;
    }

    fn backend() h9.vmm.BackendKind {
        // KVM where it's usable; the native backend on x86_64 Linux
        // without it (CI), which `.auto` never picks by itself.
        if (builtin.os.tag == .linux and builtin.cpu.arch == .x86_64) {
            std.Io.Dir.cwd().access(std.testing.io, "/dev/kvm", .{ .read = true, .write = true }) catch return .native;
        }
        return .auto;
    }

    fn runRio(s: *Session) void {
        const argv = s.argv[0..s.argc];
        s.status = if (s.m.run(.{ .guest = s.prog }, argv)) |st| switch (st) {
            .exited => |t| std.fmt.bufPrint(&s.status_buf, "exited '{s}'", .{t}) catch "exited",
            .killed => |t| std.fmt.bufPrint(&s.status_buf, "killed '{s}'", .{t}) catch "killed",
        } else |e| std.fmt.bufPrint(&s.status_buf, "run failed: {t}: {s}", .{ e, s.m.err_msg }) catch "run failed";
        s.done.store(true, .release);
    }

    /// Whether rio is still running.
    pub fn running(s: *Session) bool {
        return !s.done.load(.acquire);
    }

    /// Prints rio's output and status, for a failing test.
    pub fn dump(s: *Session) void {
        std.debug.print("harness: rio {s}\n--- stdout\n{s}\n--- stderr\n{s}\n", .{
            if (s.running()) "running" else s.status, s.out.written(), s.err.written(),
        });
    }

    /// Closes the window (a hangup for every process) and waits for the
    /// machine to end.
    pub fn stop(s: *Session) void {
        if (s.thread) |t| {
            headless.close();
            t.join();
            s.thread = null;
        }
        if (s.started) s.m.deinit();
        s.started = false;
        s.out.deinit();
        s.err.deinit();
        s.freeStrings();
        const gpa = s.gpa;
        gpa.destroy(s);
        active.store(false, .release);
    }

    fn freeStrings(s: *Session) void {
        for ([_][]u8{ s.prog, s.init_cmd, s.repo, s.root, s.work, s.work_guest }) |b| s.gpa.free(b);
    }

    // ---- Commands in the machine ----

    /// Runs `cmd` with rc inside rio's namespace (where `window`, /dev/wsys
    /// and /mnt/wsys work) and waits for it. `$work` is the session's
    /// work directory. The caller frees the result.
    pub fn exec(s: *Session, cmd: []const u8, timeout_ms: u64) !Result {
        const io = s.io;
        const gpa = s.gpa;
        var dir = try std.Io.Dir.cwd().openDir(io, s.work, .{});
        defer dir.close(io);
        const n = s.next;
        s.next += 1;
        var nb: [32]u8 = undefined;
        var tb: [32]u8 = undefined;
        // rc wants a final newline.
        const script = try std.mem.concat(gpa, u8, &.{ cmd, "\n" });
        defer gpa.free(script);
        try dir.writeFile(io, .{ .sub_path = try std.fmt.bufPrint(&tb, "{d}.tmp", .{n}), .data = script });
        try dir.rename(try std.fmt.bufPrint(&tb, "{d}.tmp", .{n}), dir, try std.fmt.bufPrint(&nb, "{d}.rc", .{n}), io);
        const st_name = try std.fmt.bufPrint(&nb, "{d}.status", .{n});
        var waited: u64 = 0;
        while (true) : (waited += 10) {
            if (dir.readFileAlloc(io, st_name, gpa, .limited(4096))) |st| {
                errdefer gpa.free(st);
                const out = try dir.readFileAlloc(io, try std.fmt.bufPrint(&tb, "{d}.out", .{n}), gpa, .limited(64 << 20));
                return .{ .status = st, .out = out };
            } else |_| {}
            if (!s.running()) {
                s.dump();
                return error.MachineExited;
            }
            if (waited >= timeout_ms) {
                std.debug.print("harness: command timed out after {d} ms: {s}\n", .{ timeout_ms, cmd });
                return error.CommandTimeout;
            }
            sleepMs(10);
        }
    }

    /// Runs `cmd` and returns its output; fails the test if its status
    /// isn't empty. The caller frees the output.
    pub fn rc(s: *Session, cmd: []const u8) ![]u8 {
        const r = try s.exec(cmd, 30_000);
        defer s.gpa.free(r.status);
        if (!r.ok()) {
            std.debug.print("harness: command failed: {s}\nstatus: {s}\noutput:\n{s}\n", .{ cmd, r.status, r.out });
            s.gpa.free(r.out);
            return error.CommandFailed;
        }
        return r.out;
    }

    /// `rc`, discarding the output.
    pub fn rcQuiet(s: *Session, cmd: []const u8) !void {
        s.gpa.free(try s.rc(cmd));
    }

    /// Reads a file in the machine's namespace (rio's), such as
    /// /dev/wsys/1/label. The caller frees it.
    pub fn readFile(s: *Session, path: []const u8) ![]u8 {
        const cmd = try std.fmt.allocPrint(s.gpa, "cat '{s}'", .{path});
        defer s.gpa.free(cmd);
        return s.rc(cmd);
    }

    /// Runs `cmd` until its status is empty, or fails after `timeout_ms`.
    pub fn waitRc(s: *Session, cmd: []const u8, timeout_ms: u64) !void {
        var waited: u64 = 0;
        while (true) : (waited += 50) {
            const r = try s.exec(cmd, 30_000);
            defer r.deinit(s.gpa);
            if (r.ok()) return;
            if (waited >= timeout_ms) {
                std.debug.print("harness: still failing after {d} ms: {s}\nstatus: {s}\noutput:\n{s}\n", .{ timeout_ms, cmd, r.status, r.out });
                return error.WaitTimeout;
            }
            sleepMs(50);
        }
    }

    /// Opens a window at `r` running `cmd` with rc, through `window -m`:
    /// a /mnt/wsys attach with a "new" spec, from a copy of the agent's
    /// namespace and environment (so $work works), with the new window's
    /// files bound over /dev. `cmd` may not contain single quotes.
    pub fn openWindow(s: *Session, r: Rect, cmd: []const u8) !void {
        std.debug.assert(std.mem.indexOfScalar(u8, cmd, '\'') == null);
        const c = try std.fmt.allocPrint(s.gpa, "window -m -r {d} {d} {d} {d} rc -c '{s}'", .{ r.min.x, r.min.y, r.max.x, r.max.y, cmd });
        defer s.gpa.free(c);
        try s.rcQuiet(c);
    }

    /// The machine's path for `name` in the work directory. Caller frees.
    pub fn workPath(s: *Session, name: []const u8) ![]u8 {
        return std.fmt.allocPrint(s.gpa, "{s}/{s}", .{ s.work_guest, name });
    }

    /// Reads `name` from the work directory on the host, or null if it
    /// isn't there. Caller frees.
    pub fn readWork(s: *Session, name: []const u8) !?[]u8 {
        var dir = try std.Io.Dir.cwd().openDir(s.io, s.work, .{});
        defer dir.close(s.io);
        return dir.readFileAlloc(s.io, name, s.gpa, .limited(64 << 20)) catch |e| switch (e) {
            error.FileNotFound => null,
            else => e,
        };
    }

    /// Waits until `name` in the work directory exists and isn't empty,
    /// and returns its contents. Caller frees.
    pub fn waitWork(s: *Session, name: []const u8, timeout_ms: u64) ![]u8 {
        var waited: u64 = 0;
        while (true) : (waited += 50) {
            if (try s.readWork(name)) |d| {
                if (d.len > 0) return d;
                s.gpa.free(d);
            }
            if (!s.running()) {
                s.dump();
                return error.MachineExited;
            }
            if (waited >= timeout_ms) {
                std.debug.print("harness: no {s} after {d} ms\n", .{ name, timeout_ms });
                s.saveScreen(name);
                return error.WaitTimeout;
            }
            sleepMs(50);
        }
    }

    /// One read of up to 8 KB from `path`: for files such as a window's
    /// wctl, where a second read blocks until something changes.
    pub fn readOnce(s: *Session, path: []const u8) ![]u8 {
        const cmd = try std.fmt.allocPrint(s.gpa, "dd -quiet 1 -bs 8192 -count 1 -if '{s}'", .{path});
        defer s.gpa.free(cmd);
        return s.rc(cmd);
    }

    /// Writes `data` to a file in the machine's namespace, as one write
    /// where it's small (ctl files take one command per write).
    pub fn writeFile(s: *Session, path: []const u8, data: []const u8) !void {
        const io = s.io;
        var dir = try std.Io.Dir.cwd().openDir(io, s.work, .{});
        defer dir.close(io);
        const name = try std.fmt.allocPrint(s.gpa, "data{d}", .{s.next});
        defer s.gpa.free(name);
        try dir.writeFile(io, .{ .sub_path = name, .data = data });
        const cmd = try std.fmt.allocPrint(s.gpa, "cat $work/{s} >'{s}'", .{ name, path });
        defer s.gpa.free(cmd);
        try s.rcQuiet(cmd);
    }

    /// The ids of rio's windows, from /dev/wsys, sorted. Caller frees.
    pub fn windows(s: *Session) ![]u32 {
        const out = try s.rc("ls /dev/wsys");
        defer s.gpa.free(out);
        var ids: std.ArrayList(u32) = .empty;
        errdefer ids.deinit(s.gpa);
        var it = std.mem.tokenizeAny(u8, out, " \n");
        while (it.next()) |p| {
            const base = std.fs.path.basename(p);
            try ids.append(s.gpa, std.fmt.parseInt(u32, base, 10) catch continue);
        }
        std.mem.sort(u32, ids.items, {}, std.sort.asc(u32));
        return ids.toOwnedSlice(s.gpa);
    }

    /// Waits until rio has exactly `n` windows; returns their ids.
    pub fn waitWindows(s: *Session, n: usize) ![]u32 {
        var waited: u64 = 0;
        while (true) : (waited += 50) {
            const ids = try s.windows();
            if (ids.len == n) return ids;
            s.gpa.free(ids);
            if (waited > 20_000) {
                std.debug.print("harness: wanted {d} windows\n", .{n});
                return error.WaitTimeout;
            }
            sleepMs(50);
        }
    }

    /// A window's rectangle on the screen, from its wctl file:
    /// rio reports minx miny maxx maxy, then current/notcurrent and
    /// hidden/visible.
    pub fn windowRect(s: *Session, id: u32) !Rect {
        const p = try std.fmt.allocPrint(s.gpa, "/dev/wsys/{d}/wctl", .{id});
        defer s.gpa.free(p);
        const w = try s.readOnce(p);
        defer s.gpa.free(w);
        var it = std.mem.tokenizeAny(u8, w, " \n");
        var v: [4]i32 = undefined;
        for (&v) |*x| x.* = try std.fmt.parseInt(i32, it.next() orelse return error.BadWctl, 10);
        return rect(v[0], v[1], v[2], v[3]);
    }

    /// The rest of a window's wctl after the rectangle, such as
    /// "current visible". Caller frees.
    pub fn windowState(s: *Session, id: u32) ![]u8 {
        const p = try std.fmt.allocPrint(s.gpa, "/dev/wsys/{d}/wctl", .{id});
        defer s.gpa.free(p);
        const w = try s.readOnce(p);
        defer s.gpa.free(w);
        var it = std.mem.tokenizeAny(u8, w, " \n");
        for (0..4) |_| _ = it.next();
        var out: std.ArrayList(u8) = .empty;
        errdefer out.deinit(s.gpa);
        while (it.next()) |f| {
            if (out.items.len > 0) try out.append(s.gpa, ' ');
            try out.appendSlice(s.gpa, f);
        }
        return out.toOwnedSlice(s.gpa);
    }

    // ---- Keyboard and mouse ----

    /// Presses and releases one key, as a person typing.
    pub fn key(s: *Session, r: u21) !void {
        _ = s;
        headless.key(r);
        sleepMs(5);
    }

    /// Types `text` (UTF-8), one key at a time.
    pub fn typeText(s: *Session, text: []const u8) !void {
        var it = (try std.unicode.Utf8View.init(text)).iterator();
        while (it.nextCodepoint()) |c| try s.key(c);
        sleepMs(30);
    }

    /// Holds or releases a key (for modifiers).
    pub fn keyEvent(s: *Session, r: u21, down: bool) void {
        _ = s;
        headless.keyEvent(r, down);
        sleepMs(5);
    }

    fn mouse(s: *Session, p: Point, buttons: u8) void {
        headless.mouse(p.x, p.y, buttons);
        s.mouse_at = p;
        s.buttons = buttons;
        sleepMs(25);
    }

    /// Moves the mouse to `p`, keeping the buttons held.
    pub fn move(s: *Session, p: Point) void {
        s.mouse(p, s.buttons);
    }

    pub fn press(s: *Session, b: Button) void {
        s.mouse(s.mouse_at, s.buttons | @intFromEnum(b));
    }

    pub fn release(s: *Session, b: Button) void {
        s.mouse(s.mouse_at, s.buttons & ~@intFromEnum(b));
    }

    /// Moves to `p` and clicks `b` there.
    pub fn click(s: *Session, p: Point, b: Button) void {
        s.move(p);
        s.press(b);
        s.release(b);
    }

    /// Presses `b` at `from`, moves to `to` in steps, and releases it.
    pub fn drag(s: *Session, from: Point, to: Point, b: Button) void {
        s.move(from);
        s.press(b);
        const steps = 8;
        for (1..steps + 1) |i| {
            const k: i32 = @intCast(i);
            s.move(.{
                .x = from.x + @divTrunc((to.x - from.x) * k, steps),
                .y = from.y + @divTrunc((to.y - from.y) * k, steps),
            });
        }
        s.release(b);
    }

    /// Picks item `index` (from 0) of a pop-up menu on button `b`, as
    /// rio and its windows draw them (menuhit: items `item_height`
    /// pixels apart, the last one used shown under the mouse).
    /// `from_item` is the item the menu opens on (rio's button-3 menu
    /// opens on the last one picked; 0 the first time).
    pub fn menuPick(s: *Session, at: Point, b: Button, index: u32, from_item: u32) void {
        s.move(at);
        s.press(b);
        sleepMs(100);
        const dy: i32 = (@as(i32, @intCast(index)) - @as(i32, @intCast(from_item))) * item_height;
        s.move(.{ .x = at.x, .y = at.y + @divTrunc(dy, 2) });
        s.move(.{ .x = at.x, .y = at.y + dy });
        sleepMs(100);
        s.release(b);
    }

    /// The height of a menu item: the default font's height (16, for
    /// /lib/font/bit/vga/unicode.font) plus menuhit's Vspacing of 2.
    pub var item_height: i32 = 18;

    // ---- The screen ----

    /// A copy of the whole screen. Caller frees with `Image.deinit`.
    pub fn screenshot(s: *Session) !Image {
        const sz = headless.size() orelse return error.NoScreen;
        const raw = try s.gpa.alloc(u8, @as(usize, sz.dx) * sz.dy * 4);
        defer s.gpa.free(raw);
        if (!headless.pixels(raw)) return error.NoScreen;
        const px = try s.gpa.alloc(u32, @as(usize, sz.dx) * sz.dy);
        for (px, 0..) |*p, i| p.* = std.mem.readInt(u32, raw[i * 4 ..][0..4], .little) & 0xFFFFFF;
        return .{ .width = sz.dx, .height = sz.dy, .pixels = px };
    }

    /// Calls `pred(ctx, image)` on fresh screenshots until it's true or
    /// `timeout_ms` passes.
    pub fn waitScreen(s: *Session, ctx: anytype, comptime pred: fn (@TypeOf(ctx), Image) bool, timeout_ms: u64) !void {
        var waited: u64 = 0;
        while (true) : (waited += 50) {
            const img = try s.screenshot();
            defer img.deinit(s.gpa);
            if (pred(ctx, img)) return;
            if (waited >= timeout_ms) {
                s.saveArtifact("wait-timeout", img);
                return error.WaitTimeout;
            }
            sleepMs(50);
        }
    }

    /// Waits until the screen stops changing (two equal screenshots
    /// `quiet_ms` apart).
    pub fn settle(s: *Session) !void {
        const quiet_ms = 150;
        var prev = try s.screenshot();
        defer prev.deinit(s.gpa);
        var waited: u64 = 0;
        while (waited < 10_000) : (waited += quiet_ms) {
            sleepMs(quiet_ms);
            const cur = try s.screenshot();
            const same = std.mem.eql(u32, prev.pixels, cur.pixels);
            prev.deinit(s.gpa);
            prev = cur;
            if (same) return;
        }
        return error.WaitTimeout;
    }

    pub const GoldenOptions = struct {
        /// The part of the screen to compare; null is all of it.
        rect: ?Rect = null,
        /// Pixels allowed to differ.
        tolerance: usize = 0,
        /// Wait this long for the screen to match before failing.
        timeout_ms: u64 = 5_000,
    };

    /// Compares the screen (or `o.rect`) with the golden PNG at `path`
    /// (relative to the repository). With -Dupdate-golden, writes it
    /// instead. On a mismatch, the actual image and a diff go to the
    /// artifacts directory.
    pub fn expectGolden(s: *Session, path: []const u8, o: GoldenOptions) !void {
        const gpa = s.gpa;
        const io = s.io;
        const full = try std.fs.path.join(gpa, &.{ s.repo, path });
        defer gpa.free(full);
        if (config.update_golden) {
            try s.settle();
            const shot = try s.screenshot();
            defer shot.deinit(gpa);
            const img = if (o.rect) |r| try shot.crop(gpa, r) else shot;
            defer if (o.rect != null) img.deinit(gpa);
            const data = try img.encodePng(gpa);
            defer gpa.free(data);
            if (std.fs.path.dirname(full)) |d| try std.Io.Dir.cwd().createDirPath(io, d);
            try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = full, .data = data });
            std.debug.print("harness: wrote {s}\n", .{path});
            return;
        }
        const data = std.Io.Dir.cwd().readFileAlloc(io, full, gpa, .limited(64 << 20)) catch {
            std.debug.print("harness: no golden image {s} (run with -Dupdate-golden to make it)\n", .{path});
            return error.GoldenMissing;
        };
        defer gpa.free(data);
        const want = try png.decode(gpa, data);
        defer gpa.free(want.pixels);

        var waited: u64 = 0;
        while (true) : (waited += 100) {
            const shot = try s.screenshot();
            defer shot.deinit(gpa);
            const img = if (o.rect) |r| try shot.crop(gpa, r) else shot;
            defer if (o.rect != null) img.deinit(gpa);
            const diff = if (img.width == want.width and img.height == want.height) blk: {
                var n: usize = 0;
                for (img.pixels, want.pixels) |a, b| n += @intFromBool(a != b);
                break :blk n;
            } else std.math.maxInt(usize);
            if (diff <= o.tolerance) return;
            if (waited >= o.timeout_ms) {
                const base = std.fs.path.stem(path);
                s.saveArtifact(base, img);
                if (img.width == want.width and img.height == want.height) {
                    const d = try gpa.alloc(u32, want.pixels.len);
                    defer gpa.free(d);
                    for (d, img.pixels, want.pixels) |*x, a, b| x.* = if (a == b) (a >> 2) & 0x3F3F3F else 0xFF00FF;
                    const dname = try std.fmt.allocPrint(gpa, "{s}.diff", .{base});
                    defer gpa.free(dname);
                    s.saveArtifact(dname, .{ .width = img.width, .height = img.height, .pixels = d });
                    std.debug.print("harness: {s}: {d} pixels differ (tolerance {d})\n", .{ path, diff, o.tolerance });
                } else {
                    std.debug.print("harness: {s}: golden is {d}x{d}, screen region is {d}x{d}\n", .{ path, want.width, want.height, img.width, img.height });
                }
                return error.GoldenMismatch;
            }
            sleepMs(100);
        }
    }

    /// Writes `img` to the artifacts directory as NAME.png, for looking
    /// at after a failure (CI uploads the directory).
    pub fn saveArtifact(s: *Session, name: []const u8, img: Image) void {
        const gpa = s.gpa;
        const io = s.io;
        std.Io.Dir.cwd().createDirPath(io, config.artifacts) catch return;
        const p = std.fmt.allocPrint(gpa, "{s}/{s}.png", .{ config.artifacts, name }) catch return;
        defer gpa.free(p);
        const data = img.encodePng(gpa) catch return;
        defer gpa.free(data);
        std.Io.Dir.cwd().writeFile(io, .{ .sub_path = p, .data = data }) catch return;
        std.debug.print("harness: saved {s}\n", .{p});
    }

    /// Saves the whole screen as an artifact.
    pub fn saveScreen(s: *Session, name: []const u8) void {
        const img = s.screenshot() catch return;
        defer img.deinit(s.gpa);
        s.saveArtifact(name, img);
    }
};

fn realPath(gpa: std.mem.Allocator, p: []const u8) ![]u8 {
    const z = try std.Io.Dir.cwd().realPathFileAlloc(std.testing.io, p, gpa);
    defer gpa.free(z);
    return gpa.dupe(u8, z);
}

pub fn sleepMs(ms: u64) void {
    std.Io.sleep(std.testing.io, .fromMilliseconds(@intCast(ms)), .awake) catch {};
}

test {
    _ = png;
}

test "Image.crop and count" {
    const gpa = std.testing.allocator;
    var px = [_]u32{ 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    const img: Image = .{ .width = 3, .height = 3, .pixels = &px };
    const c = try img.crop(gpa, rect(1, 1, 3, 3));
    defer c.deinit(gpa);
    try std.testing.expectEqualSlices(u32, &.{ 5, 6, 8, 9 }, c.pixels);
    try std.testing.expectEqual(1, img.count(null, 5));
    try std.testing.expectError(error.RectOutsideImage, img.crop(gpa, rect(2, 2, 4, 4)));
}
