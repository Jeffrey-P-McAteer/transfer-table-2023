
#define _GNU_SOURCE

#include <stdio.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <sys/resource.h>

void set_priority_and_cpu_affinity(int preferred_cpu, int priority) {
  // First off - set our affinity to preferred_cpu
  cpu_set_t  mask;
  CPU_ZERO(&mask);
  CPU_SET(preferred_cpu, &mask);
  int result = sched_setaffinity(0, sizeof(mask), &mask);
  if (result != 0) {
    printf("Error setting CPU affinity to processor %d: %s\n", preferred_cpu, strerror(errno));
  }
  // Then set our priority to -20, as high as possible
  result = setpriority(PRIO_PROCESS, 0, priority);
  if (result != 0) {
    printf("Error setting process priority to %d: %s\n", priority, strerror(errno));
  }
}
