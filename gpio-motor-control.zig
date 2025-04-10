const std = @import("std");

const cstdio = @cImport({
    // See https://github.com/ziglang/zig/issues/515
    @cDefine("_NO_CRT_STDIO_INLINE", "1");
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("stdio.h");
});

const csystypes = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("sys/types.h");
});
const csysstat = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("sys/stat.h");
});
const csystime = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("sys/time.h");
});
const cfcntl = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("fcntl.h");
});
const cunistd = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("unistd.h");
});
const csched = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("sched.h");
});
const clinuxinput = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("linux/input.h");
});
const clinuxinputeventcodes = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("linux/input-event-codes.h");
});

const csignal = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("signal.h");
});

const cstring = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("string.h");
});

const creal_errno = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("real_errno.h");
});
const c_linux_pat = @cImport({
    @cInclude("linux_priority_affinity_tasks.h");
});

const cpigpio = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("pigpio.h");
});

// Shared w/ webserver.py
const PMEM_FILE = "/mnt/usb1/pmem.bin";
const GPIO_MOTOR_KEYS_IN_DIR = "/tmp/gpio_motor_keys_in";

// When this file exists, other programs should assume the table is being driven and ought NOT perform analysis routines.
const GPIO_MOTOR_ACTIVE_FLAG_FILE = "/tmp/gpio_motor_is_active";
const GPIO_MOTOR_ACTIVE_MTIME_FILE = "/tmp/gpio_motor_last_active_mtime";
const GPIO_MOTOR_EMERGENCY_STOP_FLAG_FILE = "/tmp/emergency_stop_occurred";
const GPIO_MOTOR_EMERGENCY_STOP_CLEARED_FLAG_FILE = "/tmp/emergency_stop_cleared";


// Hardware Constants
const PREFERRED_CPU = 3;

const MOTOR_ENABLE_PIN = 22;
const MOTOR_DIRECTION_PIN = 27;
const MOTOR_STEP_PIN = 17;

const LOW = 0;
const HIGH = 1;
const MOTOR_ENABLE_SIGNAL = 0;
const MOTOR_DISABLE_SIGNAL = 1;

// const RAMP_UP_STEPS = 5200;
const RAMP_UP_STEPS = 14200;

const SLOWEST_US = 600;
//const FASTEST_US = 62;
const FASTEST_US = 224;


// Global data
var exit_requested: bool = false;
const num_keyboard_fds: u32 = 256;
var keyboard_fds: [num_keyboard_fds]c_int = .{@as(c_int, -1)} ** num_keyboard_fds;
const num_input_events: u32 = 24;
var input_events: [num_input_events]?clinuxinput.input_event = .{null} ** num_input_events;
var input_events_i: u32 = 0;

var motor_stop_requested: bool = false;
var num_input_buffer: i32 = 0;
var dial_num_steps_per_click: usize = 40;
var last_written_pmem_hash: i32 = 0;


const num_positions: u32 = 12;
const pos_dat = extern struct {
    step_position: i32,
    cm_position: f64,
};
const pmem_struct = extern struct {
    logical_position: u32,
    step_position: i32,
    positions: [num_positions]pos_dat align(1),
};
var pmem: pmem_struct = undefined;

