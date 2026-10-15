/* Kernel Noise // Listen — Linux activity as a small sound instrument. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define BLOCK 480
#define TAU 6.2831853071795864769
#define MAX_VOLUME 0.4
#define VOLUME_STEP 0.025

/* These are cumulative kernel counters, not timestamps of individual events. */
typedef struct {
    uint64_t total, idle, intr, ctxt;
    double memory;
    int memory_ok, intr_ok, ctxt_ok;
} Metrics;

typedef struct {
    double cpu, irq_rate, ctx_rate, memory, freq;
    int memory_ok, irq_ok, ctx_ok;
} Controls;

static const char *mode_names[] = {"cpu", "irq", "scheduler", "mix"};
static volatile sig_atomic_t stopping;

static int tty_fd = -1;
static int tty_saved;
static struct termios original_tty;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void signal_stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static void restore_tty(void)
{
    if (tty_saved)
        tcsetattr(tty_fd, TCSANOW, &original_tty);

    if (tty_fd >= 0) {
        close(tty_fd);
        tty_fd = -1;
    }
}

static void open_keyboard(void)
{
    tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    if (tty_fd < 0 || tcgetattr(tty_fd, &original_tty) != 0)
        return;

    struct termios tty = original_tty;
    tty.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(tty_fd, TCSANOW, &tty) == 0)
        tty_saved = 1;
}

static int read_metrics(const char *root, Metrics *m)
{
    char path[1024];
    char line[1024];
    unsigned long long ticks[10] = {0};
    int got_cpu = 0;

    memset(m, 0, sizeof(*m));
    snprintf(path, sizeof(path), "%s/stat", root);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "cpu ", 4)) {
            int n = sscanf(line + 4,
                           "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                           &ticks[0], &ticks[1], &ticks[2], &ticks[3], &ticks[4],
                           &ticks[5], &ticks[6], &ticks[7], &ticks[8], &ticks[9]);

            if (n >= 4) {
                /* Guest time is already included in user/nice; avoid double-counting. */
                for (int i = 0; i < n && i < 8; i++)
                    m->total += ticks[i];

                m->idle = ticks[3] + ticks[4];
                got_cpu = 1;
            }
        } else if (!strncmp(line, "intr ", 5)) {
            unsigned long long value;
            if (sscanf(line + 5, "%llu", &value) == 1) {
                m->intr = value;
                m->intr_ok = 1;
            }
        } else if (!strncmp(line, "ctxt ", 5)) {
            unsigned long long value;
            if (sscanf(line + 5, "%llu", &value) == 1) {
                m->ctxt = value;
                m->ctxt_ok = 1;
            }
        }
    }
    fclose(f);

    snprintf(path, sizeof(path), "%s/meminfo", root);
    f = fopen(path, "r");
    unsigned long long total = 0, available = 0;
    int got_available = 0;

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "MemTotal:", 9))
                sscanf(line + 9, "%llu", &total);

            if (!strncmp(line, "MemAvailable:", 13) &&
                sscanf(line + 13, "%llu", &available) == 1)
                got_available = 1;
        }
        fclose(f);
    }

    if (total && got_available && available <= total) {
        m->memory = 1 - (double)available / total;
        m->memory_ok = 1;
    }

    return got_cpu ? 0 : -1;
}

static Controls map_metrics(const Metrics *a, const Metrics *b, double dt)
{
    Controls c = {0};

    if (b->total > a->total && b->idle >= a->idle) {
        c.cpu = 1 - (double)(b->idle - a->idle) / (b->total - a->total);
        c.cpu = fmax(0, fmin(1, c.cpu));
    }

    c.irq_ok = a->intr_ok && b->intr_ok;
    c.ctx_ok = a->ctxt_ok && b->ctxt_ok;

    if (dt > 0 && c.irq_ok && b->intr >= a->intr)
        c.irq_rate = (b->intr - a->intr) / dt;

    if (dt > 0 && c.ctx_ok && b->ctxt >= a->ctxt)
        c.ctx_rate = (b->ctxt - a->ctxt) / dt;

    c.memory = b->memory;
    c.memory_ok = b->memory_ok;
    c.freq = 65 * pow(2, 3.5 * c.cpu);
    return c;
}

static void little16(FILE *f, unsigned value)
{
    fputc(value & 255, f);
    fputc((value >> 8) & 255, f);
}

static void little32(FILE *f, uint32_t value)
{
    little16(f, value & 65535);
    little16(f, value >> 16);
}

