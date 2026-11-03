# Terminator - build with `make`, run with `./terminator`.

CC      ?= gcc
TARGET  := terminator

SRC_DIR := src
OBJ_DIR := build
SRCS    := $(wildcard $(SRC_DIR)/*.c)
OBJS    := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DEPS    := $(OBJS:.o=.d)

# X11 flags from pkg-config, falling back to the usual defaults.
X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null)
X11_LIBS   := $(shell pkg-config --libs x11 2>/dev/null || echo -lX11)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -pthread \
           -MMD -MP $(X11_CFLAGS)
LDLIBS  += $(X11_LIBS) -pthread

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

-include $(DEPS)
