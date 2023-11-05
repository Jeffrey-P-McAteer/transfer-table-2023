
# Transfer Table 2023

Small project using a `Rasperry Pi Model B v1.2` (the 2015 edition) to
control a medium-sized stepper motor.

# Pi Setup

See [`install-notes.md`](install-notes.md).

Attach to logs with `sudo journalctl -f -u gpio-motor-control`

# Software Control

Uses a keypad or QWERTY keyboard, and a rotary dial.

```
Rotary Dial Clockwise = Move table towards 0 position
Rotary dial Counter-Clockwise = Move table towards 12 position

Escape key, tab key, '000' key, '.' key:
    Emergency stop. Table will halt within 400 steps of keypress.

Type in a number and hit enter:

Number from [1,12]:
    Move table from any current position to target position
    (supports table starting from half-position)
    Table will be at position N for programming purposes after enter key is struck,
    even if emergency stop is activated.

Number == 90:
    Reset all table position data to initial values.
    !!! THIS MUST BE DONE WITH THE TABLE POSITIONED AT TRACK 1 !!!
    Table will be at position 1 after enter key is struck.

Number from [1001, 1800]:
    Set number of steps/tick for rotary dial to Number - 1000.
    Eg typing in 1200 will set rotary dial to move 200 steps/tick
    instead of the default of 100 steps/tick.

```

# Misc Research

 - Python controller + circuits: https://forums.raspberrypi.com/viewtopic.php?t=106916#p1357530
 - C controller: https://forums.raspberrypi.com/viewtopic.php?t=256740
 - https://www.omc-stepperonline.com/download/DM556T.pdf



