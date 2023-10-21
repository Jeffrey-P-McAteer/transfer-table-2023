/*
 * Taken and adapted from https://forums.raspberrypi.com/viewtopic.php?t=256740
 * 
 * git clone https://github.com/joan2937/pigpio
 * cd pigpio
 * sudo make install
 * // Docs at https://abyz.me.uk/rpi/pigpio/cif.html
 * 
 * gcc -g -o gpio-motor-control gpio-motor-control.c -lpigpio -lrt -lpthread
 *
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/epoll.h> // for epoll_create1(), epoll_ctl(), struct epoll_event

#include <linux/types.h>
#include <linux/input.h> // for struct input_event
#include <linux/input-event-codes.h>

#include <errno.h>
#include <string.h>
#include <sched.h>
#include <signal.h>

#include <pigpio.h>

#define PREFERRED_CPU 3

#define MOTOR_ENABLE_PIN 22
#define MOTOR_DIRECTION_PIN 27
#define MOTOR_STEP_PIN 17

// 23 is north-most (closest to ground), 24 is south-most pin (closest to USB ports)
#define SONAR_TRIGGER_PIN 23
#define SONAR_ECHO_PIN 24


// see dip switches, this should match those numbers
//#define PULSES_PER_REV 400
#define PULSES_PER_REV (1600 * 5)

// 100 is approx 120s/12 positions
//#define DELAY_US 100
//#define DELAY_US 50
//#define DELAY_US 25
//#define DELAY_US 10 // Looks _real_ good
#define DELAY_US 1


// used to make gpioWrite calls nicer
#define LOW 0
#define HIGH 1
#define MOTOR_ENABLE_SIGNAL 0
#define MOTOR_DISABLE_SIGNAL 1
#define MOTOR_DIRECTION_FORWARD 1
#define MOTOR_DIRECTION_BACKWARD 0

// Poor man's enum
#define TABLE_MOVING_FORWARDS 2
#define TABLE_MOVING_BACKWARDS 1
#define TABLE_STOPPED 0

long table_state = TABLE_STOPPED;

// Forward decs

void async_read_key_data();
void enqueue_keypress(__u16 code);
void step_forward_n(int n);
void step_backward_n(int n);
void step_forward_n_eased(int n);
void step_backward_n_eased(int n);
void begin_sonar_read();


// I don't usually use usec for measurement
#define MS_SLEEP(ms) usleep((useconds_t) (ms * 1000) )

void niceExit(int exit_val) {
  gpioTerminate();
  exit(exit_val);
}

#define Z_OR_DIE(val) do { if (val != 0) { printf("%s:%d (%s): got %d when expecting 0, exiting!", __FILE__, __LINE__, __func__, val ); niceExit(1); } } while (0)

#define WITH_STEPPER_ENABLED(do_stuff) do { \
    Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN, MOTOR_ENABLE_SIGNAL)); \
    do_stuff; \
    Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN, MOTOR_DISABLE_SIGNAL)); \
    table_state = TABLE_STOPPED; \
} while(0)


volatile bool exit_requested = false;

volatile bool motor_stop_requested = false;

// when + or - pressed, step forward/backward this number of steps.
// When / pressed, divide by 2. When * pressed, multiply by 2.
volatile long num_pm_steps = PULSES_PER_REV;

// We record the .code from struct input_event,
// incrementing keypress_code_i when current .code
// != the one in the cell.
// All codes are cleared during perform_enqueued_keypresses().
#define NUM_KEYPRESS_CODES 16
__u16 keypress_codes[NUM_KEYPRESS_CODES];
int keypress_code_i = 0;

// we scan forward for /dev/input/eventN from 0 -> NUM_KEYBOARD_FDS-1
// values keyboard_dev_fds[N] < 0 are unused fds
#define NUM_KEYBOARD_FDS 24
int keyboard_dev_fds[NUM_KEYBOARD_FDS];

// Used to determine when a low signal should be sent
bool sonar_sending_trigger = false;
struct timeval sonar_trigger_begin_tv;

bool sonar_reading_echo_pin_pt1 = false;
struct timeval sonar_echo_begin_tv;
bool sonar_reading_echo_pin_pt2 = false;
struct timeval sonar_echo_end_tv;

bool sonar_bump_in_progress = false;
bool sonar_bump_may_occur = true;

// Update these with measured min/max values off sensor
//#define TABLE_BEGIN_CM 10.5
//#define TABLE_END_CM 68.5

// Super safe values to test safety
#define TABLE_BEGIN_CM 23.0
#define TABLE_END_CM 52.0

// (incorrect) Measured position offset from TABLE_BEGIN_CM (at one end of the table) to TABLE_END_CM
long last_sonar_pulse_us = 0;
double position_cm = (TABLE_END_CM - TABLE_BEGIN_CM) / 2.0; // assume center if no other data

double convert_pulse_to_cm(long pulse_us) {
  // sound moves 34300 cm/s and we have us
  // divide by 2 b/c it moved to target and back
  double pulse_s = (double) pulse_us * 0.000001;
  return (pulse_s * 34300.0) / 2.0;
}

// Called everywhere to update position_cm
void do_sonar_bookkeeping() {
  struct timeval now_tv;
  if (sonar_sending_trigger) {
    // Should we pull low b/c 10us have elapsed?
    gettimeofday(&now_tv,NULL);
    long elapsed_trigger_pulse_us = now_tv.tv_usec - sonar_trigger_begin_tv.tv_usec;
    if (elapsed_trigger_pulse_us >= 10) {
      gpioWrite(SONAR_TRIGGER_PIN, LOW);
      sonar_sending_trigger = false;
      gettimeofday(&sonar_echo_begin_tv,NULL);
      sonar_reading_echo_pin_pt1 = true;
    }
  }
  else {
    if (sonar_reading_echo_pin_pt1) {
      if (gpioRead(SONAR_ECHO_PIN) == 0) {
        gettimeofday(&sonar_echo_begin_tv,NULL);
      }
      else {
        sonar_reading_echo_pin_pt1 = false;
        sonar_reading_echo_pin_pt2 = true;
      }
    }
    if (sonar_reading_echo_pin_pt2) {
      if (gpioRead(SONAR_ECHO_PIN) == 1) {
        gettimeofday(&sonar_echo_end_tv,NULL);
      }
      else {
        // Echo ended!
        sonar_reading_echo_pin_pt2 = false;
        last_sonar_pulse_us = sonar_echo_end_tv.tv_usec - sonar_echo_begin_tv.tv_usec;
        position_cm = convert_pulse_to_cm(last_sonar_pulse_us);
        
        // We _usually_ don't do anything here; the rest of the program is probably currently
        // trying to move the motor. We do however check for safety, and ABORT whatever motor controls are happening ASAP.
        if (!sonar_bump_in_progress) {
          if (position_cm <= TABLE_BEGIN_CM && table_state == TABLE_MOVING_FORWARDS) {
            motor_stop_requested = true;
            printf("TABLE HAS HIT BEGINNING! Stopping motor!\n");
          }
          if (position_cm >= TABLE_END_CM && table_state == TABLE_MOVING_BACKWARDS) {
            motor_stop_requested = true;
            printf("TABLE HAS HIT END! Stopping motor!\n");
          }
        }

      }
    }
  }
}

// SAFETY: do not call within a WITH_STEPPER_ENABLED block!
void do_sonar_bumps() {
  if (!sonar_bump_may_occur) {
    return; // sonar_bump_may_occur is set when the user hits an emergency key
  }
  if (table_state == TABLE_STOPPED) {
    sonar_bump_in_progress = true;
    if (position_cm <= TABLE_BEGIN_CM) {
      // Bump forwards 1 rotation
      begin_sonar_read();
      printf("Table is still near beginning (%.2f cm), moving forward...\n", position_cm);
      WITH_STEPPER_ENABLED({
        table_state = TABLE_MOVING_BACKWARDS;
        step_backward_n_eased(1800);
        table_state = TABLE_STOPPED;
      });
    }
    else if (position_cm >= TABLE_END_CM) {
      // Bump forwards 1 rotation
      begin_sonar_read();
      printf("Table is still near end (%.2f cm), moving backward...\n", position_cm);
      WITH_STEPPER_ENABLED({
        table_state = TABLE_MOVING_FORWARDS;
        step_forward_n_eased(1800);
        table_state = TABLE_STOPPED;
      });
    }
  }
  sonar_bump_in_progress = false;
}


void begin_sonar_read() {
  if (sonar_sending_trigger || sonar_reading_echo_pin_pt1 || sonar_reading_echo_pin_pt2) {
    printf("Not beginning a sonar read b/c sonar_sending_trigger=%d, sonar_reading_echo_pin_pt1=%d, sonar_reading_echo_pin_pt2=%d.\n", sonar_sending_trigger, sonar_reading_echo_pin_pt1, sonar_reading_echo_pin_pt2);
    return;
  }
  gettimeofday(&sonar_trigger_begin_tv,NULL);
  gpioWrite(SONAR_TRIGGER_PIN, HIGH);
  sonar_sending_trigger = true;
  sonar_reading_echo_pin_pt1 = false;
  sonar_reading_echo_pin_pt2 = false;
}



void motorControlSignalHandler(int unused) {
  printf("Caught signal %d!\n", unused);
  exit_requested = true;
}

bool file_exists(char *filename) {
  struct stat buffer;   
  return (stat (filename, &buffer) == 0);
}

// Cannot handle num_us > 1_000_000!
/* eg
struct timeval begin_tv;
gettimeofday(&begin_tv,NULL);
// Work
poll_until_us_elapsed(begin_tv, 450)
*/
void poll_until_us_elapsed(struct timeval begin_tv, long num_us) {
  struct timeval now_tv;
  struct timeval elapsed_tv;
  do {
    gettimeofday(&now_tv,NULL);
    timersub(&now_tv, &begin_tv, &elapsed_tv);
    async_read_key_data();
    do_sonar_bookkeeping();
  }
  while (elapsed_tv.tv_usec < num_us && elapsed_tv.tv_sec == 0);
}

