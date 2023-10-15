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

//#define MOTOR_ENABLE_PIN 15
//#define MOTOR_DIRECTION_PIN 13
//#define MOTOR_STEP_PIN 11
//#define MOTOR_ENABLE_PIN 3
//#define MOTOR_DIRECTION_PIN 2
//#define MOTOR_STEP_PIN 0
#define MOTOR_ENABLE_PIN 22
#define MOTOR_DIRECTION_PIN 27
#define MOTOR_STEP_PIN 17

// see dip switches, this should match those numbers
//#define PULSES_PER_REV 400
#define PULSES_PER_REV (1600 * 5)

#define DELAY_US 100


// used to make gpioWrite calls nicer
#define LOW 0
#define HIGH 1
#define MOTOR_ENABLE_SIGNAL 0
#define MOTOR_DISABLE_SIGNAL 1
#define MOTOR_DIRECTION_FORWARD 1
#define MOTOR_DIRECTION_BACKWARD 0

// Forward decs

void async_read_key_data();


// I don't usually use usec for measurement
#define MS_SLEEP(ms) usleep((useconds_t) (ms * 1000) )

void niceExit(int exit_val) {
  gpioTerminate();
  exit(exit_val);
}

#define Z_OR_DIE(val) do { if (val != 0) { printf("%s:%d (%s): got %d when expecting 0, exiting!", __FILE__, __LINE__, __func__, val ); niceExit(1); } } while (0)

static volatile bool exit_requested = false;

static volatile bool motor_stop_requested = false;

// when + or - pressed, step forward/backward this number of steps.
// When / pressed, divide by 2. When * pressed, multiply by 2.
static volatile long num_pm_steps = PULSES_PER_REV;

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

void motorControlSignalHandler(int unused) {
  printf("Caught signal %d!\n", unused);
  exit_requested = true;
}

#define WITH_STEPPER_ENABLED(do_stuff) do { \
    Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN, MOTOR_ENABLE_SIGNAL)); \
    do_stuff; \
    Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN, MOTOR_DISABLE_SIGNAL)); \
} while(0)

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
  
  // poll_until_us_elapsed(begin_tv, DELAY_US);

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
  if (code == 1 || code == 15) {
    motor_stop_requested = true;
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
      step_forward_n(num_pm_steps);
    });
  }
  else if (code == KEY_KPMINUS) {
    printf("Got KEY_KPMINUS, step_backward_n(%ld)!\n", num_pm_steps);
    WITH_STEPPER_ENABLED({
      step_backward_n(num_pm_steps);
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
  else if (code == 1 /* esc */ || code == 15 /* tab */) {
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


  Z_OR_DIE(gpioSetMode(MOTOR_ENABLE_PIN,    PI_OUTPUT));
  Z_OR_DIE(gpioSetMode(MOTOR_DIRECTION_PIN, PI_OUTPUT));
  Z_OR_DIE(gpioSetMode(MOTOR_STEP_PIN,      PI_OUTPUT));

  Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN, MOTOR_DISABLE_SIGNAL));
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN,  LOW));
  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN,       LOW));

  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    char input_dev_file[255] = { 0 };
    snprintf(input_dev_file, 254, "/dev/input/event%d", i);

    if (file_exists(input_dev_file)) {
      keyboard_dev_fds[i] = open(input_dev_file, O_RDONLY | O_NONBLOCK);
      printf("Opened \"%s\" as fd %d\n", input_dev_file, keyboard_dev_fds[i]);
    }
  }

  while (!exit_requested) {
    MS_SLEEP(1);
    async_read_key_data();
    perform_enqueued_keypresses();
    motor_stop_requested = false;
  }

  printf("Exiting cleanly...\n");
  
  Z_OR_DIE(gpioWrite(MOTOR_ENABLE_PIN, MOTOR_DISABLE_SIGNAL));
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN, LOW));
  Z_OR_DIE(gpioWrite(MOTOR_STEP_PIN, LOW));

  gpioTerminate();
  return 0;
}
