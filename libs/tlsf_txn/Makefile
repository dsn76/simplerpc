CC      = gcc
CFLAGS  = -std=c17 -Wall -Wextra -Werror -O2
LDFLAGS = -lpthread

SRC     = tlsf_txn.c
OBJ     = $(SRC:.c=.o)
HDR     = tlsf_txn.h tlsf_internal.h

TEST_SRC = test_tlsf_txn.c
TEST_BIN = test_tlsf_txn
BENCH_SRC = bench_tlsf_txn.c
BENCH_BIN = bench_tlsf_txn

.PHONY: all clean test debug bench

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(SRC) $(LDFLAGS)

$(BENCH_BIN): $(BENCH_SRC) $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(BENCH_SRC) $(SRC) $(LDFLAGS)

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

debug: CFLAGS += -g -O0 -DTLSF_DEBUG
debug: clean $(TEST_BIN)

clean:
	rm -f $(OBJ) $(TEST_BIN) $(BENCH_BIN)
