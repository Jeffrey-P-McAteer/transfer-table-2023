
#include <signal.h>
#include <stdio.h>

typedef void (*sighandler_t)(int);

#define PI_OUTPUT 1
#define PI_INPUT 0


int gpioInitialise() {
  return 0;
}

int gpioSetSignalFunc(int signum, sighandler_t handler) {
  signal(signum, handler);
  return 0;
}

int gpioSetMode(unsigned gpio, unsigned mode) {
  printf("no-op gpioSetMode(%d, %d)\n", gpio, mode);
  return 0;
}

int gpioWrite(unsigned gpio, unsigned level) {
  printf("no-op gpioWrite(%d, %d)\n", gpio, level);
  return 0;
}



