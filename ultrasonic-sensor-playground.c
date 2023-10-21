
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

#define PREFERRED_CPU 2

// 23 is north-most (closest to ground), 24 is south-most pin (closest to USB ports)
#define GPIO_TRIGGER_PIN 23
#define GPIO_ECHO_PIN 24

// used to make gpioWrite calls nicer
#define LOW 0
#define HIGH 1

// Forward decs

// I don't usually use usec for measurement
#define MS_SLEEP(ms) usleep((useconds_t) (ms * 1000) )
#define US_SLEEP(us) usleep((useconds_t) us )

void niceExit(int exit_val) {
  gpioTerminate();
  exit(exit_val);
}

#define Z_OR_DIE(val) do { if (val != 0) { printf("%s:%d (%s): got %d when expecting 0, exiting!", __FILE__, __LINE__, __func__, val ); niceExit(1); } } while (0)

// returns the pulse's width in us
long read_distance_time_sync() {
  // send trigger
  Z_OR_DIE(gpioWrite(GPIO_TRIGGER_PIN, HIGH));
  US_SLEEP(10);
  Z_OR_DIE(gpioWrite(GPIO_TRIGGER_PIN, LOW));

  struct timeval begin_tv;
  struct timeval end_tv;

  // Record now_rv until until gpioRead(GPIO_ECHO_PIN) is no longer zero 0 (beginning of signal)
  while (gpioRead(GPIO_ECHO_PIN) == 0) {
    gettimeofday(&begin_tv,NULL);
  }
  // gpioRead(GPIO_ECHO_PIN) == 1

  while (gpioRead(GPIO_ECHO_PIN) == 1) {
    gettimeofday(&end_tv,NULL);
  }

  // begin_tv - end_tv in us is our time distance!

  return end_tv.tv_usec - begin_tv.tv_usec;
}

double convert_pulse_to_cm(long pulse_us) {
  // sound moves 34300 cm/s and we have us
  // divide by 2 b/c it moved to target and back
  double pulse_s = (double) pulse_us * 0.000001;
  return (pulse_s * 34300.0) / 2.0;
}


int main(int argc, char** argv) {
  bool exit_requested = false;
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

  Z_OR_DIE(gpioSetMode(GPIO_TRIGGER_PIN,    PI_OUTPUT));
  Z_OR_DIE(gpioSetMode(GPIO_ECHO_PIN,       PI_INPUT));
  
  Z_OR_DIE(gpioWrite(GPIO_TRIGGER_PIN, LOW));
  

  while (!exit_requested) {
    MS_SLEEP(1000);
    
    long pulse_time_us = read_distance_time_sync();
    double distance_cm = convert_pulse_to_cm(pulse_time_us);

    printf("Pulse width: %ld us\n", pulse_time_us);
    printf("Distance: %.2f cm\n", distance_cm);
    printf("\n");

  }

  printf("Exiting cleanly...\n");
  
  Z_OR_DIE(gpioWrite(GPIO_TRIGGER_PIN, LOW));
  
  gpioTerminate();
  return 0;
}
