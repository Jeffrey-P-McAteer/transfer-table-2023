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
#include <math.h>

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

typedef void (*DirectionedStepFunc)(int delay_us);

void async_read_key_data();
void enqueue_keypress(__u16 code);
//void step_forward_n(int n);
//void step_backward_n(int n);
void step_n_eased(int n, int ramp_up_end_n, DirectionedStepFunc step_func);
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
#define NUM_KEYBOARD_FDS 32
int keyboard_dev_fds[NUM_KEYBOARD_FDS];

// Used to determine when a low signal should be sent
bool           sonar_sending_trigger = false;
struct timeval sonar_trigger_begin_tv;

bool           sonar_reading_echo_pin_pt1 = false;
struct timeval sonar_echo_begin_tv;
bool           sonar_reading_echo_pin_pt2 = false;
struct timeval sonar_echo_end_tv;

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
  else if (sonar_reading_echo_pin_pt1) {
    if (gpioRead(SONAR_ECHO_PIN) == 0) {
      gettimeofday(&sonar_echo_begin_tv,NULL);
    }
    else {
      sonar_reading_echo_pin_pt1 = false;
      sonar_reading_echo_pin_pt2 = true;
    }

  }
  else if (sonar_reading_echo_pin_pt2) {
    if (gpioRead(SONAR_ECHO_PIN) == 1) {
      gettimeofday(&sonar_echo_end_tv,NULL);
    }
    else {
      // Echo ended!
      sonar_reading_echo_pin_pt2 = false;
      last_sonar_pulse_us = sonar_echo_end_tv.tv_usec - sonar_echo_begin_tv.tv_usec;
      position_cm = convert_pulse_to_cm(last_sonar_pulse_us);
      
      double dist_to_begin = position_cm - TABLE_BEGIN_CM;
      if (dist_to_begin < 15.0) { // Begin applying a speed limiting force
        
      }
      double dist_to_end = TABLE_END_CM - position_cm;
      if (dist_to_end < 15.0) { // Begin applying a speed limiting force
        
      }

    }
  }
}

void begin_sonar_read() {
  if (sonar_sending_trigger || sonar_reading_echo_pin_pt1 || sonar_reading_echo_pin_pt2) {
    // printf("Not beginning a sonar read b/c sonar_sending_trigger=%d, sonar_reading_echo_pin_pt1=%d, sonar_reading_echo_pin_pt2=%d.\n", sonar_sending_trigger, sonar_reading_echo_pin_pt1, sonar_reading_echo_pin_pt2);
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

void step_n_eased(int n, int ramp_up_end_n, DirectionedStepFunc step_func) {
#define EXIT_IF_STOP_REQ() { async_read_key_data(); \
    if (motor_stop_requested) { \
      printf("step_n_eased exiting b/c motor_stop_requested == true\n"); \
      return; \
    } }

  // 1 is as fast we we'll be bothering to measure, 30 is too fast for a begin ramp-up
  int slowest_us = 400;
  int fastest_us = 32;
  
  // For very short steps, limit top speed & change ramp up bounds.
  if (n < ramp_up_end_n) {
    ramp_up_end_n = n / 2;
    fastest_us = 100; // TODO calculate ideal off N + some math
  }
  int ramp_down_begin_n = n - ramp_up_end_n;
  double slow_fast_us_dist = ((double) slowest_us - (double) fastest_us);
  double half_pi = M_PI / 2.0;
  double wavelength = (M_PI) / ((double) ramp_up_end_n); // formula is actually 2pi/wavelength, but I want to double ramp_up_end_n so instead removed the existing 2.0.

  // Ramp up on a sinusoid
  for (int i=0; i<ramp_up_end_n; i+=1) {
    
    double delay_us_d = ((double) fastest_us) + (
      slow_fast_us_dist * (sin((wavelength * (double) i) + half_pi) + 1.0)
    );

    /*if (i % 10 == 0) {
      printf("[ramp up] delay_us_d = %.2f i = %d  \n", delay_us_d, i);
      printf("[ramp up fn] %.2f = %d + (%.3f * (sin((%.3f * %d) + (pi/2) ) + 1.0)   \n", delay_us_d, fastest_us, slow_fast_us_dist, wavelength, i );
    }// */

    int delay_us = (int) delay_us_d;
    if (delay_us <= 0) {
      delay_us = 1; // fastest possible
    }
    step_func(delay_us);
    EXIT_IF_STOP_REQ();
  }

  // Constant speed @ fastest_us
  for (int i=ramp_up_end_n; i<ramp_down_begin_n; i+=1) {
    /*if (i % 40 == 0) {
      printf("[constant] fastest_us = %d i = %d  \n", fastest_us, i);
    }// */
    step_func(fastest_us);
    EXIT_IF_STOP_REQ();
  }

  // Ramp down on a sinusoid
  for (int i=ramp_down_begin_n; i<n; i+=1) {
    int j = n - i; // j goes in reverse of i, use same fn for delay amounts

    double delay_us_d = ((double) fastest_us) + (
      slow_fast_us_dist * (sin((wavelength * (double) j) + half_pi) + 1.0)
    );

    /*if (i % 10 == 0) {
      printf("[ramp down] delay_us_d = %.2f i = %d j = %d  \n", delay_us_d, i, j);
      printf("[ramp down fn] %.2f = %d + (%.3f * (sin((%.3f * %d) + (pi/2) ) + 1.0)   \n", delay_us_d, fastest_us, slow_fast_us_dist, wavelength, j );
    }// */

    int delay_us = (int) delay_us_d;
    if (delay_us <= 0) {
      delay_us = 1; // fastest possible
    }
    step_func(delay_us);
    EXIT_IF_STOP_REQ();
  }
#undef EXIT_IF_STOP_REQ
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
    printf("Motor stop requested! (code=%d)\n", code);
  }
}

