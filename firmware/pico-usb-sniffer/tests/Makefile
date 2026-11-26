# Host tests for USB Sniffer Stream (stream.c and the capture loop body in capture_ring.h). make -C tests
CC ?= cc
CFLAGS += -std=gnu11 -O2 -W -Wall -Wno-unused-function -g -fsanitize=address,undefined -I../firmware

all: test

test_stream: test_stream.c usbline.h ../firmware/stream.c ../firmware/stream.h ../firmware/capture_ring.h
	$(CC) $(CFLAGS) -DCAP_RING_WORDS=32768 test_stream.c ../firmware/stream.c -o $@

test: test_stream
	./test_stream

clean:
	rm -rf test_stream build-emu

# Emulator smoke test (optional): make emu RP2040JS=/path/to/node_modules/rp2040js/dist/cjs/index.js
# Needs arm-none-eabi-gcc on the PATH and node. rp2040js emulates core 0 only, so emu_capture.c replaces capture.c.
ARM ?= arm-none-eabi-
FW = ../firmware
ARMFLAGS = -W -Wall --std=gnu11 -Os -ffreestanding -fdata-sections -ffunction-sections -funsigned-char \
  -funsigned-bitfields -mcpu=cortex-m0plus -mthumb -I$(FW)/include -I$(FW) -Ibuild-emu \
  -DCAP_RING_WORDS=8192 -DTX_SIZE=32768
EMU_SRCS = $(FW)/main.c $(FW)/stream.c $(FW)/usb.c $(FW)/usb_std.c $(FW)/usb_cdc.c $(FW)/usb_descriptors.c \
  $(FW)/startup_rp2040.c emu_capture.c

build-emu/emu_traffic.h: gen_traffic.c usbline.h
	mkdir -p build-emu
	$(CC) -std=gnu11 -O2 gen_traffic.c -o build-emu/gen_traffic
	build-emu/gen_traffic > $@

build-emu/emu.bin: build-emu/emu_traffic.h $(EMU_SRCS) $(wildcard $(FW)/*.h)
	$(ARM)gcc $(ARMFLAGS) -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
	  -Wl,--script=$(FW)/linker/rp2040.ld $(EMU_SRCS) -o build-emu/emu.elf
	$(ARM)objcopy -O binary build-emu/emu.elf $@
	$(ARM)nm -n build-emu/emu.elf > build-emu/emu.sym

emu: build-emu/emu.bin
	@test -n "$(RP2040JS)" || (echo "set RP2040JS to rp2040js's dist/cjs/index.js"; exit 1)
	node emu_smoke.js $(RP2040JS) build-emu/emu.bin build-emu/emu.sym build-emu/emu_traffic.h
