const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const cwist = b.dependency("cwist", .{ .target = target, .optimize = optimize });

    const exe = b.addExecutable(.{
        .name = "zig-hello",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{.{ .name = "cwist", .module = cwist.module("cwist") }},
        }),
    });
    b.installArtifact(exe);

    const run = b.addRunArtifact(exe);
    run.step.dependOn(b.getInstallStep());
    b.step("run", "Serve on http://127.0.0.1:8080").dependOn(&run.step);
}
