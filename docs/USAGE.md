# Kernel Noise — listen to your Linux system

A small experimental instrument that turns Linux activity into live sound.

Run it, listen, then open an application, move files or start a compile in another terminal. Switch between the layers. Hear what changes on your machine.

```bash
./listen.sh
```

The first run compiles a small C program. Later runs start directly. No root, Python packages, accounts or sample downloads are needed. Start with your device volume low.

## Install

You need **Linux, Bash, a C compiler and an audio player**. If you already have those, just run the command above.

On Debian, Ubuntu or Raspberry Pi OS, this is a simple starting point:

```bash
sudo apt update
sudo apt install build-essential alsa-utils
./listen.sh
```

That installs a compiler and ALSA's `aplay`. If your desktop uses PipeWire, `pw-cat` is another option (`pipewire-bin` on Debian/Ubuntu). The launcher also supports PulseAudio's `paplay` and FFmpeg's `ffplay`.

The launcher chooses an installed player and uses your default audio output. `sudo` is only for package installation, not running the instrument.

## Play

| Key | What it does |
| --- | --- |
| `1` | CPU drone |
| `2` | Interrupt-rate percussion |
| `3` | Scheduler/context-switch-rate percussion |
| `4` | All three |
| `m` | Cycle through the modes |
| `+` / `-` | Raise / lower the instrument's volume |
| Space | Mute / unmute |
| `q` or Ctrl+C | Stop |

The controls menu displays the selected mode in **bold** and a dimmed percentage beside `+/- volume`. The percentage refers to the instrument's gain range of `0..0.4` (not the operating system volume).

The terminal displays the current mode, CPU utilization and counter rates. Keyboard controls require a terminal; command-line options work without one.

Start with one layer:

```bash
./listen.sh --mode cpu
```

Record while listening:

```bash
./listen.sh --record my-machine.wav --duration 20
```

Save a live capture as a WAV without playback:

```bash
./listen.sh --render my-machine.wav --duration 20
```

Both produce stereo 48 kHz, 16-bit PCM WAVs. Existing files are never overwritten. Rendering still takes the requested time because it observes the machine while running.

## What am I hearing?

| Observed activity | Sound |
| --- | --- |
| CPU utilization | Drone pitch, amplitude and FM depth |
| Total interrupt rate | Pulse density in one percussive voice |
| Context-switch rate | Pulse density in a second percussive voice |

The data comes from `/proc/stat`, sampled roughly every 50 ms. The two percussion layers adapt to a slowly changing local reference, so a quiet machine and a busy one can both have audible detail. Pitch changes are smoothed and oscillator phases continue between audio blocks.

These are **sampled counter rates**. The synthesized pulses do not represent the exact arrival times of individual IRQs or scheduler events. This demo is a listening frame with chosen mappings, not a kernel event trace. Its own audio processing also contributes to the activity it observes.

You can read and change the complete instrument in [`src/listen.c`](../src/listen.c). All sound is synthesized locally; the EP's field recordings and vocal samples are not part of this demo.

## If there is no sound

First check what's available:

```bash
./listen.sh --check
```

Then choose a player explicitly:

```bash
./listen.sh --backend pw-cat
./listen.sh --backend aplay
./listen.sh --backend ffplay
```

`--check` verifies dependencies, not the sound hardware. If an installed player cannot connect to your audio system, select another backend. The launcher reports a playback error instead of silently switching mid-session.

If CPU mode works but a percussion layer is silent, check the displayed rate. A container or quiet system may expose zero deltas. The demo leaves that layer silent rather than inventing events.

On a Raspberry Pi, select the HDMI/USB output you actually use. List ALSA outputs with `aplay -L`, then, for example:

```bash
./listen.sh --backend aplay --device default
```

Replace `default` with a PCM name from that list if needed. No analog headphone output is assumed. On SSH, playback happens on the remote machine; use `--render` to make a file if it has no speakers.

More options: `./listen.sh --help`.

## Try it, change it

Listen to the CPU layer while doing something ordinary in another terminal. Compare a browser launch with a compile, or compare two machines doing similar work. Changing modes can help you hear activity that the mix obscures.

If you share a take, include the machine, workload, selected mode and any code changes. Twenty seconds is enough for an interesting comparison. You can open an issue or discussion in the repository once it is published.

## Related work

- [Kernel Noise blog](https://kernelnoise.com/)
- [Recordings on Bandcamp](https://kernelnoise.bandcamp.com/)
- [Idle](https://kernelnoise.bandcamp.com/track/idle), the first released track
- [Listening to Silence: From John Cage to the Linux Kernel](https://kernelnoise.com/archives/1067)

The recordings combine kernel-derived sound with sampling and arrangement. This smaller instrument lets you explore the kernel-generated material on your own system.

## Development

```bash
make
make test
```

Python 3 is used only for development tests. Core playback needs C, the standard C library, `libm`, Bash and one player. A GitHub Actions workflow builds/tests on Ubuntu; an ARM64 cross-compile step checks that the source builds for AArch64.

Actual testing performed for this release is documented in [`docs/VALIDATION.md`](VALIDATION.md).

## License and credits

MIT license; see [`LICENSE`](../LICENSE). This license covers the demo code and its included documentation, not the separate Bandcamp recordings or samples used in them.

Project concept and artistic direction: Jihèd Chaibi / Kernel Noise.

Pay attention to the noise.
