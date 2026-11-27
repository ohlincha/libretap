# Tick: cross-compiled with llvm-mingw (or mingw-w64) for i686 Windows,
# no C runtime, PE subsystem/OS version 4.0 so it starts on Windows 95/2000.
#   make            build/Tick.exe
#   make check      import allow-list check + native unit tests
#   make test       native unit tests only (Linux gcc + python3)

LLVM_MINGW ?= $(wildcard $(HOME)/opt/llvm-mingw-*-msvcrt-ubuntu-*-x86_64)
ifneq ($(LLVM_MINGW),)
  CROSS ?= $(firstword $(LLVM_MINGW))/bin/i686-w64-mingw32-
  CC := $(CROSS)clang
else
  CROSS ?= i686-w64-mingw32-
  CC := $(CROSS)gcc
endif
CROSS64 ?= $(subst i686-,x86_64-,$(CROSS))
CC64 := $(subst i686-,x86_64-,$(CC))
WINDRES ?= $(CROSS)windres
OBJDUMP ?= $(CROSS)objdump
STRIP ?= $(CROSS)strip

B := build
CFLAGS := -O2 -march=i486 -mtune=pentium2 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-stack-arg-probe \
          -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -DWINVER=0x0400 -D_WIN32_WINNT=0x0400 \
          -D_WIN32_IE=0x0400
OSVER := -Wl,--major-subsystem-version,4,--minor-subsystem-version,0,--major-os-version,4,--minor-os-version,0
LDFLAGS := -nostdlib -mwindows -Wl,-e,_TickMain@0 $(OSVER) -Wl,--disable-dynamicbase -s
LIBS := -lkernel32 -luser32 -lgdi32 -lcomdlg32
DLLFLAGS := -shared -nostdlib -Wl,-e,_DllMain@12 -Wl,--kill-at $(OSVER) -s

SRC := src/tick.c src/capture.c src/session.c src/plat_win.c src/clock.c src/launch.c src/programs.c src/lnk.c src/recev.c src/png.c src/diff.c src/ring.c src/rt.c
OBJ := $(SRC:src/%.c=$(B)/%.o) $(B)/tick.res.o $(B)/hook.res.o
SOBJ := $(SRC:src/%.c=$(B)/screen/%.o) $(B)/screen/tick.res.o
CFLAGS64 := -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
          -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type

all: $(B)/Tick.exe $(B)/TickEyes.exe

$(B):
	mkdir -p $(B)

