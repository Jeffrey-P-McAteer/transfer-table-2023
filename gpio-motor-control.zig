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

// Hardware Constants
const PREFERRED_CPU = 3;

const MOTOR_ENABLE_PIN = 22;
const MOTOR_DIRECTION_PIN = 27;
const MOTOR_STEP_PIN = 17;

// 23 is north-most (closest to ground), 24 is south-most pin (closest to USB ports)
const SONAR_TRIGGER_PIN = 23;
const SONAR_ECHO_PIN = 24;

const RAMP_UP_STEPS = 14200;

const LOW = 0;
const HIGH = 1;
const MOTOR_ENABLE_SIGNAL = 0;
const MOTOR_DISABLE_SIGNAL = 1;


// Global data
var exit_requested: bool = false;
const num_keyboard_fds: u32 = 42;
var keyboard_fds: [num_keyboard_fds]c_int = .{@as(c_int, -1)} ** num_keyboard_fds;
const num_input_events: u32 = 24;
var input_events: [num_input_events]?clinuxinput.input_event = .{null} ** num_input_events;
var input_events_i: u32 = 0;

var motor_stop_requested: bool = false;
var num_input_buffer: i32 = 0;
var dial_num_steps_per_click: usize = 100;
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

    if (!(cpigpio.gpioInitialise()>=0)) {
      std.debug.print("Error in gpioInitialise(), exiting!\n", .{});
      return;
    }

    c_linux_pat.set_priority_and_cpu_affinity(PREFERRED_CPU, -20);

    _ = cpigpio.gpioSetSignalFunc(csignal.SIGINT, motorControlSignalHandler);
    _ = cpigpio.gpioSetSignalFunc(csignal.SIGTERM, motorControlSignalHandler);

    _ = cpigpio.gpioSetMode(MOTOR_ENABLE_PIN, cpigpio.PI_OUTPUT);
    _ = cpigpio.gpioSetMode(MOTOR_DIRECTION_PIN, cpigpio.PI_OUTPUT);
    _ = cpigpio.gpioSetMode(MOTOR_STEP_PIN, cpigpio.PI_OUTPUT);
    _ = cpigpio.gpioSetMode(SONAR_TRIGGER_PIN, cpigpio.PI_OUTPUT);
    _ = cpigpio.gpioSetMode(SONAR_ECHO_PIN, cpigpio.PI_INPUT);

    _ = cpigpio.gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL);
    _ = cpigpio.gpioWrite(MOTOR_DIRECTION_PIN,  LOW);
    _ = cpigpio.gpioWrite(MOTOR_STEP_PIN,       LOW);
    _ = cpigpio.gpioWrite(SONAR_TRIGGER_PIN,    LOW);

    var evt_loop_i: u32 = 0;
    while (!exit_requested) {
        // Course do-nothing at 6ms increments until we get keypresses (evt_loop_i += 166 / second)
        std.time.sleep(6000000); // 6ms

        // Every other second, open new keyboard devices
        if (evt_loop_i % 333 == 0) {
            openAnyNewKeyboardFds();
        }
        // Every other second, write pmem if different
        if (evt_loop_i % 333 == 100) {
            write_pmem_to_file_iff_diff();
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
                var is_keypress = input_event.type == clinuxinputeventcodes.EV_KEY;
                var is_key_down = input_event.value == 1;
                if (is_keypress and is_key_down) {
                  input_events[input_events_i] = input_event;
                  input_events_i += 1;
                  performInputEvents(true);
                }
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
            perform_num_input_buffer(num_input_buffer);
            num_input_buffer = 0;
        }
        else if (code == clinuxinputeventcodes.KEY_BACKSPACE or code == 14 or code == clinuxinputeventcodes.KEY_EQUAL or code == 113) {
          // backspace or equal pressed or dial pressed down


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
          std.debug.print("Clockwise dial spin {d} times\n", .{dial_num_steps_per_click});
          for (0..dial_num_steps_per_click) |_| {
            step_once(120, HIGH);
          }
        }
        else if (code == 114) {
          // Counter-Clockwise dial spin
          std.debug.print("Counter-Clockwise dial spin {d} times\n", .{dial_num_steps_per_click});
          for (0..dial_num_steps_per_click) |_| {
            step_once(120, LOW);
          }
        }
        else {
            std.debug.print("[UNKNOWN-KEYCODE] code = {d}\n", .{code});
        }
    }
}
pub fn perform_num_input_buffer(num: i32) void {
    std.debug.print("[Enter] num_input_buffer = {d}\n", .{num});
    if (num >= 1 and num <= 12) {
        std.debug.print("Moving to position = {d}\n", .{num});
    }
    else if (num == 99) {
        std.debug.print("Zeroing pmem! TABLE MUST BE AT 0 POS!\n", .{});
        zero_pmem_struct();
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

pub fn step_n_eased() void {
  // Big TODO; we do key-code reading here using asyncReadKeyboardFds()
  // which will perform the high-importance code handling.

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

  if (level == 0) {
    pmem.step_position -= 1;
  }
  else if (level == 1) {
    pmem.step_position += 1;
  }
  else {
    @panic("in step_once, level is not 0 or 1!");
  }

}


pub fn busy_wait(delay_us: u32) void {
  std.time.sleep(delay_us * 1000); // todo better

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
  pmem.positions[5].step_position = 204700;
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
  pmem.positions[11].step_position = 466534;
  pmem.positions[11].cm_position = 70.486500;
}


pub fn read_pmem_from_file() void {
  var fd = cfcntl.open("/mnt/usb1/pmem.bin", cfcntl.O_RDONLY);
  var read_success = false;
  if (fd >= 0) {
    var num_bytes_read = cunistd.read(fd, &pmem, @sizeOf(@TypeOf(pmem)));
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
  var p_hash = pmem_hash();
  if (p_hash == last_written_pmem_hash) {
    return;
  }
  var fd = cfcntl.open("/mnt/usb1/pmem.bin", cfcntl.O_RDWR | cfcntl.O_CREAT);
  if (fd >= 0) {
    _ = cunistd.write(fd, &pmem, @sizeOf(@TypeOf(pmem)));
    _ = cunistd.close(fd);
    std.debug.print("Wrote pmem to /mnt/usb1/pmem.bin\n", .{});
    last_written_pmem_hash = pmem_hash();
  }
}
