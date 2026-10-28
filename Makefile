# Build:      make
# Test build: make SRC=tests/motor_test.c
CROSS  ?= arm-none-eabi-
SRC    ?= firmware/main.c
BUILD  := build
CFLAGS := -mcpu=arm7tdmi -marm -O0 -g -Wall -Wextra -ffreestanding -fno-builtin

all: $(BUILD)/firmware.hex

$(BUILD):
	mkdir -p $@

$(BUILD)/startup.o: firmware/startup.s | $(BUILD)
	$(CROSS)as -mcpu=arm7tdmi -g -o $@ $<

$(BUILD)/main.o: $(SRC) | $(BUILD)
	$(CROSS)gcc -c $(CFLAGS) $< -o $@

$(BUILD)/firmware.elf: $(BUILD)/startup.o $(BUILD)/main.o firmware/linker.ld
	$(CROSS)ld -T firmware/linker.ld -o $@ $(BUILD)/startup.o $(BUILD)/main.o
	$(CROSS)size $@

$(BUILD)/firmware.hex: $(BUILD)/firmware.elf
	$(CROSS)objcopy -O ihex $< $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
.DELETE_ON_ERROR:
