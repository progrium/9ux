//! Just enough PNG for golden images: writes 8-bit RGB, reads 8-bit RGB
//! and RGBA (any filter, no interlace). Pixels are 0xRRGGBB in a u32;
//! alpha is dropped on read.
const std = @import("std");
const flate = std.compress.flate;

const signature = "\x89PNG\r\n\x1a\n";

pub const Error = error{ NotPng, Unsupported, Corrupt } || std.mem.Allocator.Error;

pub const Decoded = struct {
    width: u32,
    height: u32,
    pixels: []u32,
};

fn chunk(w: *std.Io.Writer, kind: *const [4]u8, data: []const u8) !void {
    try w.writeInt(u32, @intCast(data.len), .big);
    try w.writeAll(kind);
    try w.writeAll(data);
    var crc = std.hash.Crc32.init();
    crc.update(kind);
    crc.update(data);
    try w.writeInt(u32, crc.final(), .big);
}

/// Encodes `pixels` (row-major, 0xRRGGBB) as an RGB PNG.
pub fn encode(gpa: std.mem.Allocator, width: u32, height: u32, pixels: []const u32) ![]u8 {
    std.debug.assert(pixels.len == @as(usize, width) * height);
    // Filter byte 0 (none) and RGB for each row.
    const raw = try gpa.alloc(u8, @as(usize, height) * (1 + @as(usize, width) * 3));
    defer gpa.free(raw);
    var o: usize = 0;
    for (0..height) |y| {
        raw[o] = 0;
        o += 1;
        for (pixels[y * width ..][0..width]) |p| {
            raw[o] = @truncate(p >> 16);
            raw[o + 1] = @truncate(p >> 8);
            raw[o + 2] = @truncate(p);
            o += 3;
        }
    }

    var z: std.Io.Writer.Allocating = try .initCapacity(gpa, 4096);
    defer z.deinit();
    {
        const window = try gpa.alloc(u8, flate.max_window_len);
        defer gpa.free(window);
        var c = try flate.Compress.init(&z.writer, window, .zlib, .default);
        try c.writer.writeAll(raw);
        try c.finish();
    }

    var out: std.Io.Writer.Allocating = .init(gpa);
    errdefer out.deinit();
    const w = &out.writer;
    try w.writeAll(signature);
    var ihdr: [13]u8 = undefined;
    std.mem.writeInt(u32, ihdr[0..4], width, .big);
    std.mem.writeInt(u32, ihdr[4..8], height, .big);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 2; // RGB
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    try chunk(w, "IHDR", &ihdr);
    try chunk(w, "IDAT", z.written());
    try chunk(w, "IEND", "");
    return out.toOwnedSlice();
}

fn paeth(a: u8, b: u8, c: u8) u8 {
    const p: i16 = @as(i16, a) + b - c;
    const pa = @abs(p - a);
    const pb = @abs(p - b);
    const pc = @abs(p - c);
    if (pa <= pb and pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/// Decodes an 8-bit RGB or RGBA, non-interlaced PNG.
pub fn decode(gpa: std.mem.Allocator, data: []const u8) Error!Decoded {
    if (data.len < 8 or !std.mem.eql(u8, data[0..8], signature)) return error.NotPng;
    var pos: usize = 8;
    var width: u32 = 0;
    var height: u32 = 0;
    var bpp: usize = 0;
    var idat: std.ArrayList(u8) = .empty;
    defer idat.deinit(gpa);
    while (pos + 12 <= data.len) {
        const len = std.mem.readInt(u32, data[pos..][0..4], .big);
        const kind = data[pos + 4 ..][0..4];
        if (pos + 12 + len > data.len) return error.Corrupt;
        const body = data[pos + 8 ..][0..len];
        pos += 12 + len;
        if (std.mem.eql(u8, kind, "IHDR")) {
            if (len != 13) return error.Corrupt;
            width = std.mem.readInt(u32, body[0..4], .big);
            height = std.mem.readInt(u32, body[4..8], .big);
            if (body[8] != 8 or body[12] != 0) return error.Unsupported;
            bpp = switch (body[9]) {
                2 => 3,
                6 => 4,
                else => return error.Unsupported,
            };
        } else if (std.mem.eql(u8, kind, "IDAT")) {
            try idat.appendSlice(gpa, body);
        } else if (std.mem.eql(u8, kind, "IEND")) break;
    }
    if (bpp == 0 or width == 0 or height == 0) return error.Corrupt;

    const stride = @as(usize, width) * bpp;
    const raw = try gpa.alloc(u8, @as(usize, height) * (stride + 1));
    defer gpa.free(raw);
    {
        var in: std.Io.Reader = .fixed(idat.items);
        const window = try gpa.alloc(u8, flate.max_window_len);
        defer gpa.free(window);
        var d: flate.Decompress = .init(&in, .zlib, window);
        d.reader.readSliceAll(raw) catch return error.Corrupt;
    }

    const pixels = try gpa.alloc(u32, @as(usize, width) * height);
    errdefer gpa.free(pixels);
    const cur = try gpa.alloc(u8, stride);
    defer gpa.free(cur);
    const prev = try gpa.alloc(u8, stride);
    defer gpa.free(prev);
    @memset(prev, 0);
    for (0..height) |y| {
        const row = raw[y * (stride + 1) ..][0 .. stride + 1];
        const f = row[0];
        for (0..stride) |i| {
            const x = row[1 + i];
            const a: u8 = if (i >= bpp) cur[i - bpp] else 0;
            const b = prev[i];
            const c: u8 = if (i >= bpp) prev[i - bpp] else 0;
            cur[i] = switch (f) {
                0 => x,
                1 => x +% a,
                2 => x +% b,
                3 => x +% @as(u8, @intCast((@as(u16, a) + b) / 2)),
                4 => x +% paeth(a, b, c),
                else => return error.Corrupt,
            };
        }
        for (0..width) |px| {
            const p = cur[px * bpp ..];
            pixels[y * width + px] = @as(u32, p[0]) << 16 | @as(u32, p[1]) << 8 | p[2];
        }
        @memcpy(prev, cur);
    }
    return .{ .width = width, .height = height, .pixels = pixels };
}

test "encode then decode round-trips" {
    const gpa = std.testing.allocator;
    const px = [_]u32{ 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x123456, 0x000000 };
    const data = try encode(gpa, 3, 2, &px);
    defer gpa.free(data);
    const d = try decode(gpa, data);
    defer gpa.free(d.pixels);
    try std.testing.expectEqual(3, d.width);
    try std.testing.expectEqual(2, d.height);
    try std.testing.expectEqualSlices(u32, &px, d.pixels);
}

test "decode rejects what isn't a PNG" {
    try std.testing.expectError(error.NotPng, decode(std.testing.allocator, "GIF89a.."));
}
