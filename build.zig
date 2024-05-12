const builtin = @import("builtin");
const std = @import("std");

// See https://ziglearn.org/chapter-3/
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const t_arch = target.result.cpu.arch;

    const motor_control_exe = b.addExecutable(.{
        .name = "gpio-motor-control",
        .root_source_file = .{ .path = "gpio-motor-control.zig" },
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    motor_control_exe.linkSystemLibrary("c");
    motor_control_exe.addIncludePath(.{ .path = "zig_c_code" });

    if (t_arch.isARM() or t_arch.isAARCH64()) {
        motor_control_exe.linkSystemLibrary("pigpio");
    } else {
        // Link a shim we compile from C-land on x86 machines
        motor_control_exe.addIncludePath(.{ .path = "shims" });
    }

    b.installArtifact(motor_control_exe);
}
