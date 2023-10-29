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
#include <stdint.h>

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

//#define RAMP_UP_STEPS 5200
#define RAMP_UP_STEPS 6400

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


typedef void (*DirectionedStepFunc)(int delay_us);

#define NUM_POSITIONS 12
typedef struct PosDat {
  long steps_from_0;
  double cm_from_0_expected;

} PosDat;


// The pmem struct holds persistent-memory data, and is read/written off of a file
// on startup + modifications.
typedef struct Pmem {
  int position;
  long num_pm_steps;
  PosDat position_data[NUM_POSITIONS];
  long table_steps_from_0; // changed whenever we move the table, by any means.

} __attribute__((packed)) Pmem;

Pmem pmem; // and this is the global variable that holds persistent memory
long last_written_pmem_hash = -1;

// type in a number from 1001 -> 2000 to assign 1 -> 1000 to this.
int dial_num_steps_per_click = 100;


long pmem_hash(Pmem* p) {
  long h = 0;
  h += p->position;
  h += p->num_pm_steps * 128;
  for (int i=0; i<NUM_POSITIONS; i+=1) {
    h += ( p->position_data[i].steps_from_0 * 1024 );
  }
  h += p->table_steps_from_0 * 2048;
  return h;
}

void read_pmem_from_file() {
  int fd = open("/mnt/usb1/pmem.bin", O_RDONLY);
  if (fd < 0) {
    printf("Error opening pmem file: %d %s\n", errno, strerror(errno));
    pmem.position = 0;
    pmem.num_pm_steps = PULSES_PER_REV;
    
    pmem.position_data[0].steps_from_0 = 0;
    pmem.position_data[0].cm_from_0_expected = 13.509912;
    pmem.position_data[1].steps_from_0 = 40600;
    pmem.position_data[1].cm_from_0_expected = 18.856425;
    pmem.position_data[2].steps_from_0 = 81700;
    pmem.position_data[2].cm_from_0_expected = 23.028162;
    pmem.position_data[3].steps_from_0 = 122500;
    pmem.position_data[3].cm_from_0_expected = 27.684387;
    pmem.position_data[4].steps_from_0 = 163300;
    pmem.position_data[4].cm_from_0_expected = 31.834687;
    pmem.position_data[5].steps_from_0 = 204700;
    pmem.position_data[5].cm_from_0_expected = 37.309825;
    pmem.position_data[6].steps_from_0 = 244544;
    pmem.position_data[6].cm_from_0_expected = 44.778650;
    pmem.position_data[7].steps_from_0 = 303840;
    pmem.position_data[7].cm_from_0_expected = 49.246225;
    pmem.position_data[8].steps_from_0 = 344164;
    pmem.position_data[8].cm_from_0_expected = 54.348350;
    pmem.position_data[9].steps_from_0 = 385244;
    pmem.position_data[9].cm_from_0_expected = 61.122600;
    pmem.position_data[10].steps_from_0 = 425700;
    pmem.position_data[10].cm_from_0_expected = 67.073650;
    pmem.position_data[11].steps_from_0 = 466534;
    pmem.position_data[11].cm_from_0_expected = 70.486500;

    pmem.table_steps_from_0 = 0; // On first run TABLE MUST BE AT 0!
  }
  else {
    read(fd, &pmem, sizeof(pmem));
    close(fd);
  }
  last_written_pmem_hash = pmem_hash(&pmem);
  printf("Read pmem:\n");
  printf("  pmem.position = %d;\n", pmem.position);
  printf("  pmem.num_pm_steps = %ld;\n", pmem.num_pm_steps);
  for (int i=0; i<NUM_POSITIONS; i+=1) {
    printf("  pmem.position_data[%d].steps_from_0 = %ld;\n", i, pmem.position_data[i].steps_from_0);
    printf("  pmem.position_data[%d].cm_from_0_expected = %f;\n", i, pmem.position_data[i].cm_from_0_expected);
  }
  printf("  pmem.table_steps_from_0 = %ld;\n", pmem.table_steps_from_0);
  printf("\n");
}

void write_pmem_to_file_iff_diff() {
  long hash = pmem_hash(&pmem);
  if (hash != last_written_pmem_hash) {
    int fd = open("/mnt/usb1/pmem.bin", O_RDWR | O_CREAT);
    if (fd < 0) {
      printf("Error opening pmem file: %d %s\n", errno, strerror(errno));
      return;
    }
    write(fd, &pmem, sizeof(pmem));
    close(fd);
    last_written_pmem_hash = pmem_hash(&pmem);
  }
}

