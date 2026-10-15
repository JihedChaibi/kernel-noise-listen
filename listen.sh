#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

help() {
    cat <<'TEXT'
Kernel Noise — listen to your Linux system.

  ./listen.sh
  ./listen.sh --mode cpu
  ./listen.sh --record take.wav --duration 20
  ./listen.sh --render take.wav --duration 20   # save without playback
  ./listen.sh --check                          # check dependencies; no sound

Options:
  --mode cpu|irq|scheduler|mix  default mix
  --volume 0..0.4              default 0.15
  --duration SECONDS           stop automatically (up to one hour)
  --record FILE.wav            record while listening; won't overwrite
  --render FILE.wav            record without a player; needs --duration
  --backend auto|pw-cat|paplay|aplay|ffplay|null
  --device NAME                ALSA PCM name, with --backend aplay
  --no-ui                      no keyboard/status display

Keys: 1-4 modes, m cycle, +/- volume, space mute, q quit, Ctrl+C stop.
Linux, a C compiler, and one audio player are needed. No Python packages.
TEXT
}

backend=auto
device=default
device_set=0
check=0
render=0
engine_args=()

while (($#)); do
    case "$1" in
        --help|-h)
            help
            exit 0
            ;;
        --check)
            check=1
            shift
            ;;
        --no-ui)
            engine_args+=("$1")
            shift
            ;;
        --backend|--device|--mode|--volume|--duration|--record|--render)
            if (($# < 2)); then
                echo "Missing value for $1" >&2
                exit 2
            fi

            case "$1" in
                --backend) backend=$2 ;;
                --device) device=$2; device_set=1 ;;
                --render) render=1; engine_args+=("$1" "$2") ;;
                *) engine_args+=("$1" "$2") ;;
            esac
            shift 2
            ;;
        *)
            echo "Unknown option: $1. Try ./listen.sh --help" >&2
            exit 2
            ;;
    esac
done

if [[ $(uname -s) != Linux || ! -r /proc/stat ]]; then
    echo 'This demo needs Linux with readable /proc/stat.' >&2
    exit 1
fi

case "$backend" in
    auto|pw-cat|paplay|aplay|ffplay|null) ;;
    *) echo "Unknown backend: $backend" >&2; exit 2 ;;
esac

if ((device_set)) && [[ $backend != aplay ]]; then
    echo '--device requires --backend aplay.' >&2
    exit 2
fi

# Build only when the source is newer, so ./listen.sh stays a single command.
mkdir -p .build
binary=.build/kernel-noise
if [[ ! -x $binary || src/listen.c -nt $binary ]]; then
    compiler=${CC:-cc}
    if ! command -v "$compiler" >/dev/null; then
        echo 'Install a C compiler. Debian/Ubuntu/Pi: sudo apt install build-essential' >&2
        exit 1
    fi

    echo 'Building the listening instrument...' >&2
    "$compiler" -std=c11 -O2 -Wall -Wextra -Wpedantic \
        src/listen.c -lm -o ".build/kernel-noise-$$"
    mv ".build/kernel-noise-$$" "$binary"
fi

if ((render)); then
    exec "$binary" "${engine_args[@]}"
fi

# Prefer the active sound server before falling back to ALSA or FFplay.
if [[ $backend == auto ]]; then
    if command -v pw-cat >/dev/null && [[ -S ${XDG_RUNTIME_DIR:-/nonexistent}/pipewire-0 ]]; then
        backend=pw-cat
    elif command -v paplay >/dev/null && [[ -S ${XDG_RUNTIME_DIR:-/nonexistent}/pulse/native || -n ${PULSE_SERVER:-} ]]; then
        backend=paplay
    elif command -v aplay >/dev/null; then
        backend=aplay
    elif command -v ffplay >/dev/null; then
        backend=ffplay
    elif command -v pw-cat >/dev/null; then
        backend=pw-cat
    elif command -v paplay >/dev/null; then
        backend=paplay
    else
        echo 'No audio player found. Debian/Ubuntu/Pi: sudo apt install alsa-utils' >&2
        echo 'Or install pipewire-bin, pulseaudio-utils or ffmpeg for your audio system.' >&2
        exit 1
    fi
fi

if [[ $backend != null ]] && ! command -v "$backend" >/dev/null; then
    echo "$backend not found. See README.md for installation options." >&2
    exit 1
fi

if ((check)); then
    printf 'Linux /proc: readable\nEngine: built\nSelected player: %s\n' "$backend"
    echo 'This checks availability, not whether your sound device is working.'
    exit 0
fi

case "$backend" in
    pw-cat)
        player=(pw-cat --playback --raw --format=s16 --rate=48000 --channels=2 --latency=50ms -)
        ;;
    paplay)
        player=(paplay --raw --format=s16le --rate=48000 --channels=2 --latency-msec=50)
        ;;
    aplay)
        player=(aplay -q -D "$device" -t raw -f S16_LE -r 48000 -c 2 --buffer-time=100000)
        ;;
    ffplay)
        # FFplay 7 renamed -ac to -ch_layout. Support both generations.
        ffplay_options=$(ffplay -hide_banner -h demuxer=s16le 2>&1 || true)
        if [[ $ffplay_options == *-ch_layout* ]]; then
            player=(ffplay -nodisp -autoexit -loglevel error -f s16le -ar 48000 -ch_layout stereo -i pipe:0)
        else
            player=(ffplay -nodisp -autoexit -loglevel error -f s16le -ar 48000 -ac 2 -i pipe:0)
        fi
        ;;
    null)
        player=(cat)
        ;;
esac

# A FIFO lets the C engine and audio player run independently.
tmp=$(mktemp -d)
engine_pid=
player_pid=

cleanup() {
    trap - EXIT INT TERM HUP

    [[ -z $engine_pid ]] || kill -TERM "$engine_pid" 2>/dev/null || true
    [[ -z $player_pid ]] || kill -TERM "$player_pid" 2>/dev/null || true
    [[ -z $engine_pid ]] || wait "$engine_pid" 2>/dev/null || true
    [[ -z $player_pid ]] || wait "$player_pid" 2>/dev/null || true
    rm -rf "$tmp"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
mkfifo "$tmp/audio"

echo "Player: $backend. Start with your device volume low." >&2
"$binary" "${engine_args[@]}" >"$tmp/audio" &
engine_pid=$!
"${player[@]}" <"$tmp/audio" >/dev/null &
player_pid=$!

set +e
wait "$engine_pid"; engine_status=$?
wait "$player_pid"; player_status=$?
set -e

if ((engine_status || player_status)); then
    echo 'Playback stopped with an error. Try --check or choose another --backend; see README.md.' >&2
    exit 1
fi
