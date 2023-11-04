const std = @import("std");

const cstdio = @cImport({
    // See https://github.com/ziglang/zig/issues/515
    @cDefine("_NO_CRT_STDIO_INLINE", "1");
    @cInclude("stdio.h");
});

const csystypes = @cImport({
    @cInclude("sys/types.h");
});
const csysstat = @cImport({
    @cInclude("sys/stat.h");
});
const cfcntl = @cImport({
    @cInclude("fcntl.h");
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

    var evt_loop_i: u32 = 0;
    while (!exit_requested) {
        // Course do-nothing at 6ms increments until we get keypresses (evt_loop_i += 166 / second)
        std.time.sleep(6000000); // 6ms

        // Every other second, open new keyboard devices
        if (evt_loop_i % 333 == 0) {
            openAnyNewKeyboardFds();
        }

        // Handle event loop incrementing
        evt_loop_i += 1;
        if (evt_loop_i > 1000000) {
            evt_loop_i = 0;
        }
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
            var event_file_buff: [36:0]u8 = .{@as(u8, 0)} ** 36;
            var event_file = std.fmt.bufPrint(&event_file_buff, "/dev/input/event{}", .{i}) catch &event_file_buff;

            var stat_buf: std.os.Stat = undefined;
            const result = std.os.system.stat(&event_file_buff, &stat_buf);
            if (result == 0) {
                std.debug.print("Opening {s}\n", .{event_file});
                keyboard_fds[i] = cfcntl.open(&event_file_buff, cfcntl.O_RDONLY | cfcntl.O_NONBLOCK);
                std.debug.print("{s} opened as fd {d}!\n", .{ event_file, keyboard_fds[i] });
            }
        }
    }
}

pub fn asyncReadKeyboardFds() void {
    for (0..num_keyboard_fds) |i| {
        if (keyboard_fds[i] >= 0) {}
    }
}
