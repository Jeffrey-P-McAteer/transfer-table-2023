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
const cunistd = @cImport({
    @cInclude("unistd.h");
});
const clinuxinput = @cImport({
    @cInclude("linux/input.h");
});

const csignal = @cImport({
    @cInclude("signal.h");
});

const cstring = @cImport({
    @cInclude("string.h");
});

const creal_errno = @cImport({
    @cInclude("real_errno.h");
});

const cpigpio = @cImport({
    @cInclude("pigpio.h");
});

// Global data
var exit_requested: bool = false;
const num_keyboard_fds: u32 = 42;
var keyboard_fds: [num_keyboard_fds]c_int = .{@as(c_int, -1)} ** num_keyboard_fds;
const num_input_events: u32 = 24;
var input_events: [num_input_events]?clinuxinput.input_event = .{null} ** num_input_events;
var input_events_i: u32 = 0;

pub fn main() !void {
    cpigpio.gpioSetSignalFunc(csignal.SIGINT, motorControlSignalHandler);
    cpigpio.gpioSetSignalFunc(csignal.SIGTERM, motorControlSignalHandler);

    var evt_loop_i: u32 = 0;
    while (!exit_requested) {
        // Course do-nothing at 6ms increments until we get keypresses (evt_loop_i += 166 / second)
        std.time.sleep(6000000); // 6ms

        // Every other second, open new keyboard devices
        if (evt_loop_i % 333 == 0) {
            openAnyNewKeyboardFds();
        }

        asyncReadKeyboardFds();

        // every 12ms perform keypresses if we have any
        if (evt_loop_i % 2 == 0) {
            performInputEvents(false);
        }

        // Handle event loop incrementing
        evt_loop_i += 1;
        if (evt_loop_i > std.math.maxInt(u32) / 2) {
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
        if (keyboard_fds[i] >= 0) {
            var input_event: clinuxinput.input_event = undefined;
            var num_bytes_read = cunistd.read(keyboard_fds[i], &input_event, @sizeOf(clinuxinput.input_event));
            if (num_bytes_read >= 0) {
                //std.debug.print("read {d} bytes, input_event = {}\n", .{ num_bytes_read, input_event });
                if (input_events_i >= num_input_events) {
                    input_events_i = 0;
                }
                input_events[input_events_i] = input_event;
                performInputEvents(true);
            } else {
                //var errno = @as(c_int, @intFromEnum(std.c.getErrno(c_int)));
                var errno: c_int = creal_errno.get_errno();
                var resource_unavailable_nonfatal = errno == 11;
                if (!resource_unavailable_nonfatal and errno != 0) {
                    var err_cstring = cstring.strerror(errno);
                    std.debug.print("fd {d} read gave error {d} ({s}), closing...\n", .{ i, errno, err_cstring });
                    _ = cunistd.close(keyboard_fds[i]);
                    keyboard_fds[i] = -1;
                }
            }
        }
    }
}

pub fn performInputEvents(immediate_pass: bool) void {
    for (0..num_input_events) |i| {
        if (input_events[i]) |one_nonempty_input_event| {
            performOneInputEvent(immediate_pass, one_nonempty_input_event);
            if (!immediate_pass) {
                // Zero events if not doing an immediate pass
                input_events[i] = null;
            }
        }
    }
}

pub fn performOneInputEvent(immediate_pass: bool, event: clinuxinput.input_event) void {
    std.debug.print("immediate_pass = {} event = {}\n", .{ immediate_pass, event });
}
