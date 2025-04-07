.PHONY: all run camera-display

all: gpio-motor-control camera-display

clean:
	rm gpio-motor-control

gpio-motor-control: gpio-motor-control.zig
	# gcc -Wall -Werror -ffast-math -g -O2 -o gpio-motor-control gpio-motor-control.c -lm -lpigpio -lrt -lpthread
	sudo systemctl stop gpio-motor-control || true
	zig build
	cp ./zig-out/bin/gpio-motor-control gpio-motor-control
	echo "gpio-motor-control.service was stopped if it existed; execute 'systemctl start gpio-motor-control' to re-start the service."

camera-display:
	sudo systemctl stop camera-display.service || true
	cd camera-display && cargo build --release
	echo "camera-display.service was stopped if it existed; execute 'systemctl start camera-display' to re-start the service."

run: gpio-motor-control
	sudo systemctl stop gpio-motor-control || true
	sudo ./gpio-motor-control

