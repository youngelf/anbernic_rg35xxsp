# Makefile for Anbernic RG35XX SP (Knulli / Batocera aarch64) & Linux Host Pomodoro Timer
#
# Usage:
#   make               - Build Anbernic RG35XX SP (aarch64) binary (pomodoro) and sdcard_package/
#   make 35xxsp        - Build Anbernic RG35XX SP (aarch64) binary (pomodoro)
#   make linux         - Build x86_64 Linux X11 binary (pomodoro_linux) for desktop debugging
#   make run-linux     - Build and launch pomodoro_linux in a 640x480 X11 window
#   make package       - Populate sdcard_package/roms/ports/ ready to copy to Knulli SD card
#   make clean         - Remove compiled binaries and sdcard_package/

# --- Anbernic RG35XX SP (Allwinner H700 Cortex-A53 aarch64) Configuration ---
AARCH64_ROOT ?= $(HOME)/.cache/aarch64_toolchain
ifneq ($(wildcard $(AARCH64_ROOT)/usr/bin/aarch64-linux-gnu-gcc-14),)
    AARCH64_PATH   = $(AARCH64_ROOT)/usr/bin:$(PATH)
    AARCH64_LD_LIB = $(AARCH64_ROOT)/usr/lib/x86_64-linux-gnu:$(AARCH64_ROOT)/usr/lib
    CC             = PATH="$(AARCH64_PATH)" LD_LIBRARY_PATH="$(AARCH64_LD_LIB)" aarch64-linux-gnu-gcc-14
    STRIP          = PATH="$(AARCH64_PATH)" LD_LIBRARY_PATH="$(AARCH64_LD_LIB)" aarch64-linux-gnu-strip
    SYSROOT_FLAGS  = --sysroot="$(AARCH64_ROOT)" \
                     -isystem "$(AARCH64_ROOT)/usr/aarch64-linux-gnu/include" \
                     -B"$(AARCH64_ROOT)/usr/aarch64-linux-gnu/lib" \
                     -B"$(AARCH64_ROOT)/usr/lib/gcc-cross/aarch64-linux-gnu/14" \
                     -L"$(AARCH64_ROOT)/usr/aarch64-linux-gnu/lib"
else
    CC             = aarch64-linux-gnu-gcc
    STRIP          = aarch64-linux-gnu-strip
    SYSROOT_FLAGS  =
endif

CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -mcpu=cortex-a53 -DTARGET_RG35XXSP $(SYSROOT_FLAGS)
LDFLAGS = -lm -ldl

# --- Host Linux (x86_64 X11 640x480 window) Configuration ---
HOST_CC      ?= gcc
HOST_SCALE   ?= 2
HOST_CFLAGS  = -O2 -g -Wall -Wextra -std=gnu11 -DHOST_X11 -DHOST_SCALE=$(HOST_SCALE)
HOST_LDFLAGS = -lX11 -lm

# --- Targets ---
TARGET      = pomodoro
HOST_TARGET = pomodoro_linux
SRCS        = pomodoro.c

.PHONY: all 35xxsp rg35xxsp linux host run-linux package clean

# Default builds the Anbernic RG35XX SP (aarch64) binary and SD card package
all: $(TARGET) package

35xxsp: $(TARGET)

rg35xxsp: $(TARGET)

# Build x86_64 Linux binary for local desktop testing/debugging
linux: $(HOST_TARGET)

host: $(HOST_TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)
	$(STRIP) $(TARGET)

$(HOST_TARGET): $(SRCS)
	$(HOST_CC) $(HOST_CFLAGS) -o $(HOST_TARGET) $(SRCS) $(HOST_LDFLAGS)

run-linux: $(HOST_TARGET)
	./$(HOST_TARGET)

package: $(TARGET)
	mkdir -p sdcard_package/roms/ports/pomodoro
	cp $(TARGET) sdcard_package/roms/ports/pomodoro/
	cp pomodoro.png sdcard_package/roms/ports/pomodoro/
	cp Pomodoro.sh sdcard_package/roms/ports/Pomodoro.sh
	chmod +x sdcard_package/roms/ports/Pomodoro.sh sdcard_package/roms/ports/pomodoro/$(TARGET)

clean:
	rm -f $(TARGET) $(HOST_TARGET)
	rm -rf sdcard_package