pub fn main() !void {

    _ = csignal.signal(csignal.SIGUSR1, motorControlSignalHandler);
    _ = csignal.signal(csignal.SIGUSR2, motorControlSignalHandler);

    if (!(cpigpio.gpioInitialise()>=0)) {
      std.debug.print("Error in gpioInitialise(), exiting!\n", .{});
      return;
    }

    c_linux_pat.set_priority_and_cpu_affinity(PREFERRED_CPU, -20);

    _ = cpigpio.gpioSetSignalFunc(csignal.SIGINT,  motorControlSignalHandler);
    _ = cpigpio.gpioSetSignalFunc(csignal.SIGTERM, motorControlSignalHandler);
    _ = cpigpio.gpioSetSignalFunc(csignal.SIGUSR1, motorControlSignalHandler);
    _ = cpigpio.gpioSetSignalFunc(csignal.SIGUSR2, motorControlSignalHandler);

    _ = cpigpio.gpioSetMode(MOTOR_ENABLE_PIN, cpigpio.PI_OUTPUT);
    _ = cpigpio.gpioSetMode(MOTOR_DIRECTION_PIN, cpigpio.PI_OUTPUT);
    _ = cpigpio.gpioSetMode(MOTOR_STEP_PIN, cpigpio.PI_OUTPUT);

    _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL);
    _ = cpigpio.gpioWrite(MOTOR_DIRECTION_PIN,  LOW);
    _ = cpigpio.gpioWrite(MOTOR_STEP_PIN,       LOW);

    read_pmem_from_file();

    // Do an open-close open-close loop on keyboard inputs in an attempt to ensure they are all opened & being listened to
    openAnyNewKeyboardFds();
    std.time.sleep(6000000); // 6ms
    closeAllHidFds();
    std.time.sleep(6000000); // 6ms
    openAnyNewKeyboardFds();
    std.time.sleep(6000000); // 6ms
    closeAllHidFds();
    std.time.sleep(6000000); // 6ms
    openAnyNewKeyboardFds();
    std.time.sleep(6000000); // 6ms

    var evt_loop_i: u32 = 0;
    var num_ticks_with_motor_stop_requested: u32 = 0;
    while (!exit_requested) {
        // Coarse do-nothing at 6ms increments until we get keypresses (evt_loop_i += 166 / second)
        std.time.sleep(6000000); // 6ms

        if (motor_stop_requested) {
          if (num_ticks_with_motor_stop_requested > 676) { // approx 4 seconds
            std.debug.print("Allowing motor to run again...\n", .{});
            motor_stop_requested = false; // reset it
            num_ticks_with_motor_stop_requested = 0;
            delete_emergency_stop_file();
          }
          else {
            num_ticks_with_motor_stop_requested += 1;
          }
        }
        else {
          num_ticks_with_motor_stop_requested = 0;
        }

        // Twice a second (second=166 _i s), check and add key presses from other system processes to input_events[]
        if (evt_loop_i % 83 == 50) {
            injectForeignKeypresses();
        }

        // Every other second, open new keyboard devices
        if (evt_loop_i % 333 == 0) {
            openAnyNewKeyboardFds();
        }
        // Every other second, write pmem if different
        if (evt_loop_i % 333 == 100) {
            write_pmem_to_file_iff_diff();
        }
        // Every other second, turn motor off (we ought only have it on within performInputEvents anyway)
        if (evt_loop_i % 333 == 200) {
            _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL);
        }

        // Every 3rd second, log debug data
        if (evt_loop_i % 500 == 1) {
          std.debug.print("pmem.step_position = {d}!\n", .{ pmem.step_position });
        }

        // Every 3rd second sync disks
        if (evt_loop_i % 500 == 250) {
            sync_disks();
        }

        // Every 120th second close all fds
        if (evt_loop_i % (500 * 40) == 250) {
            closeAllHidFds();
        }

        asyncReadKeyboardFds();

        // every 12ms perform keypresses if we have any
        if (evt_loop_i % 2 == 0) {
            performInputEvents(false);
        }

        // If we have exited from performInputEvents, the table motion is complete!
        // This need not be immediate, so we allow it to only be checked every 240ms
        if (evt_loop_i % 40 == 1) {
            delete_motor_active_file();
        }

        // Handle event loop incrementing
        evt_loop_i += 1;
        if (evt_loop_i > std.math.maxInt(u32) / 4) {
            evt_loop_i = 0;
        }
    }
    std.debug.print("main() exiting!\n", .{});
}


pub fn create_emergency_stop_cleared_file() void {
    const fd = cfcntl.open(GPIO_MOTOR_EMERGENCY_STOP_CLEARED_FLAG_FILE, cfcntl.O_RDWR | cfcntl.O_CREAT);
    if (fd >= 0) {
        const active_file_bytes = "---";
        _ = cunistd.write(fd, active_file_bytes, @sizeOf(@TypeOf(active_file_bytes)));
        _ = cunistd.close(fd);
    }
}

