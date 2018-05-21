# GPS Client
This `gps-client` is intended to push data to [`gps-server`](https://github.com/maxux/gps-server).

This project is composed of two part:
- `gps-gateway`: read data from serial GPS module and save it on sd-card, then forward it to a fifo
- `gps-push`: read from the fifo file and send data to the network (remote server)

## Hardware
This client is written and tested against **Adafruit Ultimate GPS Breakout (v3)**,
but it could works with any **NMEA 0183** compatible source.

## Workflow
Client is really simple and dumb, code is short and have no dependencies.

It's a kind of raw-forwarder from source to server.

### GPS Gateway
- Open local logfile (to keep data if something goes wrong, currently under `/mnt/backlog/gps-[index-incremented]`)
- Create and open /tmp/gps.pipe (in non-blocking mode)
- Read data from GPS Module:
  - Read new line from serial line
  - Check some basic consistancy
  - Append this line to local logfile and fifo

### GPS Push
- Try to reach remote server (`/api/ping`)
- Waits for server reply
- Initiate a new **session** (`/api/push/session`)
- Read data from FIFO:
  - Read new line from fifo file
  - Check some basic consistancy
  - Append this line to a « bundle-buffer » (batch-send)
  - When a `$GPRMC` frame is received, pushs bundle to server (`/api/push/datapoint`)

That's all. Server will receive plain raw-line from module directly, without pre-processing.

# Dependencies
This was tested with `glibc` and `gcc`, no more dependencies are needed.

Mostly everything is hard-coded rigth now (server, port, serial, etc.), this will change soon.

# Compilation
A simple `make` in the directory will produce `gps-gateway` and `gps-push` binaries.

To cross-compile, set compiler via `CC` variable to make (eg: `make CC=armv6j-hardfloat-linux-gnueabi-gcc`)

This was tested on linux `x86_64` and `armv6j-hardfloat-linux-gnueabi-gcc` successfully.
