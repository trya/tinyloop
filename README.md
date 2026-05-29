# tinyloop

Real-time audio loopback using TinyAlsa. Routes audio from a
capture device to a playback device with a dual-threaded ring
buffer for reliable, low-latency operation.

## Tools

**tinyloop** — Capture → playback loopback with dual-threaded
ring buffer for reliable, low-latency operation. Supports PCM
and IEC958 output (auto-detected or forced via `-f`).

**alsalist** — Probe available ALSA PCM devices. Retries with
IEC958 format when S16_LE fails (e.g. vc4-hdmi HDMI audio).

## Dependencies

- pthreads
- tinyalsa (included as git submodule)

## Build

    git submodule update --init
    make

## Install

    make install                     # default: /usr/local/bin
    make install PREFIX=/usr         # install to /usr/bin
    make install DESTDIR=/tmp/pkg    # staged install (for packaging)

## Usage

### Discover devices

    ./alsalist             # all devices
    ./alsalist -P          # playback only
    ./alsalist -C          # capture only
    ./alsalist -c -P -C    # CSV output

### Run loopback

    ./tinyloop -i <capture_card:dev> -o <playback_card:dev>

Example:

    ./tinyloop -i 1:0 -o 0:0       # S16_LE output
    ./tinyloop -i 1:0 -o 1:0       # auto IEC958 on vc4-hdmi
    ./tinyloop -i 1:0 -o 1:0 -f S16_LE  # force S16_LE
    ./tinyloop -i 1:0 -o 1:0 -f IEC958  # force IEC958

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `-r` | 48000 | Sample rate (Hz) |
| `-c` | 2 | Channels |
| `-p` | 512 | Frames per period |
| `-n` | 8 | Number of periods |
| `-f` | auto | Output format: `S16_LE` or `IEC958` (auto-detect or force) |
| `-i` | — | Capture device (card:device, required) |
| `-o` | — | Playback device (card:device, required) |

Total ALSA buffer = period_size × periods = 4096 frames.

## How it works

Capture and playback run in separate threads, decoupled by a
thread-safe ring buffer. Each thread handles xruns independently
by preparing and restarting the PCM stream.

When the output device requires IEC958_SUBFRAME_LE format (e.g.
Raspberry Pi vc4-hdmi), the playback thread converts S16_LE
samples to 32-bit IEC958 subframes before writing: each 16-bit
sample is left-justified in a 20-bit audio field with even parity
in bit 31 and V/U/C bits cleared for consumer PCM.

    capture PCM → pcm_readi → [ring buffer] → iec958_encode → pcm_writei → playback PCM
        ↑                          ↑               ↑                         ↑
    capture thread           8192 B ring      playback thread           (IEC958 if needed)
    (blocking read)        (mutex + condvar)   (non-blocking read)

On signal (Ctrl-C), the PCMs are stopped, threads joined, and
resources freed cleanly.