pub fn create_emergency_stop_file() void {
    const fd = cfcntl.open(GPIO_MOTOR_EMERGENCY_STOP_FLAG_FILE, cfcntl.O_RDWR | cfcntl.O_CREAT);
    if (fd >= 0) {
        const active_file_bytes = "---";
        _ = cunistd.write(fd, active_file_bytes, @sizeOf(@TypeOf(active_file_bytes)));
        _ = cunistd.close(fd);
    }
}

pub fn delete_emergency_stop_file() void {
    std.fs.accessAbsolute(GPIO_MOTOR_EMERGENCY_STOP_FLAG_FILE, .{ .mode = std.fs.File.OpenMode.read_only }) catch {
        return; // If we cannot access the path, it does not exist!
    };
    std.fs.deleteFileAbsolute(GPIO_MOTOR_EMERGENCY_STOP_FLAG_FILE) catch |err| {
        std.debug.print("deleteFileAbsolute failed: {}\n", .{err});
    };
}



pub fn create_motor_active_file() void {
    const fd = cfcntl.open(GPIO_MOTOR_ACTIVE_FLAG_FILE, cfcntl.O_RDWR | cfcntl.O_CREAT);
    if (fd >= 0) {
        const active_file_bytes = "---";
        _ = cunistd.write(fd, active_file_bytes, @sizeOf(@TypeOf(active_file_bytes)));
        _ = cunistd.close(fd);
    }
    const fd2 = cfcntl.open(GPIO_MOTOR_ACTIVE_MTIME_FILE, cfcntl.O_RDWR | cfcntl.O_CREAT);
    if (fd2 >= 0) {
        const active_file_bytes = "---";
        _ = cunistd.write(fd2, active_file_bytes, @sizeOf(@TypeOf(active_file_bytes)));
        _ = cunistd.close(fd2);
    }
}

pub fn delete_motor_active_file() void {
    std.fs.accessAbsolute(GPIO_MOTOR_ACTIVE_FLAG_FILE, .{ .mode = std.fs.File.OpenMode.read_only }) catch {
        return; // If we cannot access the path, it does not exist!
    };
    std.fs.deleteFileAbsolute(GPIO_MOTOR_ACTIVE_FLAG_FILE) catch |err| {
        std.debug.print("deleteFileAbsolute failed: {}\n", .{err});
    };
}

pub fn motorControlSignalHandler(sig_val: c_int) callconv(.C) void {
    std.debug.print("Caught signal {d}\n", .{sig_val});
    // Handle a remote-process halt command
    if (sig_val == csignal.SIGUSR1 or sig_val == csignal.SIGUSR2) {
        std.debug.print("REMOTE PROCESS HALT\n", .{});

        motor_stop_requested = true;
        std.debug.print("Motor stop requested! (sig_val={d})\n", .{sig_val});
        _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL); // IMMEDIATELY pull power from motor
        create_emergency_stop_file();

        return;
    }
    exit_requested = true;
}

pub fn closeAllHidFds() void {
    for (0..num_keyboard_fds) |i| {
        if (keyboard_fds[i] > 0) {
            std.debug.print("closing fd[{d}] = {d} because it was open for >120s \n", .{ i, keyboard_fds[i] });
            _ = cunistd.close(keyboard_fds[i]);
            keyboard_fds[i] = -1;
        }
    }
}

pub fn openAnyNewKeyboardFds() void {
    for (0..num_keyboard_fds) |i| {
        if (keyboard_fds[i] < 0) {
            var event_file_buff: [36:0]u8 = .{@as(u8, 0)} ** 36;
            const event_file = std.fmt.bufPrint(&event_file_buff, "/dev/input/event{}", .{i}) catch &event_file_buff;

            var stat_buf: std.posix.Stat = undefined;
            const result = std.posix.system.stat(&event_file_buff, &stat_buf);
            if (result == 0) {
                std.debug.print("Opening {s}\n", .{event_file});
                keyboard_fds[i] = cfcntl.open(&event_file_buff, cfcntl.O_RDONLY | cfcntl.O_NONBLOCK);
                std.debug.print("{s} opened as fd {d}!\n", .{ event_file, keyboard_fds[i] });
            }
        }
    }
}

