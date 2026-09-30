# Bare-metal build for the NUCLEO-G474RE remote_bitbang adapter.
#   make            build rbb.bin / rbb.elf
#   make flash      write it with st-flash (uses the onboard ST-LINK)
CROSS  ?= arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g3 -std=c11 \
          -ffreestanding -fno-builtin -Wall -Wextra \
          -ffunction-sections -fdata-sections
LDFLAGS = -T linker.ld -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map=rbb.map

all: rbb.bin

rbb.elf: main.c startup.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) main.c startup.c -o $@
	$(SIZE) $@

rbb.bin: rbb.elf
	$(OBJCOPY) -O binary $< $@

flash: rbb.bin
	st-flash --reset write rbb.bin 0x08000000

clean:
	rm -f rbb.elf rbb.bin rbb.map
.PHONY: all flash clean