void async_read_key_data() {
  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    if (keyboard_dev_fds[i] >= 0) {
      struct input_event ev;
      ssize_t num_bytes_read = read(keyboard_dev_fds[i], &ev, sizeof(ev));
      if (num_bytes_read == -1 && errno != 11 /* 11 means data not here, poll again */) {
        printf("Keyboard read error: %d %s\n", errno, strerror(errno));
        keyboard_dev_fds[i] = -1;
        continue;
      }
      if (num_bytes_read > 0) {
        if(ev.type == EV_KEY) { // https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/input-event-codes.h#n35
          // Is this up or down?
          // printf("ev.value = %d\n", ev.value);
          if (ev.value == 1) { // .value == 1 means key down, we observe this one first.
            immediate_keycode_perform(ev.code);
            enqueue_keypress(ev.code);
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
      step_n_eased(num_pm_steps, 5200, step_forward_eased);
      table_state = TABLE_STOPPED;
    });
  }
  else if (code == KEY_KPMINUS) {
    printf("Got KEY_KPMINUS, step_backward_n(%ld)!\n", num_pm_steps);
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_BACKWARDS;
      step_n_eased(num_pm_steps, 5200, step_backward_eased);
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

void open_input_event_fds() {
  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    if (keyboard_dev_fds[i] < 0) {
      char input_dev_file[255] = { 0 };
      snprintf(input_dev_file, 254, "/dev/input/event%d", i);

      if (file_exists(input_dev_file)) {
        keyboard_dev_fds[i] = open(input_dev_file, O_RDONLY | O_NONBLOCK);
        printf("Opened \"%s\" as fd %d\n", input_dev_file, keyboard_dev_fds[i]);
      }
    }
  }
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

  open_input_event_fds();

  long loop_i = 0;
  struct timeval loop_now_tv;
  while (!exit_requested) {

    gettimeofday(&loop_now_tv,NULL);
    poll_until_us_elapsed(loop_now_tv, 1000); // 1ms delay between high-level keypress stuff

    async_read_key_data();
    
    perform_enqueued_keypresses(); // This function blocks to perform user-requested tasks!

    if (loop_i % 100 == 0) { // Approx 10x a second, begin reads to update table global position data
      begin_sonar_read();
      if (loop_i % 4000 == 0) {
        printf("last position_cm = %.3f\n", position_cm);
      }
    }
    if (loop_i % 2000 == 0) { // Approx every 2s, open new keyboards.
      open_input_event_fds();
    }

    do_sonar_bookkeeping();

    motor_stop_requested = false;
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
