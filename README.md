# GPS Client

This `gps-client` is intended to push data to [`gps-server`](https://github.com/maxux/gps-server).

## Hardware

This client is written and tested against **Adafruit Ultimate GPS Breakout (v3)**,
but it could works with any **NMEA 0183** compatible source.

## Workflow

Client is really simple and dumb, code is short and have no dependencies.

It's a kind of raw-forwarder from source to server.

- Open local logfile (to keep data if something goes wrong, currently under `/mnt/backlog/gps-[timestamp]`)
- Try to reach remote server (`/api/ping`)
- Waits for server reply
- Initiate a new **session** (`/api/push/session`)
- Read data from GPS Module:
  - Read new line from serial line
  - Check some basic consistancy
  - Append this line to a « bundle-buffer » (batch-send)
  - When a `$GPRMC` frame is received, pushs bundle to server (`/api/push/datapoint`)

That's all. Server will receive plain raw-line from module directly, without pre-processing.

# Dependencies

This was tested with `glibc` and `gcc`, no more dependencies are needed.

# Compilation

A simple `make` in the directory will produce `gps-client` binary.

To cross-compile, set compiler via `CC` variable to make (eg: `make CC=armv6j-hardfloat-linux-gnueabi-gcc`)

This was tested on linux `x86_64` and `armv6j-hardfloat-linux-gnueabi-gcc` successfully.