void async_read_key_data();
void enqueue_keypress(__u16 code);
//void step_forward_n(int n);
//void step_backward_n(int n);
void step_n_eased(int n, int ramp_up_end_n, DirectionedStepFunc step_func);
void begin_sonar_read();
void step_forward_eased(int delay_us);
void step_backward_eased(int delay_us);

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


void move_to_position(int pos_num) {
  printf("Moving to position %d (index %d)...\n", pos_num, pos_num-1);
  pos_num = pos_num-1; // Go from human number to index number
  if (pos_num < 0 || pos_num > NUM_POSITIONS) {
    printf("%d is an invalid position number at the moment! (0 to %d allowed!)\n", pos_num, NUM_POSITIONS);
    return;
  }
  /*if (pmem.position == pos_num) {
    printf("Already at %d!\n", pmem.position);
    return; // we're there!
  }*/
  int pos_delta = pmem.position - pos_num;

  printf("Moving %d logical steps from %d!\n", pos_delta, pmem.position);

  //long num_steps_to_move = pmem->position_data[pmem.position].steps_from_0 - pmem->position_data[pos_num].steps_from_0;

  // We use the actual table steps now, so a manual move won't leave the table not knowing where it is.
  long num_steps_to_move = pmem.table_steps_from_0 - pmem.position_data[pos_num].steps_from_0;

  printf("pmem.table_steps_from_0 = %ld - pmem.position_data[pos_num].steps_from_0 = %ld = %ld \n", pmem.table_steps_from_0, pmem.position_data[pos_num].steps_from_0, num_steps_to_move);

  printf("Sending abs(%ld) steps to motor in direction of magnitude\n", num_steps_to_move);

  if (num_steps_to_move < 0) {
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_BACKWARDS;
      step_n_eased(llabs(num_steps_to_move), RAMP_UP_STEPS, step_backward_eased);
      table_state = TABLE_STOPPED;
    });
  }
  else if (num_steps_to_move > 0) {
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_FORWARDS;
      step_n_eased(llabs(num_steps_to_move), RAMP_UP_STEPS, step_forward_eased);
      table_state = TABLE_STOPPED;
    });
  }
  
  // Even if we're emergency-stopped, record where we think we are.
  pmem.position = pos_num;

}








// We record the .code from struct input_event,
// incrementing keypress_code_i when current .code
// != the one in the cell.
// All codes are cleared during perform_enqueued_keypresses().
#define NUM_KEYPRESS_CODES 16
__u16 keypress_codes[NUM_KEYPRESS_CODES];
int keypress_code_i = 0;

// we scan forward for /dev/input/eventN from 0 -> NUM_KEYBOARD_FDS-1
// values keyboard_dev_fds[N] < 0 are unused fds
#define NUM_KEYBOARD_FDS 52
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

// We record here & write averages to position_cm; trade-off a little delay (.5s) for higher accuracy.
#define NUM_POSITION_CM_HIST 4
double position_cm_hist[NUM_POSITION_CM_HIST];
int position_cm_hist_i = 0;

int safety_smallest_us = 1; // If a step ever happens in less than this many us, set the step delay to this many us.

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
      
      if (position_cm_hist_i >= NUM_POSITION_CM_HIST) {
        position_cm_hist_i = 0;
      }
      position_cm_hist[position_cm_hist_i] = convert_pulse_to_cm(last_sonar_pulse_us);
      position_cm_hist_i += 1;

      // Calc average
      position_cm = 0.0;
      for (int i=0; i<NUM_POSITION_CM_HIST; i+=1) {
        position_cm += position_cm_hist[i];
      }
      position_cm /= (double) NUM_POSITION_CM_HIST;

      /*
      safety_smallest_us = 0; // reset here, always adjust below
      double dist_to_begin = fabs(position_cm - pmem.position_data[0].cm_from_0_expected);
      if (dist_to_begin < 8.0) { // Begin applying a speed limiting force
        safety_smallest_us = (int) ((8.0 - dist_to_begin) * 10.0);
      }
      double dist_to_end = fabs(pmem.position_data[NUM_POSITIONS-1].cm_from_0_expected - position_cm);
      if (dist_to_end < 8.0) { // Begin applying a speed limiting force
        safety_smallest_us = (int) ((8.0 - dist_to_end) * 10.0);
      }

      if (safety_smallest_us < 0) {
        safety_smallest_us = 0;
      }
      else if (safety_smallest_us > 100) {
        safety_smallest_us = 100;
      }
      */

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


