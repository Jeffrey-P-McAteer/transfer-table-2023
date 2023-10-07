/*
 * Taken and adapted from https://forums.raspberrypi.com/viewtopic.php?t=256740
 * 
 * git clone https://github.com/joan2937/pigpio
 * cd pigpio
 * sudo make install
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
#include <linux/types.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <string.h>
#include <sched.h>
#include <signal.h>

#include <pigpio.h>

#define PREFERRED_CPU 3

#define INPUT_DEV_FILE "/dev/input/event0"

#define MOTOR_ENABLE_PIN 15
#define MOTOR_DIRECTION_PIN 13
#define MOTOR_STEP_PIN 11

// see dip switches, this should match those numbers
#define PULSES_PER_REV 400

#define DELAY 0.0003

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

int keyboard_dev_fd = -1;

void signalHandler(int unused) {
    exit_requested = true;
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

void async_read_key_data() {
  if (keyboard_dev_fd >= 0) {

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


int main(int argc, char** argv)
{
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

  // Bind to SIGINT
  signal(SIGINT, signalHandler);

  if (!(gpioInitialise()>=0)) {
    printf("Error in gpioInitialise(), exiting!\n");
    return 1;
  }

  gpioSetMode(MOTOR_ENABLE_PIN, PI_OUTPUT);
  gpioWrite(MOTOR_ENABLE_PIN, 0);
  gpioSetMode(MOTOR_DIRECTION_PIN, PI_OUTPUT);
  gpioWrite(MOTOR_DIRECTION_PIN, 0);
  gpioSetMode(MOTOR_STEP_PIN, PI_OUTPUT);
  gpioWrite(MOTOR_STEP_PIN, 0);

  keyboard_dev_fd = open(INPUT_DEV_FILE, O_RDONLY);

  while (!exit_requested) {
    MS_SLEEP(5);
    printf("Tick!\n");

  }

  printf("Exiting cleanly...\n");
  gpioTerminate();
  return 0;
}
