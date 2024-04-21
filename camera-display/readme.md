
# Camera Display

This is a rust equivelant of the camera rail detection logic from `webserver.py`.

## Useful One-Liners

```bash
cargo build --release && sudo timeout 10 ./target/release/camera-display ; sudo chvt 1

sudo systemctl stop camera-display.service && cargo build --release && sudo systemctl start camera-display.service

```