$(B)/%.o: src/%.c src/*.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

# TickEyes.exe: the same program without the listener (no DLL inside)
$(B)/screen/%.o: src/%.c src/*.h | $(B)
	mkdir -p $(B)/screen
	$(CC) $(CFLAGS) -DTICK_SCREEN_ONLY -c -o $@ $<

$(B)/screen/tick.res.o: $(B)/tick.res.o
	mkdir -p $(B)/screen
	cp $< $@

$(B)/TickEyes.exe: $(SOBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOBJ) $(LIBS)

# 64-bit parts, carried inside Tick.exe and used only for 64-bit programs
$(B)/TickHook64.dll: hook/tickhook.c src/ring.c src/rt.c src/ring.h src/rt.h | $(B)
	$(CC64) $(CFLAGS64) -shared -nostdlib -Wl,-e,DllMain -s -o $@ hook/tickhook.c src/ring.c src/rt.c -lkernel32

$(B)/TickHelper64.exe: helper/tickhelper64.c src/rt.c src/rt.h | $(B)
	$(CC64) $(CFLAGS64) -nostdlib -mwindows -Wl,-e,HelperMain -s -o $@ helper/tickhelper64.c src/rt.c -lkernel32

src/tick.ico: tools/mkicon.py
	python3 -I tools/mkicon.py $@

$(B)/TickHook.dll: hook/tickhook.c src/ring.c src/rt.c src/ring.h src/rt.h | $(B)
	$(CC) $(CFLAGS) $(DLLFLAGS) -o $@ hook/tickhook.c src/ring.c src/rt.c -lkernel32

$(B)/tick.res.o: src/tick.rc src/resource.h src/tick.ico | $(B)
	$(WINDRES) -I src -i src/tick.rc -o $@

$(B)/hook.res.o: src/hook.rc src/resource.h $(B)/TickHook.dll $(B)/TickHook64.dll $(B)/TickHelper64.exe | $(B)
	$(WINDRES) -I src -i src/hook.rc -o $@

$(B)/Tick.exe: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

check: $(B)/Tick.exe test
	OBJDUMP=$(OBJDUMP) sh tools/check-imports.sh $(B)/Tick.exe $(B)/TickEyes.exe $(B)/TickHook.dll

test: | $(B)
	gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $(B)/test_shortcuts tests/test_shortcuts.c src/lnk.c src/rt.c src/posix/desktop.c
	rm -rf $(B)/shortcuts && mkdir -p $(B)/shortcuts && $(B)/test_shortcuts $(B)/shortcuts
	gcc -O2 -Wall -Wextra -o $(B)/test_core tests/test_core.c src/rt.c src/png.c src/diff.c src/ring.c -lpthread
	mkdir -p $(B)/testout
	$(B)/test_core $(B)/testout
	python3 -I tests/verify_png.py $(B)/testout

clean:
	rm -rf $(B)

# test helpers for Wine runs (use the normal mingw C runtime; not shipped)
tools-test: $(B)/tickctl.exe $(B)/fakevendor.exe $(B)/fakevendor64.exe $(B)/fakedevices.exe $(B)/fakedevices64.exe \
            $(B)/fakewinvendor.exe $(B)/mklink.exe $(B)/lnkdump.exe
$(B)/fakewinvendor.exe: tests/fakewinvendor.c | $(B)
	$(CC) -O2 -mwindows -o $@ $<
$(B)/mklink.exe: tests/mklink.c | $(B)
	$(CC) -O2 -o $@ $< -lole32 -luuid
$(B)/lnkdump.exe: tests/lnkdump.c src/lnk.c src/rt.c | $(B)
	$(CC) -O2 -o $@ tests/lnkdump.c src/lnk.c src/rt.c
$(B)/fakedevices.exe: tests/fakedevices.c | $(B)
	$(CC) -O2 -o $@ $< -lws2_32
$(B)/fakedevices64.exe: tests/fakedevices.c | $(B)
	$(CC64) -O2 -o $@ $< -lws2_32
$(B)/fakevendor64.exe: tests/fakevendor.c | $(B)
	$(CC64) -O2 -o $@ $<
$(B)/tickctl.exe: tests/tickctl.c | $(B)
	$(CC) -O2 -o $@ $<
$(B)/fakevendor.exe: tests/fakevendor.c | $(B)
	$(CC) -O2 -o $@ $<

# ---------------- Linux: build/linux/tick (static, x86-64) ----------------
LCC ?= gcc
LCFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -D_FILE_OFFSET_BITS=64
LSRC := src/posix/tick_linux.c src/posix/capture_x11.c src/posix/x11.c src/posix/listener_posix.c \
        src/posix/gui_x11.c src/posix/programs_posix.c src/posix/desktop.c \
        src/posix/plat_posix.c src/posix/clock_posix.c src/session.c src/recev.c src/png.c src/diff.c src/ring.c src/rt.c
L := $(B)/linux

linux: $(L)/tick

$(L)/libtickhook.so: hook/tickhook_linux.c src/ring.c src/ring.h
	mkdir -p $(L)
	$(LCC) -shared -fPIC -O2 -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra \
	    -nostartfiles -Wl,--hash-style=both -Wl,-z,noexecstack -s -o $@ hook/tickhook_linux.c

$(L)/hookso.c: $(L)/libtickhook.so tools/embed.py
	python3 -I tools/embed.py $< tickhook_so > $@

$(L)/tick: $(LSRC) src/*.h src/posix/*.h $(L)/hookso.c
	$(LCC) $(LCFLAGS) -static -pthread -o $@ $(LSRC) $(L)/hookso.c
	strip $@

# USB decoding without USB hardware (Linux hook code + recev.c, native)
test-usb: | $(B)
	mkdir -p $(L)
	$(LCC) -O1 -Wall -Wno-unused-function -fno-builtin -o $(L)/test_usb_decode tests/test_usb_decode.c src/recev.c src/rt.c
	$(L)/test_usb_decode

linux-check: $(L)/tick
	sh tools/check-linux.sh $(L)/tick $(L)/libtickhook.so

linux-tools-test:
	$(LCC) -O2 -Wall -o $(L)/xdnd_drop tests/xdnd_drop.c src/posix/x11.c src/rt.c
	$(LCC) -O2 -o $(L)/fakevendor_linux tests/fakevendor_linux.c
	$(LCC) -O2 -o $(L)/fakedevices_linux tests/fakedevices_linux.c

# ---------------- DemoMeter / DemoInstrument: a pretend vendor program and instrument for trying Tick ----------------
DEMOFLAGS := $(filter-out -DWINVER=0x0400 -D_WIN32_WINNT=0x0400,$(CFLAGS)) -DWINVER=0x0400 -D_WIN32_WINNT=0x0400
demo: $(B)/demo/DemoMeter.exe $(B)/demo/DemoInstrument.exe $(B)/demo/demo-instrument
$(B)/demo/demo-instrument: demo/instrument_linux.c
	mkdir -p $(B)/demo
	$(LCC) -O2 -Wall -Wextra -static -o $@ $< -lm
	strip $@
$(B)/demo/DemoMeter.exe: demo/meter.c demo/rt_demo.c demo/demo_common.h
	mkdir -p $(B)/demo
	$(CC) $(DEMOFLAGS) -nostdlib -mwindows -Wl,-e,_MeterMain@0 $(OSVER) -Wl,--disable-dynamicbase -s -o $@ demo/meter.c demo/rt_demo.c -lkernel32 -luser32 -lgdi32 -lws2_32
$(B)/demo/DemoInstrument.exe: demo/instrument.c demo/rt_demo.c demo/demo_common.h
	mkdir -p $(B)/demo
	$(CC) $(DEMOFLAGS) -nostdlib -mconsole -Wl,-e,_InstrumentMain@0 $(OSVER) -Wl,--disable-dynamicbase -s -o $@ demo/instrument.c demo/rt_demo.c -lkernel32 -luser32 -lws2_32
demo-check: demo
	sh tools/check-demo-imports.sh $(OBJDUMP) $(B)/demo/DemoMeter.exe $(B)/demo/DemoInstrument.exe

.PHONY: demo demo-check all check test clean tools-test linux linux-check test-usb linux-tools-test
