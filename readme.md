
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

Rotary Dial Depress, '=' key:
    Save table position to last logical number typed in.

Escape key, tab key, '000' key, '.' key:
    Emergency stop. Table will halt within 400 steps of keypress.
    Table will not be operable from electronic controls for the next 2 seconds
    after stopping.

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

# Building

```bash
make gpio-motor-control

# To quickly run a test, stopping gpio-motor-control.service
make run

```

# Installing

```bash
cp gpio-motor-control.service /etc/systemd/system/gpio-motor-control.service
cp gpio-motor-control-watcher.path /etc/systemd/system/gpio-motor-control-watcher.path
cp gpio-motor-control-watcher.service /etc/systemd/system/gpio-motor-control-watcher.service

sudo systemctl enable --now gpio-motor-control
sudo systemctl enable --now gpio-motor-control-watcher

```

# Misc Research

 - Python controller + circuits: https://forums.raspberrypi.com/viewtopic.php?t=106916#p1357530
 - C controller: https://forums.raspberrypi.com/viewtopic.php?t=256740
 - https://www.omc-stepperonline.com/download/DM556T.pdf

# Command notes

```bash
# To attach my griffin dial to a /tmp/int_a for CV research
griffin-reader 'file_int_ex(50, "/tmp/int_a", lambda x: x - 1)' 'file_int_ex(50, "/tmp/int_a", lambda x: x + 1)' 'None'

# Even better - two numbers!
griffin-reader 'file_int_ex(50, get_g("F", "/tmp/int_a"), lambda x: x - 1)' 'file_int_ex(50, get_g("F", "/tmp/int_a"), lambda x: x + 1)' 'flip_g("F", "/tmp/int_a", "/tmp/int_b")'

# Research for image track detector
python image_correction_experiment.py research-photos/mpv-shot0001.jpg research-photos/mpv-shot0002.jpg research-photos/mpv-shot0003.jpg research-photos/mpv-shot0004.jpg research-photos/mpv-shot0010.jpg research-photos/mpv-shot0011.jpg research-photos/mpv-shot0012.jpg


```