pub fn injectForeignKeypresses() void {
    // For each file in a folder, read content as int & construct a
    // input_event.type == clinuxinputeventcodes.EV_KEY && input_event.value == 1
    // struct with event.code == the ASCII number in the file.s
    var stat_buf: std.posix.Stat = undefined;
    const result = std.posix.system.stat(GPIO_MOTOR_KEYS_IN_DIR, &stat_buf);
    if (result != 0) { // if dir does not exist, create it!
        std.debug.print("Creating {s}\n", .{GPIO_MOTOR_KEYS_IN_DIR});
        std.fs.makeDirAbsolute(GPIO_MOTOR_KEYS_IN_DIR) catch |err| {
            std.debug.print("makeDirAbsolute failed: {}\n", .{err});
            return;
        };
    }

    const dir = std.fs.openDirAbsolute(GPIO_MOTOR_KEYS_IN_DIR, .{.iterate = true}) catch |err| {
        std.debug.print("openDirAbsolute failed: {}\n", .{err});
        return;
    };
    var iterator = dir.iterate();
    while (iterator.next()) |file| {
        if (file == null) {
            break;
        }
        else {
            const file_name = file.?.name;
            std.debug.print("file = {s}\n", .{ file.?.name });

            var open_f = dir.openFile(file_name, .{}) catch |err| {
                std.debug.print("openFile failed: {}\n", .{err});
                continue;
            };

            var f_content: [4096]u8 = std.mem.zeroes([4096]u8);
            _ = open_f.readAll(&f_content) catch |err| {
                std.debug.print("readAll failed: {}\n", .{err});
                continue;
            };

            std.debug.print("f_content = {s}\n", .{f_content});

            // Iterate all characters, calling std.fmt.parseInt() on every range of non-digit-non-digit ranges of values.
            // This means for contents like "ab12 34,56" we'll parse out 12, 34, and 56.
            var current_int_begin_i: usize = 0;
            var current_int_end_i: usize = 0;
            for (0..4096) |i| {
                if (std.ascii.isDigit(f_content[i])) {
                    current_int_end_i = i+1;
                }
                else {
                    if (current_int_end_i > current_int_begin_i) {
                        if (!std.ascii.isDigit(f_content[current_int_begin_i])) {
                            current_int_begin_i += 1;
                        }
                        if (current_int_end_i > current_int_begin_i) {
                            // we have reached the end of a digit! try to parse begin..end
                            // std.debug.print("Parsing this {s}\n", .{f_content[current_int_begin_i..current_int_end_i]});
                            const parsed_keycode: u32 = std.fmt.parseInt(u32, f_content[current_int_begin_i..current_int_end_i], 10) catch |err| {
                                std.debug.print("parseInt failed: {}\n", .{err});
                                break; // assume the worst, some unicode chaos
                            };

                            std.debug.print("parsed_keycode = {}\n", .{parsed_keycode});
                            // Insert into buffer as event struct

                            var event: clinuxinput.input_event = undefined;
                            event.type = clinuxinputeventcodes.EV_KEY;
                            event.value = 1;
                            event.code = @truncate(parsed_keycode);

                            if (input_events_i >= num_input_events) {
                                input_events_i = 0;
                            }
                            input_events[input_events_i] = event;
                            input_events_i += 1;

                        }
                    }
                    current_int_begin_i = i;
                    current_int_end_i = i;
                }
            }


            // Finally delete the file, we're done with it
            dir.deleteFile(file_name) catch |err| {
                std.debug.print("deleteFile failed: {}\n", .{err});
                continue;
            };
        }
    }
    else |err| {
        std.debug.print("iterator.next() failed: {}\n", .{err});
    }

}


