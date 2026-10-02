# Host (PC) build of the platform-independent firmware code.
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter
INC      = -I../components/motion_sensor/include -I../components/touch_sensor/include -I../components/expression/include
SRC_CORE = ../components/motion_sensor/src/motion_core.c
SRC_EXPR = ../components/expression/src/expression.c
SRC_TOUCH = ../components/touch_sensor/src/touch_core.c

all: build/test_host build/replay build/replay_touch
	./build/test_host

build/test_host: test_host.c $(SRC_CORE) $(SRC_EXPR) $(SRC_TOUCH) | build
	$(CC) $(CFLAGS) $(INC) $^ -o $@ -lm

build/replay: replay.c $(SRC_CORE) | build
	$(CC) $(CFLAGS) $(INC) $^ -o $@ -lm

build/replay_touch: replay_touch.c $(SRC_TOUCH) | build
	$(CC) $(CFLAGS) $(INC) $^ -o $@ -lm

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
