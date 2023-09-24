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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <sys/time.h>

#include <pigpio.h>

#define L 1024
#define D 1000

unsigned m[][4]={
  {14,15,17,18},
  {27,22,23,24}
};

int b[8]={0b0001, 0b011, 0b0010, 0b0110, 0b0100, 0b1100, 0b1000, 0b1001};

int M=sizeof(m)/sizeof(m[0]);
int N=sizeof(m[0])/sizeof(m[0][0]);

void gpiosWrite(int *p, unsigned v)
{
  for(int i=0; i<N; ++i)
    gpioWrite( p[i], (v&(1<<i)) ? 1 : 0 );
}

void hstep(int mn, unsigned v)
{
  gpiosWrite(m[mn], b[v & 0x7]);
}

void hstep2(int x, int y)
{
  struct timeval begin_tv;
  gettimeofday(&begin_tv,NULL);

  hstep(0,x);
  hstep(1,y);
  
  //usleep(D);

  // We're no longer sleeping, we're polling!
  struct timeval now_tv;
  struct timeval elapsed_tv;
  do {
    gettimeofday(&now_tv,NULL);
    timersub(&now_tv, &begin_tv, &elapsed_tv);

  }
  while (elapsed_tv.tv_usec < D && elapsed_tv.tv_sec == 0);

}

int main(int argc, char** argv)
{
  int x=0;
  int y=0;

  if (!(gpioInitialise()>=0)) {
    printf("Error in gpioInitialise(), exiting!\n");
    return 1;
  }

  for (int i=0; i<M; ++i) {
    for (int j=0; j<N; ++j) {
      gpioSetMode(m[i][j], PI_OUTPUT);
      gpioWrite(m[i][j], 0);
    }
  }

  hstep2(x,y);

  for (int i=0; i<L/2; ++i) {
    hstep2(x,++y);
  }
  for (int i=0; i<L; ++i) {
    hstep2(x,--y);
  }
  for (int i=0; i<L/2; ++i) {
    hstep2(x,++y);
  }

  for (int i=0; i<L/2; ++i) {
    hstep2(++x,y);
  }
  for (int i=0; i<L; ++i) {
    hstep2(--x,y);
  }
  for (int i=0; i<L/2; ++i) {
    hstep2(++x,y);
  }
     
  for (int i=0; i<L/2; ++i) {
    hstep2(++x,++y);
  }
  for (int i=0; i<L; ++i) {
    hstep2(--x,--y);
  }
  for (int i=0; i<L/2; ++i) {
    hstep2(++x,++y);
  }
     
  for (int i=0; i<L/2; ++i) {
    hstep2(--x,++y);
  }
  for (int i=0; i<L; ++i) {
    hstep2(++x,--y);
  }
  for (int i=0; i<L/2; ++i) {
    hstep2(--x,++y);
  }
     
  gpioTerminate();
}