void step_once() {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);

  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN, HIGH));

  poll_until_us_elapsed(begin_tv, DELAY_US);
  if (motor_stop_requested) {
    return;
  }

  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN, LOW));

  poll_until_us_elapsed(begin_tv, 2 * DELAY_US);
}

void step_forward() {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);
  
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN, MOTOR_DIRECTION_FORWARD));
  
  step_once();
}

void step_forward_n(int n) {
  for (int i=0; i<n; i+=1) {
    step_forward();
    async_read_key_data();
    if (motor_stop_requested) {
      printf("step_forward_n exiting b/c motor_stop_requested == true\n");
      return;
    }
  }
}

void step_forward_eased(int delay_us) {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);
  
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN, MOTOR_DIRECTION_FORWARD));
  
  // step_once() w/ timing data

  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN, HIGH));

  poll_until_us_elapsed(begin_tv, delay_us);
  if (motor_stop_requested) {
    return;
  }

  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN, LOW));

  poll_until_us_elapsed(begin_tv, 2 * delay_us);

}

void step_forward_n_eased(int n) {
  int slowest_delay_us = 30;
  int fastest_delay_us = 1;
  
  for (int i=0; i<n; i+=1) {
    //double f = (double) i+1 / (double) n; // f goes from 0.0 -> 1.0
    int delay_us = 999;
    if (i < n/2) {
      // begin slow, ease UP to fastest_delay_us
      double f = (double) (i+1) / (double) (n/2); // f goes from 0.0 -> 1.0
      double inv_f = 1.0 - f;
      if (inv_f < 0.0) { inv_f = 0.0; }
      delay_us = (int) ( (f * (double) fastest_delay_us) + (inv_f * (double) slowest_delay_us) );
    }
    else {
      // begin slow, ease DOWN to slowest_delay_us
      double f = (double) ((i-(n/2))+1) / (double) n; // f goes from 0.0 -> 1.0
      double inv_f = 1.0 - f;
      if (inv_f < 0.0) { inv_f = 0.0; }
      delay_us = (int) ( (f * (double) slowest_delay_us) + (inv_f * (double) fastest_delay_us) );
    }

    // printf("n=%d i=%d delay_us=%d\n", n, i, delay_us);

    step_forward_eased(delay_us);

    async_read_key_data();
    
    if (motor_stop_requested) {
      printf("step_forward_n exiting b/c motor_stop_requested == true\n");
      return;
    }
  }
}


