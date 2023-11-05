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
const clinuxinputeventcodes = @cImport({
    @cInclude("linux/input-event-codes.h");
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

var motor_stop_requested: bool = false;
var num_input_buffer: i32 = 0;
var dial_num_steps_per_click: i32 = 0;

const num_positions: u32 = 12;
const pos_dat = packed struct {
    step_position: i32,
    cm_position: f64,
};
const pmem_struct = packed struct {
    logical_position: u32,
    step_position: i32,
    positions: [num_positions]pos_dat,
};
var pmem: pmem_struct = .{};

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
            // See https://www.kernel.org/doc/html/latest/input/event-codes.html
            var is_keypress = one_nonempty_input_event.type == clinuxinputeventcodes.EV_KEY;
            var is_key_down = one_nonempty_input_event.value == 1;
            if (is_keypress and is_key_down) {
                performOneInputEvent(immediate_pass, one_nonempty_input_event);
            }
            if (!immediate_pass) {
                // Zero events if not doing an immediate pass
                input_events[i] = null;
            }
        }
    }
}

pub fn performOneInputEvent(immediate_pass: bool, event: clinuxinput.input_event) void {
    //std.debug.print("immediate_pass = {} event = {}\n", .{ immediate_pass, event });
    var code = event.code;
    if (code == 1 or code == 15 or code == 51 or code == 83) {
        // escape, tab, 000 key, decimal key are all mapped to immediate halt
        motor_stop_requested = true;
        std.debug.print("Motor stop requested! (code={d})\n", .{code});
        return;
    }
    if (!immediate_pass) {
        // First normalize keycodes to the keypad numbers, so QWERTY 1 and keypad 1 are identical.
        if (code == clinuxinputeventcodes.KEY_0) {
            code = clinuxinputeventcodes.KEY_KP0;
        }
        else if (code == clinuxinputeventcodes.KEY_1) {
            code = clinuxinputeventcodes.KEY_KP1;
        }
        else if (code == clinuxinputeventcodes.KEY_2) {
            code = clinuxinputeventcodes.KEY_KP2;
        }
        else if (code == clinuxinputeventcodes.KEY_3) {
            code = clinuxinputeventcodes.KEY_KP3;
        }
        else if (code == clinuxinputeventcodes.KEY_4) {
            code = clinuxinputeventcodes.KEY_KP4;
        }
        else if (code == clinuxinputeventcodes.KEY_5) {
            code = clinuxinputeventcodes.KEY_KP5;
        }
        else if (code == clinuxinputeventcodes.KEY_6) {
            code = clinuxinputeventcodes.KEY_KP6;
        }
        else if (code == clinuxinputeventcodes.KEY_7) {
            code = clinuxinputeventcodes.KEY_KP7;
        }
        else if (code == clinuxinputeventcodes.KEY_8) {
            code = clinuxinputeventcodes.KEY_KP8;
        }
        else if (code == clinuxinputeventcodes.KEY_9) {
            code = clinuxinputeventcodes.KEY_KP9;
        }

        // Perform code
        if (code == clinuxinputeventcodes.KEY_KP0) {
            num_input_buffer *= 10;
            num_input_buffer += 0;
        }
        else if (code == clinuxinputeventcodes.KEY_KP1) {
            num_input_buffer *= 10;
            num_input_buffer += 1;
        }
        else if (code == clinuxinputeventcodes.KEY_KP2) {
            num_input_buffer *= 10;
            num_input_buffer += 2;
        }
        else if (code == clinuxinputeventcodes.KEY_KP3) {
            num_input_buffer *= 10;
            num_input_buffer += 3;
        }
        else if (code == clinuxinputeventcodes.KEY_KP4) {
            num_input_buffer *= 10;
            num_input_buffer += 4;
        }
        else if (code == clinuxinputeventcodes.KEY_KP5) {
            num_input_buffer *= 10;
            num_input_buffer += 5;
        }
        else if (code == clinuxinputeventcodes.KEY_KP6) {
            num_input_buffer *= 10;
            num_input_buffer += 6;
        }
        else if (code == clinuxinputeventcodes.KEY_KP7) {
            num_input_buffer *= 10;
            num_input_buffer += 7;
        }
        else if (code == clinuxinputeventcodes.KEY_KP8) {
            num_input_buffer *= 10;
            num_input_buffer += 8;
        }
        else if (code == clinuxinputeventcodes.KEY_KP9) {
            num_input_buffer *= 10;
            num_input_buffer += 9;
        }
        else if (code == 96 or code == 28) {
            // Enter is 96 on keypad, 28 is enter on QWERTY
            std.debug.print("[Enter] num_input_buffer = {d}\n", .{num_input_buffer});
            perform_num_input_buffer(num_input_buffer);
            num_input_buffer = 0;
        }
        else if (code == clinuxinputeventcodes.KEY_BACKSPACE or code == 14 or code == clinuxinputeventcodes.KEY_EQUAL or code == 113) {
          // backspace or equal pressed


          // if (pmem.position >= 0 && pmem.position < NUM_POSITIONS) {
          //   pmem.position_data[pmem.position].steps_from_0 = pmem.table_steps_from_0;
          //   pmem.position_data[pmem.position].cm_from_0_expected = position_cm;
          //   printf("Saving: \n");
          //   printf("pmem.position_data[%d].steps_from_0 = %ld\n", pmem.position, pmem.table_steps_from_0);
          //   printf("pmem.position_data[%d].cm_from_0_expected = %f\n", pmem.position, position_cm);
          // }
          // write_pmem_to_file_iff_diff();

        }
        else if (code == 115) {
          // Clockwise dial spin

        }
        else if (code == 114) {
          // Counter-Clockwise dial spin

        }
        else {
            std.debug.print("[UNKNOWN-KEYCODE] code = {d}\n", .{code});
        }
    }
}
pub fn perform_num_input_buffer(num: i32) void {
    if (num >= 1 and num <= 12) {
        std.debug.print("Moving to position = {d}\n", .{num});
    }
    else if (num >= 1001 and num <= 2000) {
        // Set dial sensitivity tonum - 1000 steps per click
        let num_steps_per_click = num - 1000;
        std.debug.print("Setting dial sensitivity to {d} steps/click\n", .{num_steps_per_click});
        dial_num_steps_per_click = num_steps_per_click;
    }
    else {
        std.debug.print("[UNKNOWN-NUMBER] num = {d}\n", .{num});
    }
}