pub fn asyncReadKeyboardFds() void {
    for (0..num_keyboard_fds) |i| {
        if (keyboard_fds[i] >= 0) {
            var input_event: clinuxinput.input_event = undefined;
            const num_bytes_read = cunistd.read(keyboard_fds[i], &input_event, @sizeOf(clinuxinput.input_event));
            if (num_bytes_read >= 0) {
                //std.debug.print("read {d} bytes, input_event = {}\n", .{ num_bytes_read, input_event });
                if (input_events_i >= num_input_events) {
                    input_events_i = 0;
                }
                const is_keypress = input_event.type == clinuxinputeventcodes.EV_KEY;
                const is_key_down = input_event.value == 1;
                if (is_keypress and is_key_down) {
                  input_events[input_events_i] = input_event;
                  input_events_i += 1;
                  performInputEvents(true);
                }
            } else {
                //var errno = @as(c_int, @intFromEnum(std.c.getErrno(c_int)));
                const errno: c_int = creal_errno.get_errno();
                const resource_unavailable_nonfatal = errno == 11;
                if (!resource_unavailable_nonfatal and errno != 0) {
                    const err_cstring = cstring.strerror(errno);
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
            const is_keypress = one_nonempty_input_event.type == clinuxinputeventcodes.EV_KEY;
            const is_key_down = one_nonempty_input_event.value == 1;
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
    if (code == 1 or code == 15 or code == 51 or code == 83 or
        code == clinuxinputeventcodes.KEY_KPPLUS or code == clinuxinputeventcodes.KEY_KPMINUS or code == clinuxinputeventcodes.KEY_KPDOT or
        code == clinuxinputeventcodes.KEY_SLASH or code == clinuxinputeventcodes.KEY_NUMERIC_STAR or
        code == 98 or code == 55) { // 98 is '/' and 55 is * on the keypad
        // escape, tab, 000 key, decimal key are all mapped to immediate halt
        motor_stop_requested = true;
        std.debug.print("Motor stop requested! (code={d})\n", .{code});
        _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL); // IMMEDIATELY pull power from motor
        create_emergency_stop_file();
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
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 0;
        }
        else if (code == clinuxinputeventcodes.KEY_KP1) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 1;
        }
        else if (code == clinuxinputeventcodes.KEY_KP2) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 2;
        }
        else if (code == clinuxinputeventcodes.KEY_KP3) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 3;
        }
        else if (code == clinuxinputeventcodes.KEY_KP4) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 4;
        }
        else if (code == clinuxinputeventcodes.KEY_KP5) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 5;
        }
        else if (code == clinuxinputeventcodes.KEY_KP6) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 6;
        }
        else if (code == clinuxinputeventcodes.KEY_KP7) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 7;
        }
        else if (code == clinuxinputeventcodes.KEY_KP8) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 8;
        }
        else if (code == clinuxinputeventcodes.KEY_KP9) {
            num_input_buffer = @mod(num_input_buffer, 100_000);
            num_input_buffer *= 10;
            num_input_buffer += 9;
        }
        else if (code == 96 or code == 28) {
            // Enter is 96 on keypad, 28 is enter on QWERTY
            perform_num_input_buffer(num_input_buffer);
            num_input_buffer = 0;
        }
        else if (code == clinuxinputeventcodes.KEY_BACKSPACE or code == 14 or code == clinuxinputeventcodes.KEY_EQUAL or code == 113) {
          // backspace or equal pressed or dial pressed down
          last_written_pmem_hash = 0;
          if (pmem.logical_position >= 0 and pmem.logical_position < num_positions) {
            pmem.positions[pmem.logical_position].step_position = pmem.step_position;
            pmem.positions[pmem.logical_position].cm_position = 0.0; // TODO store gloal sonar step count!
            std.debug.print("Saving: \n", .{});
            std.debug.print("pmem.positions[{}].step_position = {d}\n", .{pmem.logical_position, pmem.step_position} );
            std.debug.print("pmem.positions[{}].cm_position = {}\n", .{pmem.logical_position, 0.0} );
          }
          write_pmem_to_file_iff_diff();

        }
        else if (code == 115) {
          // Clockwise dial spin
          create_motor_active_file();
          std.debug.print("Clockwise dial spin {d} times\n", .{dial_num_steps_per_click});
          _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_ENABLE_SIGNAL);
          for (0..dial_num_steps_per_click) |i| {
            if (i % 300 == 0) { // Every 300 steps ensure we read a character for safety
              asyncReadKeyboardFds();
            }
            step_once(180, HIGH);
          }
          _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL);
        }
        else if (code == 114) {
          // Counter-Clockwise dial spin
          create_motor_active_file();
          std.debug.print("Counter-Clockwise dial spin {d} times\n", .{dial_num_steps_per_click});
          _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_ENABLE_SIGNAL);
          for (0..dial_num_steps_per_click) |i| {
            if (i % 300 == 0) { // Every 300 steps ensure we read a character for safety
              asyncReadKeyboardFds();
            }
            step_once(180, LOW);
          }
          _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL);
        }
        else {
            std.debug.print("[UNKNOWN-KEYCODE] code = {d}\n", .{code});
        }
    }
}
pub fn perform_num_input_buffer(num: i32) void {
    std.debug.print("[Enter] num_input_buffer = {d}\n", .{num});
    if (num >= 1 and num <= 12) {
        move_to_position(@intCast(num) );
    }
    else if (num == 90) {
        zero_pmem_struct();
    }
    else if (num == 99) {
        create_emergency_stop_cleared_file();
    }
    else if (num >= 1001 and num <= 1800) {
        // Set dial sensitivity to num - 1000 steps per click
        var num_steps_per_click: i32 = num - 1000;
        if (num_steps_per_click < 1) {
          num_steps_per_click = 1;
        }
        if (num_steps_per_click > 800) {
          num_steps_per_click = 800;
        }
        std.debug.print("Setting dial sensitivity to {d} steps/click\n", .{num_steps_per_click});
        dial_num_steps_per_click = @intCast(num_steps_per_click);
    }
    else {
        std.debug.print("[UNKNOWN-NUMBER] num = {d}\n", .{num});
    }
}

