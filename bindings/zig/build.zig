//! Zig bindings for CWIST: `zig build test` against an installed libcwist.
//!
//! libcwist is found through `pkg-config --static cwist` (the `cwist.pc` that
//! `make install` writes); set PKG_CONFIG_PATH when it is not in a default
//! location. Other packages use the `cwist` module this file exports.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const flags = pkgConfigFlags(b);

    // The C API, translated from the installed headers (src/cwist.h).
    const translate = b.addTranslateC(.{
        .root_source_file = b.path("src/cwist.h"),
        .target = target,
        .optimize = optimize,
    });
    // src/cwist.h includes the system <stdatomic.h> itself; this keeps
    // libttak's portable fallback, which translate-c cannot parse, out.
    translate.defineCMacro("__TTAK_STDATOMIC_SYSTEM_INCLUDED", "1");
    for (flags.include_dirs.items) |dir| translate.addIncludePath(b.graph.cwdRelativePath(dir));

    const mod = b.addModule("cwist", .{
        .root_source_file = b.path("src/cwist.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{.{ .name = "c", .module = translate.createModule() }},
    });
    for (flags.lib_dirs.items) |dir| mod.addLibraryPath(b.graph.cwdRelativePath(dir));
    // -lc, -lm, -lpthread and -ldl map to Zig's own libc handling; everything
    // else is searched in the -L directories first, so libcwist and its
    // bundled dependencies link statically.
    for (flags.libs.items) |lib| {
        if (std.mem.eql(u8, lib, "stdc++")) {
            linkCxxRuntime(b, mod, target);
        } else {
            mod.linkSystemLibrary(lib, .{ .use_pkg_config = .no });
        }
    }

    const tests = b.addTest(.{ .root_module = mod });
    const test_step = b.step("test", "Run the binding tests against libcwist");
    test_step.dependOn(&b.addRunArtifact(tests).step);
}

/// Links the C++ runtime libcwist's bundled C++ code (BoringSSL) was built
/// against. On a native Linux build that is the system libstdc++ plus the
/// libgcc_s unwinder, as the g++ driver links them: the objects reference
/// libstdc++ internals (std::__throw_out_of_range_fmt) and _Unwind_Resume,
/// which Zig's own libc++ does not provide. Elsewhere, including macOS where
/// the system runtime is libc++, Zig's libc++ is used.
fn linkCxxRuntime(b: *std.Build, mod: *std.Build.Module, target: std.Build.ResolvedTarget) void {
    if (target.query.isNative() and target.result.os.tag == .linux) {
        const cxx = b.graph.environ_map.get("CXX") orelse "c++";
        // libgcc_s.so is usually a linker script; the .so.1 is the library.
        const stdcxx = compilerFile(b, cxx, "libstdc++.so");
        const gcc_s = compilerFile(b, cxx, "libgcc_s.so.1");
        if (stdcxx != null and gcc_s != null) {
            mod.addObjectFile(b.graph.cwdRelativePath(stdcxx.?));
            mod.addObjectFile(b.graph.cwdRelativePath(gcc_s.?));
            mod.link_libc = true;
            return;
        }
    }
    mod.linkSystemLibrary("stdc++", .{ .use_pkg_config = .no });
}

/// The absolute path the C++ compiler resolves `name` to, or null.
fn compilerFile(b: *std.Build, cxx: []const u8, name: []const u8) ?[]const u8 {
    const arg = b.fmt("-print-file-name={s}", .{name});
    switch (b.runFallible(&.{ cxx, arg }, .{ .stderr_behavior = .ignore })) {
        .success => |stdout| {
            const path = std.mem.trim(u8, stdout, " \t\r\n");
            // A bare name back means the compiler did not find it.
            return if (std.fs.path.isAbsolute(path)) path else null;
        },
        else => return null,
    }
}

const Flags = struct {
    include_dirs: std.ArrayList([]const u8) = .empty,
    lib_dirs: std.ArrayList([]const u8) = .empty,
    libs: std.ArrayList([]const u8) = .empty,
};

/// System libraries `cwist.pc` names as plain `-l` flags. Their own
/// pkg-config files supply the directory when it is not a default one
/// (Homebrew on macOS), the same way the CWIST Makefile finds them.
const system_deps = [_][]const u8{
    "libzstd", "libbrotlienc", "libbrotlicommon", "libbrotlidec", "libcurl", "libnghttp2",
};

fn pkgConfigFlags(b: *std.Build) Flags {
    const arena = b.graph.arena;
    const pkg_config = b.graph.environ_map.get("PKG_CONFIG") orelse "pkg-config";
    var flags: Flags = .{};

    // --static: Libs.private (curl, nghttp2) is needed for a static libcwist.
    const out = switch (b.runFallible(&.{ pkg_config, "--cflags", "--libs", "--static", "cwist" }, .{
        .stderr_behavior = .inherit,
    })) {
        .success => |stdout| stdout,
        else => std.debug.panic(
            "libcwist was not found through {s}. Build and install CWIST first, for example\n" ++
                "    make && make install PREFIX=$HOME/.local\n" ++
                "then set PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig",
            .{pkg_config},
        ),
    };
    addFlags(arena, &flags, out);

    for (system_deps) |dep| {
        switch (b.runFallible(&.{ pkg_config, "--libs-only-L", dep }, .{ .stderr_behavior = .ignore })) {
            .success => |stdout| addFlags(arena, &flags, stdout),
            // Optional: without a .pc file the library must be on a default path.
            else => {},
        }
    }
    return flags;
}

fn addFlags(arena: std.mem.Allocator, flags: *Flags, output: []const u8) void {
    var it = std.mem.tokenizeAny(u8, output, " \t\r\n");
    while (it.next()) |arg| {
        const list, const value = if (std.mem.startsWith(u8, arg, "-I"))
            .{ &flags.include_dirs, arg[2..] }
        else if (std.mem.startsWith(u8, arg, "-L"))
            .{ &flags.lib_dirs, arg[2..] }
        else if (std.mem.startsWith(u8, arg, "-l"))
            .{ &flags.libs, arg[2..] }
        else
            continue;
        for (list.items) |seen| {
            if (std.mem.eql(u8, seen, value)) break;
        } else list.append(arena, value) catch @panic("OOM");
    }
}
