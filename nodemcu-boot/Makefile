.PHONY: build
build: $(addsuffix .bin,$(addprefix bin/nodemcu-boot-, 9600 74880 115200 1000000 silent nobaudchange))

CC=xtensa-lx106-elf-cc
MKESPIMAGE?=./mkespimage.py

CFLAGS=-std=c99 -Os -Wall -Wextra -mtext-section-literals -fdata-sections -fomit-frame-pointer -mlongcalls

LDFLAGS=-nostdlib -T ld/nodemcu-boot.ld -Wl,-EL -static -Wl,-Map=$@.map -Wl,--cref

bin/nodemcu-boot-%.bin: obj/nodemcu-boot-%.elf
	$(MKESPIMAGE) elf2image -o "bin/$*-" $<
	mv "bin/$*-0x00000.bin" $@

.PRECIOUS: obj/nodemcu-boot-%.elf
obj/nodemcu-boot-%.elf: src/main.c
	$(CC) $(CFLAGS) -DVERBOSE=1 -DBAUD_RATE=$* $^ $(LDFLAGS) -o $@

obj/nodemcu-boot-nobaudchange.elf: src/main.c
	$(CC) $(CFLAGS) -DVERBOSE=1 -DBAUD_RATE=0 $^ $(LDFLAGS) -o $@

obj/nodemcu-boot-silent.elf: src/main.c
	$(CC) $(CFLAGS) -DVERBOSE=0 $^ $(LDFLAGS) -o $@

.PHONY: clean
clean:
	-rm -f obj/* bin/*
