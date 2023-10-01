
# Experimental script from https://forums.raspberrypi.com/viewtopic.php?t=106916#p1357530

# yay -S python-rpi-gpio

#import RPi.GPIO as io
import RPi.GPIO as GPIO

GPIO.setmode(GPIO.BCM)
import sys, tty, termios, time

# This blocks of code defines the three GPIO
# pins used for the stepper motor
motor_enable_pin = 0
motor_direction_pin = 2
motor_step_pin = 3
delay = 3E-004              # By playing with this delay you can influence the rotational speed.
pulses_per_rev = 400        # This can be configured on the driver using the DIP-switches
GPIO.setup(motor_enable_pin, GPIO.OUT)
GPIO.setup(motor_direction_pin, GPIO.OUT)
GPIO.setup(motor_step_pin, GPIO.OUT)


# The getch method can determine which key has been pressed
# by the user on the keyboard by accessing the system files
# It will then return the pressed key as a variable
def getch():
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setraw(sys.stdin.fileno())
        ch = sys.stdin.read(1)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
    return ch

  
# This section of code defines the methods used to determine
# whether the stepper motor needs to spin forward or backwards. 
# Different directions are acheived by setting the
# direction GPIO pin to true or to false. 
# My driver required:
#   DIR must be ahead of PUL effective edge by 5 micro-s to ensure correct direction;
#   Pulse width not less than 2.5 micro-s;
#   Pulse low-level width not less than 2.5 micro-s.

def stepper_enable():
    GPIO.output(motor_enable_pin, False)

def stepper_disable():
    GPIO.output(motor_enable_pin, True)

def step_once():
    GPIO.output(motor_step_pin, True)
    time.sleep(delay)
    GPIO.output(motor_step_pin, False)
    time.sleep(delay)

def step_forward():
    GPIO.output(motor_direction_pin, True)
    time.sleep(delay)
    step_once()

def step_reverse():
    GPIO.output(motor_direction_pin, False)
    time.sleep(delay)
    step_once()

# Setting the stepper pins to false so the motors will not move
# until the user presses the first key
GPIO.output(motor_enable_pin, False)
GPIO.output(motor_step_pin, False)


# Print instructions for when the user has an interface
print("e/d: enable/disable")
print("f/r: step forward / reverse")
print("g/t: rotate forward / reverse")
print("x: exit")

# Infinite loop that will not end until the user presses the
# exit key
while True:
    # Keyboard character retrieval method is called and saved
    # into variable
    char = getch()
    print(char)

    # The stepper will be enabled when the "e" key is pressed
    if (char == "e"):
        stepper_enable()

    # The stepper will be disabled when the "d" key is pressed
    if (char == "d"):
        stepper_disable()

    # The "f" key will step the motor forward
    if (char == "f"):
        step_forward()

    # The "r" key will step the motor in reverse
    if (char == "r"):
        step_reverse()

    # The "g" key will step the motor 1 rotation forwards
    if (char == "g"):
        for x in range(0, pulses_per_rev):
            step_forward()

    # The "t" key will step the motor 1 rotation in reverse
    if (char == "t"):
        for x in range(0, pulses_per_rev):
            step_reverse()

    # The "x" key will break the loop and exit the program
    if (char == "x"):
        print("Program Ended")
        break

    # The keyboard character variable will be set to blank, ready
    # to save the next key that is pressed
    char = ""

# Program will cease all GPIO activity before terminating
GPIO.cleanup()