void step_backward() {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);
  
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN, MOTOR_DIRECTION_BACKWARD));
  
  // poll_until_us_elapsed(begin_tv, DELAY_US);

  step_once();
}

void step_backward_n(int n) {
  for (int i=0; i<n; i+=1) {
    step_backward();
    async_read_key_data();
    if (motor_stop_requested) {
      printf("step_backward_n exiting b/c motor_stop_requested == true\n");
      return;
    }
  }
}


void step_backward_eased(int delay_us) {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);
  
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN, MOTOR_DIRECTION_BACKWARD));
  
  // step_once() w/ timing data

  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN, HIGH));

  poll_until_us_elapsed(begin_tv, delay_us);
  if (motor_stop_requested) {
    return;
  }

  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN, LOW));

  poll_until_us_elapsed(begin_tv, 2 * delay_us);

}

void step_backward_n_eased(int n) {
  int slowest_delay_us = 30;
  int fastest_delay_us = 1;
  
  for (int i=0; i<n; i+=1) {
    //double f = (double) i+1 / (double) n; // f goes from 0.0 -> 1.0
    int delay_us = 999;
    if (i < n/2) {
      // begin slow, ease UP to fastest_delay_us
      double f = (double) (i+1) / (double) (n/2); // f goes from 0.0 -> 1.0
      double inv_f = 1.0 - f;
      if (inv_f < 0.0) { inv_f = 0.0; }
      delay_us = (int) ( (f * (double) fastest_delay_us) + (inv_f * (double) slowest_delay_us) );
    }
    else {
      // begin slow, ease DOWN to slowest_delay_us
      double f = (double) ((i-(n/2))+1) / (double) n; // f goes from 0.0 -> 1.0
      double inv_f = 1.0 - f;
      if (inv_f < 0.0) { inv_f = 0.0; }
      delay_us = (int) ( (f * (double) slowest_delay_us) + (inv_f * (double) fastest_delay_us) );
    }

    // printf("n=%d i=%d delay_us=%d\n", n, i, delay_us);

    step_backward_eased(delay_us);

    async_read_key_data();
    
    if (motor_stop_requested) {
      printf("step_forward_n exiting b/c motor_stop_requested == true\n");
      return;
    }
  }
}




