# tinyloop

Real-time audio loopback using TinyAlsa. Routes audio from a
capture device to a playback device with a dual-threaded ring
buffer for reliable, low-latency operation.

## Tools

**tinyloop** — Capture → playback loopback with dual-threaded
ring buffer for reliable, low-latency operation.

**alsalist** — Probe available ALSA PCM devices.

## Dependencies

- libtinyalsa
- pthreads

## Build

    make

## Usage

### Discover devices

    ./alsalist             # all devices
    ./alsalist -P          # playback only
    ./alsalist -C          # capture only
    ./alsalist -c -P -C    # CSV output

### Run loopback

    ./tinyloop -i <capture_card:dev> -o <playback_card:dev>

Example:

    ./tinyloop -i 1:0 -o 0:0

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `-r` | 48000 | Sample rate (Hz) |
| `-c` | 2 | Channels |
| `-p` | 512 | Frames per period |
| `-n` | 8 | Number of periods |
| `-i` | — | Capture device (card:device, required) |
| `-o` | — | Playback device (card:device, required) |

Total ALSA buffer = period_size × periods = 4096 frames.

## How it works

Capture and playback run in separate threads, decoupled by a
thread-safe ring buffer. Each thread handles xruns independently
by preparing and restarting the PCM stream.

    capture PCM → pcm_readi → [ring buffer] → pcm_writei → playback PCM
        ↑                          ↑                          ↑
    capture thread           8192 B ring              playback thread
    (blocking read)        (mutex + condvar)         (non-blocking read)

On signal (Ctrl-C), the PCMs are stopped, threads joined, and
resources freed cleanly.
