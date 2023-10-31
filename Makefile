.PHONY: all run

all: gpio-motor-control

clean:
	rm gpio-motor-control

gpio-motor-control: gpio-motor-control.zig
	# gcc -Wall -Werror -ffast-math -g -O2 -o gpio-motor-control gpio-motor-control.c -lm -lpigpio -lrt -lpthread
	zig build-exe gpio-motor-control.zig

run: gpio-motor-control
	sudo ./gpio-motor-control

