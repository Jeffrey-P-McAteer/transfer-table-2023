.PHONY: all run

all: gpio-motor-control

clean:
	rm gpio-motor-control

gpio-motor-control: gpio-motor-control.c
	gcc -Wall -Werror -ffast-math -g -o gpio-motor-control gpio-motor-control.c -lm -lpigpio -lrt -lpthread

run: gpio-motor-control
	sudo ./gpio-motor-control