static int wav_header(FILE *f, uint32_t frames)
{
    if (fseek(f, 0, SEEK_SET))
        return -1;

    fwrite("RIFF", 1, 4, f);
    little32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    little32(f, 16);
    little16(f, 1);              /* PCM */
    little16(f, 2);              /* stereo */
    little32(f, RATE);
    little32(f, RATE * 4);       /* byte rate */
    little16(f, 4);              /* block alignment */
    little16(f, 16);             /* bits per sample */
    fwrite("data", 1, 4, f);
    little32(f, frames * 4);

    return ferror(f) ? -1 : 0;
}

static double number(const char *value, const char *label, double low, double high)
{
    char *end;
    errno = 0;
    double n = strtod(value, &end);

    if (errno || !*value || *end || !isfinite(n) || n < low || n > high) {
        fprintf(stderr, "%s must be %.3g..%.3g\n", label, low, high);
        exit(2);
    }
    return n;
}

static int mode_id(const char *name)
{
    for (int i = 0; i < 4; i++) {
        if (!strcmp(name, mode_names[i]))
            return i;
    }

    fprintf(stderr, "mode must be cpu, irq, scheduler or mix\n");
    exit(2);
}

static void usage(void)
{
    fprintf(stderr,
            "Kernel Noise engine (use ./listen.sh for audio)\n"
            "  --mode cpu|irq|scheduler|mix   --volume 0..0.4\n"
            "  --duration SECONDS            --record FILE.wav\n"
            "  --render FILE.wav             --no-ui\n"
            "Keys: 1-4 mode; m cycle; +/- volume; space mute; q quit.\n");
}

#ifndef KERNEL_NOISE_NO_MAIN
static void read_keyboard(int *mode, double *volume, int *muted)
{
    if (tty_fd < 0)
        return;

    unsigned char keys[32];
    ssize_t count = read(tty_fd, keys, sizeof(keys));

    for (ssize_t i = 0; i < count; i++) {
        unsigned char key = keys[i];

        if (key >= '1' && key <= '4')
            *mode = key - '1';
        else if (key == 'm')
            *mode = (*mode + 1) % 4;
        else if (key == '+' || key == '=')
            *volume = fmin(MAX_VOLUME, *volume + VOLUME_STEP);
        else if (key == '-')
            *volume = fmax(0, *volume - VOLUME_STEP);
        else if (key == ' ')
            *muted = !*muted;
        else if (key == 'q')
            stopping = 1;
    }
}

/* The menu occupies the line above the live activity readout.
 * Redraw only when a key changes the mode or the instrument gain. */
static void draw_menu(int mode, double volume, int update)
{
    if (update)
        fputs("\033[1A\r\033[2K", stderr);

    for (int i = 0; i < 4; i++) {
        if (i > 0)
            fputs("  ", stderr);

        if (i == mode)
            fprintf(stderr, "\033[1;31m%d. %s\033[0m", i + 1, mode_names[i]);
        else
            fprintf(stderr, "%d. %s", i + 1, mode_names[i]);
    }

    /* 100% is the maximum instrument gain (0.4), not system volume.
     * Use dim text to keep the current value readable but unobtrusive. */
    fprintf(stderr,
            " | +/- volume \033[2m(%.0f%%)\033[22m | space mute | q quit\n",
            100.0 * volume / MAX_VOLUME);
    fflush(stderr);
}

static void draw_status(const Controls *c, int muted)
{
    fprintf(stderr,
            "\r\033[2Kcpu %5.1f%% | irq %7.0f/s%s | ctx %7.0f/s%s%s",
            c->cpu * 100,
            c->irq_rate, c->irq_ok ? "" : " N/A",
            c->ctx_rate, c->ctx_ok ? "" : " N/A",
            muted ? " | MUTED" : "");
    fflush(stderr);
}

