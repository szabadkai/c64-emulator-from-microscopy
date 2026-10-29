# Makefile for the SID emulation (C99, no dependencies)
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
LDLIBS  += -lm

# Some macOS setups have a default SDK whose libSystem.tbd the installed
# linker cannot read. Probe a trivial link; if it fails, use the newest SDK
# that links.
ifeq ($(shell uname),Darwin)
SDKS    := $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk /opt/xcode/*.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.*.sdk 2>/dev/null | sort -rV)
LINK_OK := $(shell printf 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null 2>/dev/null && echo yes)
ifneq ($(LINK_OK),yes)
SYSROOT := $(firstword $(foreach s,$(SDKS),$(shell printf 'int main(void){return 0;}' | $(CC) -isysroot $(s) -x c - -o /dev/null 2>/dev/null && echo $(s))))
ifneq ($(SYSROOT),)
override CFLAGS += -isysroot $(SYSROOT)
endif
endif
endif

OBJS = sid.o wav.o

.PHONY: all test clean

all: sidtest

sidtest: sidtest.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ sidtest.o $(OBJS) $(LDLIBS)

%.o: %.c sid.h wav.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: sidtest
	@mkdir -p out
	./sidtest

clean:
	rm -f *.o sidtest
