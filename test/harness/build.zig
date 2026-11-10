//! The 9/UX GUI test harness (docs/testing.md).
//!
//!   zig build test                     every GUI test under test/
//!   zig build test -Dfilter=window     only tests whose names contain it
//!   zig build test -Dupdate-golden     rewrite golden PNGs instead of comparing
//!
//! Test files are found, not listed: every `*_test.zig` file directly inside a
//! directory of test/ other than harness/ and fakes/ (test/rio/*_test.zig, and
//! later test/e2e/*_test.zig) is built as its own test binary with the
//! `harness` module imported. A test file's golden images live in a
//! `golden/` directory beside it.
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const io = b.graph.io;

    const repo = b.pathFromRoot("../..");
    const root_opt = b.option([]const u8, "root", "9front root tree (default $H9ROOT, else .local/hyper9/.local/9front/root)");
    const filters = b.option([]const []const u8, "filter", "Run only the tests whose names contain one of these") orelse &.{};
    const update_golden = b.option(bool, "update-golden", "Write the screen to golden PNGs instead of comparing") orelse false;
    const artifacts = b.option([]const u8, "artifacts", "Directory for failure screenshots (default .local/test/artifacts)") orelse
        b.pathJoin(&.{ repo, ".local/test/artifacts" });

    const h9_root = root_opt orelse b.graph.environ_map.get("H9ROOT") orelse
        b.pathJoin(&.{ repo, ".local/hyper9/.local/9front/root" });

    const h9_dep = b.dependency("h9", .{
        .target = target,
        // h9 itself is always optimized: guest syscalls go through it.
        .optimize = .ReleaseSafe,
        .gfx = .headless,
    });
    const h9 = h9_dep.module("h9");

    const config = b.addOptions();
    config.addOption([]const u8, "repo", repo);
    config.addOption([]const u8, "h9_root", h9_root);
    config.addOption(bool, "update_golden", update_golden);
    config.addOption([]const u8, "artifacts", artifacts);

    const harness = b.addModule("harness", .{
        .root_source_file = b.path("src/harness.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{
            .{ .name = "h9", .module = h9 },
            .{ .name = "config", .module = config.createModule() },
        },
    });

    const test_step = b.step("test", "Run the GUI tests");

    // The harness's own unit tests (PNG, regions), no machine needed.
    const unit = b.addTest(.{ .root_module = harness, .filters = filters });
    test_step.dependOn(&b.addRunArtifact(unit).step);

    var test_dir = std.Io.Dir.cwd().openDir(io, b.pathJoin(&.{ repo, "test" }), .{ .iterate = true }) catch return;
    defer test_dir.close(io);
    var it = test_dir.iterate();
    while (it.next(io) catch null) |e| {
        if (e.kind != .directory) continue;
        if (std.mem.eql(u8, e.name, "harness") or std.mem.eql(u8, e.name, "fakes")) continue;
        const sub = b.pathJoin(&.{ repo, "test", e.name });
        var d = std.Io.Dir.cwd().openDir(io, sub, .{ .iterate = true }) catch continue;
        defer d.close(io);
        var fit = d.iterate();
        while (fit.next(io) catch null) |f| {
            if (f.kind != .file or !std.mem.endsWith(u8, f.name, "_test.zig")) continue;
            const t = b.addTest(.{
                .name = b.fmt("{s}-{s}", .{ e.name, f.name[0 .. f.name.len - 9] }),
                .root_module = b.createModule(.{
                    .root_source_file = .{ .cwd_relative = b.pathJoin(&.{ sub, f.name }) },
                    .target = target,
                    .optimize = optimize,
                    .imports = &.{.{ .name = "harness", .module = harness }},
                }),
                .filters = filters,
            });
            const run = b.addRunArtifact(t);
            run.setCwd(.{ .cwd_relative = repo });
            // GUI tests share one screen per process; files run one after
            // another so a slow CI host doesn't run several machines at once.
            run.has_side_effects = true;
            test_step.dependOn(&run.step);
        }
    }
}
