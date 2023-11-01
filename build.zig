const std = @import("std");

// See https://ziglearn.org/chapter-3/
pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const motor_control_exe = b.addExecutable(.{
        .name = "gpio-motor-control",
        .root_source_file = .{ .path = "gpio-motor-control.zig" },
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    motor_control_exe.linkSystemLibrary("c");

    b.installArtifact(motor_control_exe);
}
