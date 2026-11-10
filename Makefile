VERSION ?= 1.0.0
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
STATIC ?= -static
OUT ?= build

all: $(OUT)/zonezap

$(OUT)/zonezap: src/main.c src/logic.c src/logic.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DZZ_VERSION=\"$(VERSION)\" $(STATIC) -o $@ src/main.c src/logic.c
	strip $@ 2>/dev/null || true

$(OUT)/test_logic: tests/test_logic.c src/logic.c src/logic.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fsanitize=undefined -fsanitize-trap=undefined -o $@ tests/test_logic.c src/logic.c

test: $(OUT)/test_logic
	$(OUT)/test_logic

clean:
	rm -rf $(OUT)

.PHONY: all test clean