pub fn move_to_position(pos_num: u8) void {
  if (pos_num < 1 or pos_num > pmem.positions.len) {
    std.debug.print("Refusing to move to invalid position {d}!\n", .{pos_num});
    return;
  }

  create_motor_active_file();

  const target_position: i32 = pmem.positions[pos_num-1].step_position;
  std.debug.print("Moving from pmem.step_position = {d} to target_position = {d}\n", .{pmem.step_position, target_position});

  const num_steps_to_move: i32 = pmem.step_position - pmem.positions[pos_num-1].step_position;

  std.debug.print("Sending abs({d}) steps to motor in direction of magnitude\n", .{num_steps_to_move});

  _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_ENABLE_SIGNAL);

  if (num_steps_to_move < 0) {
    step_n(@intCast(-num_steps_to_move), @intCast(RAMP_UP_STEPS), LOW);
  }
  else if (num_steps_to_move > 0) {
    step_n(@intCast(num_steps_to_move), @intCast(RAMP_UP_STEPS), HIGH);
  }

  // Write mtime at END of movement as well!
  create_motor_active_file();

  // Even if we're emergency-stopped, record where we think we are.
  pmem.logical_position = pos_num-1;
  num_input_buffer = 0;

  _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL);

}


pub fn step_n(n: u32, ramp_up_end_n_arg: u32, level: c_uint) void {
  // For very short steps, limit top speed & change ramp up bounds.
  var ramp_up_end_n = ramp_up_end_n_arg;
  var fastest_us: u32 = FASTEST_US;

  // Avoid harmonics by shifting slower in one dir; constants found by testing
  if (level == HIGH) {
    fastest_us = FASTEST_US+7;
  }

  if (n < ramp_up_end_n) {
    ramp_up_end_n = (n / 2) - 1;
    fastest_us = FASTEST_US; // TODO calculate ideal off N + some math
  }

  const ramp_down_begin_n: i32 = @intCast(n - ramp_up_end_n);
  const slow_fast_us_dist: f32 = @floatFromInt(@as(u32, SLOWEST_US - fastest_us));
  const half_pi: f32 = std.math.pi / 2.0;
  const wavelength: f32 = std.math.pi / (@as(f32, @floatFromInt(ramp_up_end_n))); // formula is actually 2pi/wavelength, but I want to double ramp_up_end_n so instead removed the existing 2.0.

  // Ramp up on a sinusoid
  for (0..@as(usize, @intCast(ramp_up_end_n))) |i| {
    const delay_us_d: f32 = @as(f32, @floatFromInt(fastest_us) ) + (
      slow_fast_us_dist * (@sin((wavelength *  @as(f32, @floatFromInt(i) ) ) + half_pi) + 1.0)
    );

    var delay_us: u32 = @intFromFloat(delay_us_d);
    if (delay_us <= 0) {
      delay_us = 1;
    }

    step_once(delay_us, level);

    if (i % 300 == 0) { // Every 300 steps ensure we read a character for safety
      asyncReadKeyboardFds();
      if (motor_stop_requested) {
        break;
      }
    }

  }

  // Constant speed @ fastest_us, IF we have that (else just ramp_down -> n)
  if (ramp_up_end_n < ramp_down_begin_n) {
    for (@as(usize, @intCast(ramp_up_end_n))..@as(usize, @intCast(ramp_down_begin_n))) |i| {

      step_once(fastest_us + @as(u32, @intCast(@mod(i, 2))), level);

      if (i % 300 == 0) { // Every 300 steps ensure we read a character for safety
        asyncReadKeyboardFds();
        if (motor_stop_requested) {
          break;
        }
      }
    }
  }


  // Ramp down on a sinusoid
  for (@as(usize, @intCast(ramp_down_begin_n))..@as(usize, @intCast(n))) |j| {
    const i = n - j;
    const delay_us_d: f32 = @as(f32, @floatFromInt(fastest_us) ) + (
      slow_fast_us_dist * (@sin((wavelength *  @as(f32, @floatFromInt(i) ) ) + half_pi) + 1.0)
    );

    var delay_us: u32 = @intFromFloat(delay_us_d);
    if (delay_us <= 0) {
      delay_us = 1;
    }

    step_once(delay_us, level);

    if (i % 300 == 0) { // Every 300 steps ensure we read a character for safety
      asyncReadKeyboardFds();
      if (motor_stop_requested) {
        break;
      }
    }

  }

}

