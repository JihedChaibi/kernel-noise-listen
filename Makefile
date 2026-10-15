CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic

.PHONY: all test clean
all: .build/kernel-noise
.build/kernel-noise: src/listen.c
	mkdir -p .build
	$(CC) -std=c11 $(CFLAGS) $< -lm -o $@
test: all
	$(CC) -std=c11 $(CFLAGS) -Wno-unused-function tests/test_metrics.c -lm -o .build/test-metrics
	.build/test-metrics
	python3 -m unittest discover -s tests -v
clean:
	rm -rf .build
