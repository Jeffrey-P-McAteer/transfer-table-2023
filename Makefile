.PHONY: all run

all: gpio-motor-control

clean:
	rm gpio-motor-control

gpio-motor-control: gpio-motor-control.c
	gcc -Wall -Werror -g -o gpio-motor-control gpio-motor-control.c -lpigpio -lrt -lpthread

ultrasonic-sensor-playground: ultrasonic-sensor-playground.c
	gcc -Wall -Werror -g -o ultrasonic-sensor-playground ultrasonic-sensor-playground.c -lpigpio -lrt -lpthread

run: gpio-motor-control
	sudo ./gpio-motor-control