// Used to record keys to an int before getting <enter>
int num_input_buffer = 0;

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
  uint32_t poll_i = 0;
  async_read_key_data(); // Guarantee that we run this _at_least_ once, even if loop below terminates fast
  do {
    gettimeofday(&now_tv,NULL);
    timersub(&now_tv, &begin_tv, &elapsed_tv);
    do_sonar_bookkeeping();
    if (poll_i % 900 == 0) {
      async_read_key_data();
    }
    poll_i += 1;
  }
  while (elapsed_tv.tv_usec < num_us && elapsed_tv.tv_sec == 0);

  // printf("num_us = %ld poll_i = %d\n", num_us, poll_i); // Debugging todo rm me
  // Saw values like
  //    num_us = 1000 poll_i = 1811

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

  pmem.table_steps_from_0 -= 1;
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

  if (delay_us < safety_smallest_us) {
    delay_us = safety_smallest_us;
  }

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

  pmem.table_steps_from_0 -= 1;

}

void step_n_eased(int n, int ramp_up_end_n, DirectionedStepFunc step_func) {
#define EXIT_IF_STOP_REQ() { async_read_key_data(); \
    if (motor_stop_requested) { \
      printf("step_n_eased exiting b/c motor_stop_requested == true\n"); \
      return; \
    } }

  // 1 is as fast we we'll be bothering to measure, 30 is too fast for a begin ramp-up
  int slowest_us = 400;
  int fastest_us = 78;
  
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
    begin_sonar_read();
  }

  // Constant speed @ fastest_us
  for (int i=ramp_up_end_n; i<ramp_down_begin_n; i+=1) {
    /*if (i % 40 == 0) {
      printf("[constant] fastest_us = %d i = %d  \n", fastest_us, i);
    }// */
    step_func(fastest_us);
    EXIT_IF_STOP_REQ();
    begin_sonar_read();
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
    begin_sonar_read();
  }
#undef EXIT_IF_STOP_REQ
}


