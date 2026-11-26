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

SRC := src/tick.c src/capture.c src/session.c src/clock.c src/launch.c src/png.c src/diff.c src/ring.c src/rt.c
OBJ := $(SRC:src/%.c=$(B)/%.o) $(B)/tick.res.o

all: $(B)/Tick.exe

$(B):
	mkdir -p $(B)

$(B)/%.o: src/%.c src/*.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

src/tick.ico: tools/mkicon.py
	python3 -I tools/mkicon.py $@

$(B)/TickHook.dll: hook/tickhook.c src/ring.c src/rt.c src/ring.h src/rt.h | $(B)
	$(CC) $(CFLAGS) $(DLLFLAGS) -o $@ hook/tickhook.c src/ring.c src/rt.c -lkernel32

$(B)/tick.res.o: src/tick.rc src/resource.h src/tick.ico $(B)/TickHook.dll | $(B)
	$(WINDRES) -I src -i src/tick.rc -o $@

$(B)/Tick.exe: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

check: $(B)/Tick.exe test
	OBJDUMP=$(OBJDUMP) sh tools/check-imports.sh $(B)/Tick.exe $(B)/TickHook.dll

test: | $(B)
	gcc -O2 -Wall -Wextra -o $(B)/test_core tests/test_core.c src/rt.c src/png.c src/diff.c src/ring.c -lpthread
	mkdir -p $(B)/testout
	$(B)/test_core $(B)/testout
	python3 -I tests/verify_png.py $(B)/testout

clean:
	rm -rf $(B)

# test helpers for Wine runs (use the normal mingw C runtime; not shipped)
tools-test: $(B)/tickctl.exe $(B)/fakevendor.exe
$(B)/tickctl.exe: tests/tickctl.c | $(B)
	$(CC) -O2 -o $@ $<
$(B)/fakevendor.exe: tests/fakevendor.c | $(B)
	$(CC) -O2 -o $@ $<

.PHONY: all check test clean tools-test
