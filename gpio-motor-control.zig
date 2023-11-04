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
const num_keyboard_fds: u32 = 42;
// const keyboard_fds: [num_keyboard_fds]c_int
var keyboard_fds: [num_keyboard_fds]c_int = .{@as(c_int, -1)} ** num_keyboard_fds; //std.mem.zeroes([num_keyboard_fds]c_int);

pub fn main() !void {
    std.debug.print("Hello, World!\n", .{});

    cpigpio.gpioSetSignalFunc(csignal.SIGINT, motorControlSignalHandler);
    cpigpio.gpioSetSignalFunc(csignal.SIGTERM, motorControlSignalHandler);

    // We couldn't initialize the array with -1s, so do that here.
    for (0..num_keyboard_fds) |i| {
        keyboard_fds[i] = -1;
    }

    openAnyNewKeyboardFds();

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

pub fn openAnyNewKeyboardFds() void {
    for (0..num_keyboard_fds) |i| {
        if (keyboard_fds[i] < 0) {
            const event_prefix = "/dev/input/event";
            const event_file_len = event_prefix.len + 12;
            var event_file: [event_file_len]u8 = .{@as(u8, 0)} ** event_file_len;
            for (event_prefix, 0..) |char_val, cst_i| event_file[cst_i] = char_val;
            var i_buf: [12]u8 = .{@as(u8, 0)} ** 12;
            var i_buf_slice = std.fmt.bufPrint(&i_buf, "{}", .{i}) catch &i_buf;

            for (i_buf_slice, 0..) |char_val, cst_i| event_file[event_prefix.len + cst_i] = char_val;

            std.debug.print("Does {s} exist?!\n", .{event_file});
        }
    }
}
