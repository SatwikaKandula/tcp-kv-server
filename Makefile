CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -g
LDFLAGS += -pthread
CFLAGS  += -pthread -D_POSIX_C_SOURCE=200809L

SERVER_SRC = src/server.c src/threadpool.c src/kvstore.c
BENCH_SRC  = src/bench.c

.PHONY: all clean tsan asan test

all: kvserver bench

kvserver: $(SERVER_SRC) src/threadpool.h src/kvstore.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS)

bench: $(BENCH_SRC)
	$(CC) $(CFLAGS) -o $@ $(BENCH_SRC) $(LDFLAGS)

# ThreadSanitizer build: detects data races at runtime.
tsan: $(SERVER_SRC)
	$(CC) -std=c11 -O1 -g -pthread -D_POSIX_C_SOURCE=200809L -fsanitize=thread -o kvserver-tsan $(SERVER_SRC)

# AddressSanitizer + LeakSanitizer build: detects memory errors and leaks.
asan: $(SERVER_SRC)
	$(CC) -std=c11 -O1 -g -pthread -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined \
	      -fno-omit-frame-pointer -o kvserver-asan $(SERVER_SRC)

test: all
	./tests/run_tests.sh

clean:
	rm -f kvserver bench kvserver-tsan kvserver-asan
