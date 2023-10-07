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
#include <linux/types.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <string.h>
#include <sched.h>
#include <signal.h>

#include <pigpio.h>

#define PREFERRED_CPU 3

#define MOTOR_ENABLE_PIN 15
#define MOTOR_DIRECTION_PIN 13
#define MOTOR_STEP_PIN 11

// see dip switches, this should match those numbers
#define PULSES_PER_REV 400

#define DELAY 0.0003

// used to make gpioWrite calls nicer
#define LOW 0
#define HIGH 1
#define MOTOR_ENABLE_SIGNAL 0
#define MOTOR_DISABLE_SIGNAL 1
#define MOTOR_DIRECTION_FORWARD 1
#define MOTOR_DIRECTION_BACKWARD 0

// I don't usually use usec for measurement
#define MS_SLEEP(ms) usleep((useconds_t) (ms * 1000) )

static volatile bool exit_requested = false;

// We record the .code from struct input_event,
// incrementing keypress_code_i when current .code
// != the one in the cell.
// All codes are cleared during perform_keypresses().
#define NUM_KEYPRESS_CODES 16
__u16 keypress_codes[NUM_KEYPRESS_CODES];
int keypress_code_i = 0;

// we scan forward for /dev/input/eventN from 0 -> NUM_KEYBOARD_FDS-1
// values keyboard_dev_fds[N] < 0 are unused fds
#define NUM_KEYBOARD_FDS 8
int keyboard_dev_fds[NUM_KEYBOARD_FDS];

void motorControlSignalHandler(int unused) {
  printf("Caught signal %d!\n", unused);
  exit_requested = true;
}

#define WITH_STEPPER_ENABLED(do_stuff) do { \
    gpioWrite(MOTOR_ENABLE_PIN, MOTOR_ENABLE_SIGNAL); \
    do_stuff; \
    gpioWrite(MOTOR_ENABLE_PIN, MOTOR_DISABLE_SIGNAL); \
}while(0)

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

  }
  while (elapsed_tv.tv_usec < num_us && elapsed_tv.tv_sec == 0);
}

void step_once() {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);

  gpioWrite(MOTOR_STEP_PIN, HIGH);

  poll_until_us_elapsed(begin_tv, 100 /* 0.1ms wide square wave */);

  gpioWrite(MOTOR_STEP_PIN, LOW);

  poll_until_us_elapsed(begin_tv, 200 /* 0.1ms wide square wave */);
}

void step_forward() {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);
  
  gpioWrite(MOTOR_DIRECTION_PIN, MOTOR_DIRECTION_FORWARD);
  
  poll_until_us_elapsed(begin_tv, 100 /* 0.1ms wide square wave */);

  step_once();
}

void step_forward_n(int n) {
  for (int i=0; i<n; i+=1) {
    step_forward();
  }
}


void step_backward() {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);
  
  gpioWrite(MOTOR_DIRECTION_PIN, MOTOR_DIRECTION_BACKWARD);
  
  poll_until_us_elapsed(begin_tv, 100 /* 0.1ms wide square wave */);

  step_once();
}

void step_backward_n(int n) {
  for (int i=0; i<n; i+=1) {
    step_backward();
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

void async_read_key_data() {
  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    if (keyboard_dev_fds[i] >= 0) {
      // todo async read
      // enqueue_keypress(KEY_0) // or whatever
    }
  }
}

void perform_keypress(__u16 code) {
  if (code == KEY_0) {
    printf("Got KEY_0!\n");
  }
  else if (code == KEY_1) {
    printf("Got KEY_1!\n");
  }
  else if (code == KEY_2) {
    printf("Got KEY_2!\n");
  }
  
  else if (code == KEY_NUMERIC_0) {
    printf("Got KEY_NUMERIC_0!\n");
  }
  else if (code == KEY_NUMERIC_1) {
    printf("Got KEY_NUMERIC_1!\n");
  }
  else if (code == KEY_NUMERIC_2) {
    printf("Got KEY_NUMERIC_2!\n");
  }

  else if (code == KEY_KP0) {
    printf("Got KEY_KP0!\n");
  }
  else if (code == KEY_KP1) {
    printf("Got KEY_KP1!\n");
  }
  else if (code == KEY_KP2) {
    printf("Got KEY_KP2!\n");
  }
  else if (code == KEY_KPPLUS) {
    printf("Got KEY_KPPLUS!\n");
  }

}

void perform_keypresses() {
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

  gpioSetMode(MOTOR_ENABLE_PIN, PI_OUTPUT);
  gpioWrite(MOTOR_ENABLE_PIN, MOTOR_DISABLE_SIGNAL);
  gpioSetMode(MOTOR_DIRECTION_PIN, PI_OUTPUT);
  gpioWrite(MOTOR_DIRECTION_PIN, LOW);
  gpioSetMode(MOTOR_STEP_PIN, PI_OUTPUT);
  gpioWrite(MOTOR_STEP_PIN, LOW);

  for (int i=0; i<NUM_KEYBOARD_FDS; i+=1) {
    char input_dev_file[255] = { 0 };
    snprintf(input_dev_file, 254, "/dev/input/event%d", i);

    if (file_exists(input_dev_file)) {
      keyboard_dev_fds[i] = open(input_dev_file, O_RDONLY | O_NONBLOCK);
      printf("Opened \"%s\" as fd %d\n", input_dev_file, keyboard_dev_fds[i]);
    }
  }

  while (!exit_requested) {
    //MS_SLEEP(5);
    MS_SLEEP(250);
    printf("Tick!\n");
    async_read_key_data();
    perform_keypresses();

  }

  printf("Exiting cleanly...\n");
  gpioTerminate();
  return 0;
}
