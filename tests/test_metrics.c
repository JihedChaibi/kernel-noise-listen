#define KERNEL_NOISE_NO_MAIN
#include "../src/listen.c"
#include <assert.h>

int main(void)
{
    char dir[] = "/tmp/kernel-noise-test-XXXXXX";
    char path[1024];
    assert(mkdtemp(dir));

    snprintf(path, sizeof(path), "%s/stat", dir);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("cpu 100 20 30 400 50 6 7 8 9 10\nintr 500 1 2 3\nctxt 600\n", f);
    fclose(f);

    snprintf(path, sizeof(path), "%s/meminfo", dir);
    f = fopen(path, "w");
    assert(f);
    fputs("MemTotal: 1000 kB\nMemAvailable: 400 kB\n", f);
    fclose(f);

    Metrics a, b;
    assert(read_metrics(dir, &a) == 0);
    assert(a.total == 621 && a.idle == 450); /* guest fields aren't counted twice */
    assert(a.intr == 500 && a.ctxt == 600 && a.memory_ok);
    assert(fabs(a.memory - 0.6) < 1e-9);

    b = a;
    b.total += 100;
    b.idle += 25;
    b.intr += 150;
    b.ctxt += 200;

    Controls c = map_metrics(&a, &b, 0.05);
    assert(fabs(c.cpu - 0.75) < 1e-9);
    assert(fabs(c.irq_rate - 3000) < 1e-9);
    assert(fabs(c.ctx_rate - 4000) < 1e-9);
    assert(c.freq > 65 && c.freq < 736);

    /* Counter reset: never underflow. */
    c = map_metrics(&b, &a, 0.05);
    assert(c.cpu == 0 && c.irq_rate == 0 && c.ctx_rate == 0);

    snprintf(path, sizeof(path), "%s/meminfo", dir);
    unlink(path);
    assert(read_metrics(dir, &a) == 0 && !a.memory_ok);

    snprintf(path, sizeof(path), "%s/stat", dir);
    unlink(path);
    assert(read_metrics(dir, &a) == -1);

    rmdir(dir);
    puts("Metric parser/mappings: passed");
    return 0;
}
