# vic2 - cycle-exact MOS 6569 (PAL) VIC-II + test bench
CC      := clang
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter

# Some macOS setups ship an SDK whose libSystem.tbd the installed linker cannot
# read; probe a trivial link and fall back to the newest SDK that links.
SDKS := $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk 2>/dev/null | sort -rV)
LINK_OK := $(shell printf 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null 2>/dev/null && echo yes)
ifneq ($(LINK_OK),yes)
  SYSROOT := $(firstword $(foreach s,$(SDKS),$(shell printf 'int main(void){return 0;}' | $(CC) -isysroot $(s) -x c - -o /dev/null 2>/dev/null && echo $(s))))
  ifneq ($(SYSROOT),)
    CFLAGS += -isysroot $(SYSROOT)
  endif
endif

all: victest

victest: victest.c vic.c vic.h
	$(CC) $(CFLAGS) -o $@ victest.c vic.c

test: victest
	@mkdir -p out
	./victest

clean:
	rm -f victest

.PHONY: all test clean
