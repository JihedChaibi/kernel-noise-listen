# Validation — 2026-10-15

Executed in the Linux x86-64 development container:

- C11 build with GCC-compatible `cc`, `-Wall -Wextra -Wpedantic -Werror`: passed.
- C fixture tests: CPU tick accounting (including guest-field exclusion), interrupt/context-switch rates, memory availability, counter-reset handling and absent `/proc` inputs: passed.
- Nine Python integration tests: passed. These cover all four modes, exact PCM/WAV frame counts, bounded default output level, rendering without a player, existing-file preservation, invalid arguments, the launcher's FIFO pipeline, simulated player failure, bold selected-mode display in the controls menu, dimmed volume percentage next to `+/- volume` (before/after adjustment), live keyboard controls and terminal mode restoration.
- Real `/proc/stat` observations were used for the live integration tests. Zero event deltas may validly produce silence in a percussion layer; tests do not require invented activity.
- FFplay backend exercised with `SDL_AUDIODRIVER=dummy`: accepted the live raw-PCM stream and exited normally after the requested duration.
- AddressSanitizer and UndefinedBehaviorSanitizer instrumented build exercised with a short live WAV render: passed with leak detection disabled because LeakSanitizer cannot inspect all container tasks here.
- `bash -n listen.sh`: passed.

Test on available hardware report (2026-10-15):

OKn my Linux desktop debian 13 (trixie) : OK
RaspberryPi 3B (RaspberryPi OS) : OK

Not established in this container:

- Physical speaker/headphone output, independently assessed sound quality, or actual end-to-end audio latency.
- PipeWire, PulseAudio and ALSA connections to real sound hardware. Their command lines follow the projects' documented interfaces; the player-failure test checks propagation, not actual device compatibility. FFplay was tested with an SDL dummy audio device only.
- Independent Raspberry Pi OS installation or ARM execution tests in this container. An ARM64 cross-compile check is configured in GitHub Actions but has not run in this session.
- Recovery under severe system overload, audio-device hotplug, or every terminal emulator.

## Useful host reports

Add the date, OS, CPU/board, selected player, physical output, result of mode/mute/quit controls, and recording result after testing locally. Preserve distinctions between compiling for a platform and actually listening on it.

## Interface references

- PipeWire `pw-cat`: https://docs.pipewire.org/page_man_pw-cat_1.html
- ALSA `aplay`: https://manpages.debian.org/bookworm/alsa-utils/aplay.1.en.html
- FFplay: https://ffmpeg.org/ffplay.html
