@echo off
rem Usage: build.bat                       builds firmware\main.c
rem        build.bat tests\motor_test.c    builds a test program
set SRC=firmware\main.c
if not "%~1"=="" set SRC=%~1
if not exist build mkdir build
echo Building %SRC% ...
arm-none-eabi-as -mcpu=arm7tdmi -g -o build\startup.o firmware\startup.s || goto fail
arm-none-eabi-gcc -c -mcpu=arm7tdmi -marm -O0 -g -Wall -Wextra -ffreestanding -fno-builtin %SRC% -o build\main.o || goto fail
arm-none-eabi-ld -T firmware\linker.ld -o build\firmware.elf build\startup.o build\main.o || goto fail
arm-none-eabi-objcopy -O ihex build\firmware.elf build\firmware.hex || goto fail
arm-none-eabi-size build\firmware.elf
echo BUILD OK: build\firmware.elf, build\firmware.hex
goto :eof
:fail
echo BUILD FAILED