void enqueue_keypress(__u16 code) {
  if (keypress_code_i < NUM_KEYPRESS_CODES) {
    keypress_codes[keypress_code_i] = code;
    keypress_code_i += 1;
  }
  else {
    keypress_codes[0] = code;
    keypress_code_i = 1;
  }
}

void immediate_keycode_perform(__u16 code) {
  if (code == 1 /* esc */ || code == 15 /* tab */ || code == 96 /* enter */) {
    motor_stop_requested = true;
    sonar_bump_may_occur = false;
    printf("Motor stop requested! (code=%d)\n", code);
  }
}

void async_read_key_data() {
  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    if (keyboard_dev_fds[i] >= 0) {
      struct input_event ev;
      ssize_t num_bytes_read = read(keyboard_dev_fds[i], &ev, sizeof(ev));
      if (num_bytes_read > 0) {
        if(ev.type == EV_KEY) { // https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/input-event-codes.h#n35
          // Is this up or down?
          // printf("ev.value = %d\n", ev.value);
          if (ev.value == 1) { // .value == 1 means key down, we observe this one first.
            enqueue_keypress(ev.code);
            immediate_keycode_perform(ev.code);
          }
        }
      }
    }
  }
}

void perform_keypress(__u16 code) {
  // Map numbers from _other_ keyboard numbers to KEY_KP0
  if (code == KEY_0 || code == KEY_NUMERIC_0) {
    code = KEY_KP0;
  }
  else if (code == KEY_1 || code == KEY_NUMERIC_1) {
    code = KEY_KP1;
  }
  else if (code == KEY_2 || code == KEY_NUMERIC_2) {
    code = KEY_KP2;
  }
  else if (code == KEY_3 || code == KEY_NUMERIC_3) {
    code = KEY_KP3;
  }
  else if (code == KEY_4 || code == KEY_NUMERIC_4) {
    code = KEY_KP4;
  }
  else if (code == KEY_5 || code == KEY_NUMERIC_5) {
    code = KEY_KP5;
  }
  else if (code == KEY_6 || code == KEY_NUMERIC_6) {
    code = KEY_KP6;
  }
  else if (code == KEY_7 || code == KEY_NUMERIC_7) {
    code = KEY_KP7;
  }
  else if (code == KEY_8 || code == KEY_NUMERIC_8) {
    code = KEY_KP8;
  }
  else if (code == KEY_9 || code == KEY_NUMERIC_9) {
    code = KEY_KP9;
  }
  
  // Now handle key presses
  if (code == KEY_KP0) {
    printf("Got KEY_KP0!\n");
  }
  else if (code == KEY_KP1) {
    printf("Got KEY_KP1!\n");
  }
  else if (code == KEY_KP2) {
    printf("Got KEY_KP2!\n");
  }
  else if (code == KEY_KP3) {
    printf("Got KEY_KP3!\n");
  }
  else if (code == KEY_KP4) {
    printf("Got KEY_KP4!\n");
  }
  else if (code == KEY_KP5) {
    printf("Got KEY_KP5!\n");
  }
  else if (code == KEY_KP6) {
    printf("Got KEY_KP6!\n");
  }
  else if (code == KEY_KP7) {
    printf("Got KEY_KP7!\n");
  }
  else if (code == KEY_KP8) {
    printf("Got KEY_KP8!\n");
  }
  else if (code == KEY_KP9) {
    printf("Got KEY_KP9!\n");
  }
  else if (code == KEY_KPPLUS) {
    printf("Got KEY_KPPLUS, step_forward_n(%ld)!\n", num_pm_steps);
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_FORWARDS;
      step_forward_n_eased(num_pm_steps);
      table_state = TABLE_STOPPED;
    });
  }
  else if (code == KEY_KPMINUS) {
    printf("Got KEY_KPMINUS, step_backward_n(%ld)!\n", num_pm_steps);
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_BACKWARDS;
      step_backward_n_eased(num_pm_steps);
      table_state = TABLE_STOPPED;
    });
  }
  else if (code == 98) { // '/' on keypad
    num_pm_steps /= 2;
    if (num_pm_steps < 2) {
      num_pm_steps = 2;
    }
  }
  else if (code == 55) { // '*' on keypad
    num_pm_steps *= 2;
    if (num_pm_steps > PULSES_PER_REV * 16) { // allow up to 16 revs
      num_pm_steps = PULSES_PER_REV * 16;
    }
  }
  else if (code == 1 /* esc */ || code == 15 /* tab */ || code == 96 /* enter */) {
    motor_stop_requested = true;
    printf("Motor stop requested! (code=%d)\n", code);
  }
  else if (code != 0) {
    printf("Got unknown key, %d!\n", code);
  }

}