pub fn step_once(delay_us: u32, level: c_uint) void {
  if (motor_stop_requested) {
    return;
  }

  _ = cpigpio.gpioWrite(MOTOR_DIRECTION_PIN,  level);

  _ = cpigpio.gpioWrite(MOTOR_STEP_PIN, HIGH);

  busy_wait(delay_us / 2);

  if (motor_stop_requested) {
    return;
  }

  _ = cpigpio.gpioWrite(MOTOR_STEP_PIN, LOW);

  busy_wait(delay_us / 2);

  if (level == 0) { // low == 0 == counter-clockwise rotation on dial, position is going towards 999999
    pmem.step_position += 1;
  }
  else if (level == 1) { // high == 1 == clockwise rotation on dial, position is going towards 0
    pmem.step_position -= 1;
  }
  else {
    @panic("in step_once, level is not 0 or 1!");
  }

}


pub fn busy_wait(wait_delay_us: u32) void {
  // std.time.sleep(wait_delay_us * 1000); // todo better

  var begin_tv: csystime.timeval = undefined;
  var now_tv: csystime.timeval = undefined;
  _ = csystime.gettimeofday(&begin_tv, null);

  var elapsed_tv: csystime.timeval = undefined;

  while (!motor_stop_requested and !exit_requested) {
    _ = csystime.gettimeofday(&now_tv, null);
    // Perform offset-aware subtraction
    //csystime.timersub(&now_tv, &begin_tv, &elapsed_tv);

    c_linux_pat.timersub_cmacro(@ptrCast(&now_tv), @ptrCast(&begin_tv), @ptrCast(&elapsed_tv));

    if (elapsed_tv.tv_usec >= wait_delay_us or elapsed_tv.tv_sec > 0) {
      break;
    }
  }

}