void step_backward() {
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);
  
  Z_OR_DIE(gpioWrite(MOTOR_DIRECTION_PIN, MOTOR_DIRECTION_BACKWARD));
  
  // poll_until_us_elapsed(begin_tv, DELAY_US);

  step_once();

  pmem.table_steps_from_0 += 1;
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

  if (delay_us < safety_smallest_us) {
    delay_us = safety_smallest_us;
  }

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

  pmem.table_steps_from_0 += 1;

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
  //if (code == 1 /* esc */ || code == 15 /* tab */ || code == 96 /* enter */) {
  if (code == 1 /* esc */ || code == 15 /* tab */ || code == 51 /* 000 key */ || code == 83 /* decimal place */) {
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
        close(keyboard_dev_fds[i]);
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


void perform_num_input_buffer(int num) {
  if (num >= 1 && num <= 12) {
    move_to_position(num);
  }
  else if (num >= 1001 && num <= 2000) {
    dial_num_steps_per_click = num - 1000;
    printf("user typed in %d, so set dial_num_steps_per_click=%d \n", num, dial_num_steps_per_click);
    if (dial_num_steps_per_click <= 1) {
      dial_num_steps_per_click = 1;
    }
    if (dial_num_steps_per_click >= 1000) {
      dial_num_steps_per_click = 1000;
    }
  }
  else {
    printf("Got un-used number in, num=%d!\n", num);
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
    num_input_buffer *= 10;
    num_input_buffer += 0;
  }
  else if (code == KEY_KP1) {
    num_input_buffer *= 10;
    num_input_buffer += 1;
  }
  else if (code == KEY_KP2) {
    num_input_buffer *= 10;
    num_input_buffer += 2;
  }
  else if (code == KEY_KP3) {
    num_input_buffer *= 10;
    num_input_buffer += 3;
  }
  else if (code == KEY_KP4) {
    num_input_buffer *= 10;
    num_input_buffer += 4;
  }
  else if (code == KEY_KP5) {
    num_input_buffer *= 10;
    num_input_buffer += 5;
  }
  else if (code == KEY_KP6) {
    num_input_buffer *= 10;
    num_input_buffer += 6;
  }
  else if (code == KEY_KP7) {
    num_input_buffer *= 10;
    num_input_buffer += 7;
  }
  else if (code == KEY_KP8) {
    num_input_buffer *= 10;
    num_input_buffer += 8;
  }
  else if (code == KEY_KP9) {
    num_input_buffer *= 10;
    num_input_buffer += 9;
  }
  else if (code == KEY_KPPLUS) { // forward == towards 99999, by wall.
    printf("Got KEY_KPPLUS, stepping backwards %ld !\n", pmem.num_pm_steps);
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_BACKWARDS;
      step_n_eased(pmem.num_pm_steps, RAMP_UP_STEPS, step_backward_eased);
      table_state = TABLE_STOPPED;
    });
  }
  else if (code == KEY_KPMINUS) { // backward == towards 0, by work table
    printf("Got KEY_KPMINUS, stepping forwards %ld !\n", pmem.num_pm_steps);
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_FORWARDS;
      step_n_eased(pmem.num_pm_steps, RAMP_UP_STEPS, step_forward_eased);
      table_state = TABLE_STOPPED;
    });
  }
  else if (code == 98) { // '/' on keypad
    pmem.num_pm_steps /= 2;
    if (pmem.num_pm_steps < 2) {
      pmem.num_pm_steps = 2;
    }
  }
  else if (code == 55) { // '*' on keypad
    pmem.num_pm_steps *= 2;
    if (pmem.num_pm_steps > PULSES_PER_REV * 16) { // allow up to 16 revs
      pmem.num_pm_steps = PULSES_PER_REV * 16;
    }
  }
  //else if (code == 1 /* esc */ || code == 15 /* tab */ || code == 96 /* enter */) {
  else if (code == 1 /* esc */ || code == 15 /* tab */ || code == 51 /* 000 key */ || code == 83 /* decimal place */) {
    motor_stop_requested = true;
    printf("Motor stop requested! (code=%d)\n", code);
  }
  else if (code == KEY_BACKSPACE || code == 14 /* backspace */ || code == KEY_EQUAL || code == 113 /* push */) {
    //printf("Got Backspace, TODO enter assignment for next number input! (code=%d)\n", code);
    if (pmem.position >= 0 && pmem.position < NUM_POSITIONS) {
      pmem.position_data[pmem.position].steps_from_0 = pmem.table_steps_from_0;
      pmem.position_data[pmem.position].cm_from_0_expected = position_cm;
      printf("Saving: \n");
      printf("pmem.position_data[%d].steps_from_0 = %ld\n", pmem.position, pmem.table_steps_from_0);
      printf("pmem.position_data[%d].cm_from_0_expected = %f\n", pmem.position, position_cm);
    }
    write_pmem_to_file_iff_diff();
  }
  else if (code == 96 /* enter */) {
    // Turn number buffer into an int, move position based on int.
    perform_num_input_buffer(num_input_buffer);
    // Back to beginning
    num_input_buffer = 0;
    write_pmem_to_file_iff_diff();
  }
  else if (code == 115 /* clockwise */) {
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_FORWARDS;
      for (int x=0; x<dial_num_steps_per_click; x+=1) {
        step_forward_eased(120);
      }
      table_state = TABLE_STOPPED;
    });
  }
  else if (code == 114 /* counter-clockwise */) {
    WITH_STEPPER_ENABLED({
      table_state = TABLE_MOVING_BACKWARDS;
      for (int x=0; x<dial_num_steps_per_click; x+=1) {
        step_backward_eased(120);
      }
      table_state = TABLE_STOPPED;
    });
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

  read_pmem_from_file();

  long loop_i = 0;
  struct timeval loop_now_tv;

  // We also measure the initial table distance for 4 seconds & use that to assign pmem.position
  // on the assumption the table may have moved while powered off.
  begin_sonar_read();
  gettimeofday(&loop_now_tv,NULL);
  poll_until_us_elapsed(loop_now_tv, 4000 * 1000); /* 4,000ms == 4s */

  printf("TODO assign position_cm!\n");
  // position_cm;


  while (!exit_requested) {

    gettimeofday(&loop_now_tv,NULL);
    poll_until_us_elapsed(loop_now_tv, 1000); // 1ms delay between high-level keypress stuff

    async_read_key_data();
    
    perform_enqueued_keypresses(); // This function blocks to perform user-requested tasks!

    if (loop_i % 100 == 0) { // Approx 10x a second, begin reads to update table global position data
      begin_sonar_read();
      if (loop_i % 4500 == 0) {
        printf("last position_cm = %.3f pmem.table_steps_from_0 = %ld\n", position_cm, pmem.table_steps_from_0);
      }
    }
    if (loop_i % 2000 == 0) { // Approx every 2s, open new keyboards.
      open_input_event_fds();
    }
    if (loop_i % 6000 == 250) {
      write_pmem_to_file_iff_diff();
    }

    do_sonar_bookkeeping(); // this is unecessary but why not? Just for fun, make it easier to guarantee the state machine never stops.

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