int main(int argc, char **argv)
{
    int mode = 3;
    int no_ui = 0;
    int file_only = 0;
    double volume = 0.15;
    double duration = 0;
    const char *record = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "--no-ui")) {
            no_ui = 1;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value / unknown argument: %s\n", argv[i]);
            return 2;
        }

        if (!strcmp(argv[i], "--mode"))
            mode = mode_id(argv[++i]);
        else if (!strcmp(argv[i], "--volume"))
            volume = number(argv[++i], "volume", 0, MAX_VOLUME);
        else if (!strcmp(argv[i], "--duration"))
            duration = number(argv[++i], "duration", 0.01, 3600);
        else if (!strcmp(argv[i], "--record") || !strcmp(argv[i], "--render")) {
            if (record) {
                fprintf(stderr, "Choose only one recording path.\n");
                return 2;
            }
            file_only = !strcmp(argv[i], "--render");
            record = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (file_only && !duration) {
        fprintf(stderr, "--render requires --duration.\n");
        return 2;
    }

    Metrics previous;
    if (read_metrics("/proc", &previous)) {
        perror("Cannot read Linux /proc/stat");
        return 1;
    }

    FILE *wav = NULL;
    if (record) {
        int fd = open(record, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (fd < 0) {
            perror(record);
            return 1;
        }

        wav = fdopen(fd, "w+b");
        if (!wav) {
            close(fd);
            perror("Recording");
            return 1;
        }
        if (wav_header(wav, 0)) {
            fclose(wav);
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    atexit(restore_tty);

    int ui = !no_ui && isatty(STDERR_FILENO);
    if (ui)
        open_keyboard();
    setvbuf(stdout, NULL, _IONBF, 0);

    Controls c = {0};
    c.freq = 65;

    double phase = 0, fm_phase = 0;
    double irq_phase = 0.99, ctx_phase = 0.99;
    double hit1 = 0, hit2 = 0;
    double hit_phase1 = 0, hit_phase2 = 0;
    double freq = 65, gain = 0;
    double irq_base = 0, ctx_base = 0;
    double smooth_left = 0, smooth_right = 0;
    double mode_weight[4] = {0};
    mode_weight[mode] = 1;

    int bases_ready = 0;
    int muted = 0;
    int output_error = 0;

    uint64_t frames = 0;
    uint64_t max_frames = duration ? (uint64_t)llround(duration * RATE) : UINT64_MAX;
    if (wav && max_frames > (UINT32_MAX - 36) / 4)
        max_frames = (UINT32_MAX - 36) / 4;

    double start = now_s();
    double metric_time = start, draw_time = start, quit_time = 0;

    if (ui) {
        fprintf(stderr, "\033[?25l\033[32m");
        fprintf(stderr,
        "  _  __                    _ _   _       _                _ _     _             \n"
        " | |/ /___ _ __ _ __   ___| | \\ | | ___ (_)___  ___      | (_)___| |_ ___ _ __  \n"
        " | ' // _ \\ '__| '_ \\ / _ \\ |  \\| |/ _ \\| / __|/ _ \\_____| | / __| __/ _ \\ '_ \\ \n"
        " | . \\  __/ |  | | | |  __/ | |\\  | (_) | \\__ \\  __/_____| | \\__ \\ ||  __/ | | |\n"
        " |_|\\_\\___|_|  |_| |_|\\___|_|_| \\_|\\___/|_|___/\\___|     |_|_|___/\\__\\___|_| |_|\n");
        fprintf(stderr, " v0.2\n""\033[0m\n");   // reset color
        draw_menu(mode, volume, 0);
    }

    while (frames < max_frames) {
        double wall = now_s();

        if (stopping && !quit_time)
            quit_time = wall;
        if (quit_time && wall - quit_time > 0.06)
            break;

        /* Turn successive /proc/stat samples into rates every ~50 ms. */
        if (wall - metric_time >= 0.05) {
            Metrics next;
            if (read_metrics("/proc", &next)) {
                fprintf(stderr, "\nLost /proc/stat.\n");
                output_error = 1;
                break;
            }

            c = map_metrics(&previous, &next, wall - metric_time);
            previous = next;
            metric_time = wall;

            if (!bases_ready) {
                irq_base = fmax(c.irq_rate, 100);
                ctx_base = fmax(c.ctx_rate, 100);
                bases_ready = 1;
            }

            /* Follow the machine's baseline slowly; let sudden changes be heard. */
            irq_base += 0.005 * (fmax(c.irq_rate, 100) - irq_base);
            ctx_base += 0.005 * (fmax(c.ctx_rate, 100) - ctx_base);
        }

        if (ui) {
            int old_mode = mode;
            double old_volume = volume;
            read_keyboard(&mode, &volume, &muted);

            if (mode != old_mode || volume != old_volume) {
                draw_menu(mode, volume, 1);
                draw_status(&c, muted);
            }
        }

        if (ui && wall - draw_time > 0.15) {
            draw_status(&c, muted);
            draw_time = wall;
        }

        size_t nframes = BLOCK;
        if (max_frames - frames < nframes)
            nframes = (size_t)(max_frames - frames);

        unsigned char pcm[BLOCK * 4];
        double irq_density = c.irq_ok && bases_ready
                           ? fmin(35, 2 * c.irq_rate / irq_base) : 0;
        double ctx_density = c.ctx_ok && bases_ready
                           ? fmin(45, 3 * c.ctx_rate / ctx_base) : 0;

        for (size_t i = 0; i < nframes; i++) {
            double elapsed = (frames + i) / (double)RATE;
            double edge = fmin(1, elapsed / 0.25);
            if (duration)
                edge = fmin(edge, (max_frames - frames - i) / (0.05 * RATE));

            double target = muted || stopping ? 0 : volume * edge;
            gain += 0.002 * (target - gain);
            freq += 0.0008 * (c.freq - freq);

            phase += TAU * freq / RATE;
            fm_phase += TAU * freq * 1.41421356237 / RATE;
            if (phase > TAU)
                phase -= TAU;
            if (fm_phase > TAU)
                fm_phase -= TAU;

            irq_phase += irq_density / RATE;
            ctx_phase += ctx_density / RATE;
            if (irq_phase >= 1) {
                irq_phase -= 1;
                hit1 = 0.35;
                hit_phase1 = 0;
            }
            if (ctx_phase >= 1) {
                ctx_phase -= 1;
                hit2 = 0.22;
                hit_phase2 = 0;
            }

            hit_phase1 += TAU * (520 + 1000 * c.cpu) / RATE;
            hit_phase2 += TAU * 1900 / RATE;
            if (hit_phase1 > TAU)
                hit_phase1 -= TAU;
            if (hit_phase2 > TAU)
                hit_phase2 -= TAU;

            hit1 *= 0.996;
            hit2 *= 0.99;

            double drone = sin(phase + 2.5 * c.cpu * sin(fm_phase)) * (0.3 + 0.5 * c.cpu);
            double irq = sin(hit_phase1) * hit1;
            double ctx = sin(hit_phase2) * hit2;

            /* Fade between layers instead of clicking when modes change. */
            for (int k = 0; k < 4; k++) {
                double selected = (k == mode) ? 1 : 0;
                mode_weight[k] += 0.001 * (selected - mode_weight[k]);
            }

            double left = drone * (mode_weight[0] + mode_weight[3])
                        + irq * (mode_weight[1] + mode_weight[3])
                        + ctx * 0.45 * (mode_weight[2] + mode_weight[3]);
            double right = drone * (mode_weight[0] + mode_weight[3])
                         + irq * 0.45 * (mode_weight[1] + mode_weight[3])
                         + ctx * (mode_weight[2] + mode_weight[3]);

            /* Soft saturation, then a one-pole filter to round the bright edges. */
            double target_left = tanh(left * 1.4) * gain;
            double target_right = tanh(right * 1.4) * gain;
            smooth_left += 0.15 * (target_left - smooth_left);
            smooth_right += 0.15 * (target_right - smooth_right);

            int16_t l = (int16_t)lrint(fmax(-1, fmin(1, smooth_left)) * 32767);
            int16_t r = (int16_t)lrint(fmax(-1, fmin(1, smooth_right)) * 32767);

            pcm[i * 4]     = (uint16_t)l & 255;
            pcm[i * 4 + 1] = ((uint16_t)l >> 8) & 255;
            pcm[i * 4 + 2] = (uint16_t)r & 255;
            pcm[i * 4 + 3] = ((uint16_t)r >> 8) & 255;
        }

        size_t bytes = nframes * 4;
        if (!file_only && fwrite(pcm, 1, bytes, stdout) != bytes) {
            output_error = 1;
            break;
        }
        if (wav && fwrite(pcm, 1, bytes, wav) != bytes) {
            output_error = 1;
            break;
        }

        frames += nframes;
        double deadline = start + frames / (double)RATE;
        struct timespec until = {
            (time_t)deadline,
            (long)((deadline - floor(deadline)) * 1e9)
        };
        int rc;
        do {
            rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
        } while (rc == EINTR && !stopping);
    }

    if (wav) {
        if (wav_header(wav, (uint32_t)frames))
            output_error = 1;
        if (fclose(wav))
            output_error = 1;
    }

    restore_tty();
    if (ui)
        fprintf(stderr, "\r\033[2K\033[?25hStopped.\n");
    if (record)
        fprintf(stderr, "Saved %s (%.2fs)\n", record, frames / (double)RATE);
    if (output_error)
        fprintf(stderr, "Audio/file output failed. Check the player or destination.\n");

    return output_error ? 1 : 0;
}
#endif