pub fn pmem_hash() i32 {
  var h: i32 = 0;
  h += @intCast(pmem.logical_position);
  h += @intCast(pmem.step_position + 24000);
  for (0..num_positions) |i| {
    h += @intCast((pmem.positions[i].step_position * @as(i32, @intCast(i)) ) + 24000);
  }
  return h;
}

pub fn zero_pmem_struct() void {
  std.debug.print("Zeroing pmem! TABLE MUST BE AT 0 POS!\n", .{});

  pmem.logical_position = 0;
  pmem.step_position = 0; // On first run TABLE MUST BE AT 0!

  pmem.positions[0].step_position = 0;
  pmem.positions[0].cm_position = 13.509912;

  pmem.positions[1].step_position = 40600;
  pmem.positions[1].cm_position = 18.856425;

  pmem.positions[2].step_position = 81700;
  pmem.positions[2].cm_position = 23.028162;

  pmem.positions[3].step_position = 122500;
  pmem.positions[3].cm_position = 27.684387;

  pmem.positions[4].step_position = 163300;
  pmem.positions[4].cm_position = 31.834687;

  pmem.positions[5].step_position = 203700;
  pmem.positions[5].cm_position = 37.309825;

  pmem.positions[6].step_position = 244544;
  pmem.positions[6].cm_position = 44.778650;

  pmem.positions[7].step_position = 303840;
  pmem.positions[7].cm_position = 49.246225;

  pmem.positions[8].step_position = 344164;
  pmem.positions[8].cm_position = 54.348350;

  pmem.positions[9].step_position = 385244;
  pmem.positions[9].cm_position = 61.122600;

  pmem.positions[10].step_position = 425700;
  pmem.positions[10].cm_position = 67.073650;

  pmem.positions[11].step_position = 466000;
  pmem.positions[11].cm_position = 70.486500;
}


pub fn read_pmem_from_file() void {
  const fd = cfcntl.open(PMEM_FILE, cfcntl.O_RDONLY);
  var read_success = false;
  if (fd >= 0) {
    const num_bytes_read = cunistd.read(fd, &pmem, @sizeOf(@TypeOf(pmem)));
    if (num_bytes_read >= 0) {
      read_success = true;
    }
    _ = cunistd.close(fd);
  }
  if (!read_success) {
    zero_pmem_struct();
  }
  last_written_pmem_hash = pmem_hash();
}

pub fn write_pmem_to_file_iff_diff() void {
  const p_hash = pmem_hash();
  if (p_hash == last_written_pmem_hash) {
    return;
  }
  const fd = cfcntl.open(PMEM_FILE, cfcntl.O_RDWR | cfcntl.O_CREAT);
  if (fd >= 0) {
    _ = cunistd.write(fd, &pmem, @sizeOf(@TypeOf(pmem)));
    _ = cunistd.close(fd);
    std.debug.print("Wrote pmem to /mnt/usb1/pmem.bin\n", .{});
    last_written_pmem_hash = pmem_hash();
  }
  sync_disks();
}

pub fn sync_disks() void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer std.debug.assert(gpa.deinit() == .ok);
    const allocator = gpa.allocator();

    const result = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{ "sync" },
    }) catch |err| {
        std.debug.print("sync err: {}", .{ err });
        return;
    };
    defer {
        allocator.free(result.stdout);
        allocator.free(result.stderr);
    }
    if (result.stdout.len > 0) {
        std.debug.print("sync stdout: {s}", .{ result.stdout });
    }
    if (result.stderr.len > 0) {
        std.debug.print("sync stderr: {s}", .{ result.stderr });
    }

}

