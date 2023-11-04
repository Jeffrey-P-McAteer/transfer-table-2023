const std = @import("std");

const cstdio = @cImport({
    // See https://github.com/ziglang/zig/issues/515
    @cDefine("_NO_CRT_STDIO_INLINE", "1");
    @cInclude("stdio.h");
});

const cstdlib = @cImport({
    @cInclude("stdlib.h");
});

const csignal = @cImport({
    @cInclude("signal.h");
});

const cpigpio = @cImport({
    @cInclude("pigpio.h");
});

// Global data
var exit_requested: bool = false;

pub fn main() !void {
    std.debug.print("Hello, World!\n", .{});

    cpigpio.gpioSetSignalFunc(csignal.SIGINT, motorControlSignalHandler);
    cpigpio.gpioSetSignalFunc(csignal.SIGTERM, motorControlSignalHandler);

    while (!exit_requested) {
        std.time.sleep(512000000); // 512ms
        std.debug.print("Tick!\n", .{});
    }
    std.debug.print("main() exiting!\n", .{});
}

pub fn motorControlSignalHandler(sig_val: c_int) callconv(.C) void {
    std.debug.print("Caught signal {d}\n", .{sig_val});
    exit_requested = true;
}
