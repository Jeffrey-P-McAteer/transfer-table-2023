all: gpio-motor-control

clean:
	rm gpio-motor-control

gpio-motor-control: gpio-motor-control.c
	gcc -g -o gpio-motor-control gpio-motor-control.c -lpigpio -lrt -lpthread



