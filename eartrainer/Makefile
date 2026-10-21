# Makefile for Anbernic RG 35xxSP Guitar Ear Trainer (640x480)
#
# Targets:
#   1. Anbernic RG 35xxSP (Allwinner H700 ARM Cortex-A53 64-bit aarch64)
#   2. Linux Desktop (x86_64 X11 native 640x480 window + PulseAudio)

TARGET      = eartrainer
HOST_TARGET = eartrainer_linux
SRC         = eartrainer.c

TC_CACHE = $(HOME)/.cache/aarch64_toolchain
ifneq ($(wildcard $(TC_CACHE)/usr/bin/aarch64-linux-gnu-gcc-14),)
    ARM_ENV   = PATH="$(TC_CACHE)/usr/bin:$$PATH" LD_LIBRARY_PATH="$(TC_CACHE)/usr/lib/x86_64-linux-gnu:$(TC_CACHE)/usr/lib"
    ARM_CC    = $(ARM_ENV) aarch64-linux-gnu-gcc-14
    ARM_STRIP = $(ARM_ENV) aarch64-linux-gnu-strip
    ARM_SYS   = --sysroot="$(TC_CACHE)" \
                -isystem "$(TC_CACHE)/usr/aarch64-linux-gnu/include" \
                -B"$(TC_CACHE)/usr/aarch64-linux-gnu/lib" \
                -B"$(TC_CACHE)/usr/lib/gcc-cross/aarch64-linux-gnu/14" \
                -L"$(TC_CACHE)/usr/aarch64-linux-gnu/lib"
else
    ARM_CC    = aarch64-linux-gnu-gcc
    ARM_STRIP = aarch64-linux-gnu-strip
    ARM_SYS   =
endif

ARM_CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -mcpu=cortex-a53 -DTARGET_RG35XXSP -D_GNU_SOURCE=1 $(ARM_SYS)
ARM_LDFLAGS = -lm -lpthread -ldl

HOST_CC      = gcc
HOST_CFLAGS  = -O2 -Wall -Wextra -std=gnu99 -DHOST_X11 -D_GNU_SOURCE=1
HOST_LDFLAGS = -lX11 -lm -lpthread -ldl

.PHONY: all arm rg35xxsp linux linux-x64 x64 host run-linux package clean

all: $(TARGET) $(HOST_TARGET) package

arm: $(TARGET)

rg35xxsp: $(TARGET)

linux: $(HOST_TARGET)

linux-x64: $(HOST_TARGET)

x64: $(HOST_TARGET)

host: $(HOST_TARGET)

$(TARGET): $(SRC) eartrainer_font.h
	$(ARM_CC) $(ARM_CFLAGS) -o $(TARGET) $(SRC) $(ARM_LDFLAGS)
	-$(ARM_STRIP) $(TARGET)

$(HOST_TARGET): $(SRC) eartrainer_font.h
	$(HOST_CC) $(HOST_CFLAGS) -o $(HOST_TARGET) $(SRC) $(HOST_LDFLAGS)

run-linux: $(HOST_TARGET)
	./$(HOST_TARGET)

package: $(TARGET)
	rm -rf sdcard_package
	mkdir -p sdcard_package/roms/ports/eartrainer
	cp $(TARGET) eartrainer.png sdcard_package/roms/ports/eartrainer/
	cp EarTrainer.sh sdcard_package/roms/ports/EarTrainer.sh
	chmod +x sdcard_package/roms/ports/EarTrainer.sh sdcard_package/roms/ports/eartrainer/$(TARGET)

clean:
	rm -rf $(TARGET) $(HOST_TARGET) sdcard_package