void perform_enqueued_keypresses() {
  for (keypress_code_i=0; keypress_code_i < NUM_KEYPRESS_CODES; keypress_code_i += 1) {
    perform_keypress(keypress_codes[keypress_code_i]);
    keypress_codes[keypress_code_i] = 0;
  }
  keypress_code_i = 0;
}


int main(int argc, char** argv) {

  exit_requested = false;
  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    keyboard_dev_fds[i] = -1;
  }

  // First off - set our affinity to PREFERRED_CPU
  cpu_set_t  mask;
  CPU_ZERO(&mask);
  CPU_SET(PREFERRED_CPU, &mask);
  int result = sched_setaffinity(0, sizeof(mask), &mask);
  if (result != 0) {
    printf("Error setting CPU affinity to processor %d: %s\n", PREFERRED_CPU, strerror(errno));
  }
  // Then set our priority to -20, as high as possible
  result = setpriority(PRIO_PROCESS, 0, -20);
  if (result != 0) {
    printf("Error setting process priority to -20: %s\n", strerror(errno));
  }

  if (!(gpioInitialise()>=0)) {
    printf("Error in gpioInitialise(), exiting!\n");
    return 1;
  }

  // Bind to SIGINT + SIGTERM; cannot use signal() directly b/c 
  // pigpio assumes it can bind to OS events for machine safety reasons
  gpioSetSignalFunc(SIGINT, motorControlSignalHandler);
  gpioSetSignalFunc(SIGTERM, motorControlSignalHandler);

  // Initialize GPIOs
  Z_OR_DIE(gpioSetMode(MOTOR_ENABLE_PIN,     PI_OUTPUT));
  Z_OR_DIE(gpioSetMode(MOTOR_DIRECTION_PIN,  PI_OUTPUT));
  Z_OR_DIE(gpioSetMode(MOTOR_STEP_PIN,       PI_OUTPUT));
  Z_OR_DIE(gpioSetMode(SONAR_TRIGGER_PIN,    PI_OUTPUT));
  Z_OR_DIE(gpioSetMode(SONAR_ECHO_PIN,       PI_INPUT));

  Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL));
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN,  LOW));
  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN,       LOW));
  Z_OR_DIE(gpioWrite(SONAR_TRIGGER_PIN,    LOW));

  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    char input_dev_file[255] = { 0 };
    snprintf(input_dev_file, 254, "/dev/input/event%d", i);

    if (file_exists(input_dev_file)) {
      keyboard_dev_fds[i] = open(input_dev_file, O_RDONLY | O_NONBLOCK);
      printf("Opened \"%s\" as fd %d\n", input_dev_file, keyboard_dev_fds[i]);
    }
  }

  long loop_i = 0;
  struct timeval loop_now_tv;
  while (!exit_requested) {

    gettimeofday(&loop_now_tv,NULL);
    poll_until_us_elapsed(loop_now_tv, 1000); // 1ms delay

    async_read_key_data();
    perform_enqueued_keypresses();
    if (loop_i % 250 == 0) { // Approx 4x a second, begin reads to update table global position data
      begin_sonar_read();
      if (loop_i % 1000 == 0) {
        printf("last position_cm = %.3f\n", position_cm);
      }
    }
    do_sonar_bookkeeping();
    motor_stop_requested = false;
    do_sonar_bumps();
    loop_i += 1;
  }

  printf("Exiting cleanly...\n");
  
  Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN,     MOTOR_DISABLE_SIGNAL));
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN,  LOW));
  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN,       LOW));
  Z_OR_DIE(gpioWrite(SONAR_TRIGGER_PIN,    LOW));

  gpioTerminate();
  return 0;
}
