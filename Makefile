# Cross-compilation Makefile for MT7628 MIPS32 LE utilities

CC := zig cc -target mipsel-linux-musleabi
CFLAGS := -static -Os -fno-stack-protector

all: bin/i2s_dump bin/i2s_tiny bin/telnetd

bin:
	mkdir -p bin

bin/i2s_dump: src/i2s_dump.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/i2s_tiny: src/i2s_tiny.c | bin
	$(CC) -static -nostdlib -Os -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -Wl,-e,_start -o $@ $<

bin/telnetd: src/telnetd.c | bin
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf bin

.PHONY: all clean
