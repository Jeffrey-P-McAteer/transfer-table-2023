
#include <signal.h>

typedef void (*sighandler_t)(int);

void gpioSetSignalFunc(int signum, sighandler_t handler) {
  signal(signum, handler);
}




