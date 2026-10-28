# SPI EEPROM Motor-Profile Controller (LPC2138)

![Language](https://img.shields.io/badge/language-Embedded%20C%20%2B%20ARM%20asm-blue)
![MCU](https://img.shields.io/badge/MCU-NXP%20LPC2138%20(ARM7TDMI--S)-informational)
![Toolchain](https://img.shields.io/badge/toolchain-arm--none--eabi--gcc-green)
![Simulation](https://img.shields.io/badge/verified%20in-Proteus-orange)

Bare-metal firmware for the **NXP LPC2138** that stores a DC-motor **speed/direction profile** in a **Microchip 25LC256 SPI EEPROM**, validates it at power-up, and executes it on a DC motor through an **L293D H-bridge**, with speed ramps, PWM speed control and safe direction reversal.

Written from scratch at register level: no vendor libraries, no RTOS, with a custom start-up file and linker script.

![Proteus schematic](docs/images/proteus_schematic.png)

## Features

- **SPI0 master driver** (mode 0,0, 375 kHz) with a software chip select and a time-out on every transfer
- **25LC256 EEPROM driver:** write enable with WEL check, page-aware writes, WIP polling, sequential reads
- **Validated profile storage:** magic bytes, version number, range checks and checksum; defaults are written and read back automatically if the EEPROM is blank or corrupt
- **Motor control:** 100 Hz PWM (1 % resolution), linear speed ramps, and a ramp-down plus 200 ms pause before every direction change
- **Fault handling:** SPI time-out, mode fault, EEPROM not answering, write protection and write time-out all stop the motor and are reported by LED and error code
- **Debug-friendly:** status variables visible in the Proteus Watch Window

## Hardware

| Ref. | Part | Role |
|---|---|---|
| U1 | NXP LPC2138 | Controller (ARM7TDMI-S, 12 MHz, no PLL) |
| U2 | Microchip 25LC256 | 32 KB SPI EEPROM holding the motor profile |
| U3 | L293D | H-bridge motor driver (5 V logic, 12 V motor supply) |
| M1 | DC motor | Load |
| D1, R1 | LED + 330 Ω | Status indicator |

### Pin map

| LPC2138 pin | Function | Connected to |
|---|---|---|
| P0.4 / SCK0 (27) | SPI clock | 25LC256 SCK (6) |
| P0.5 / MISO0 (29) | SPI data in | 25LC256 SO (2) |
| P0.6 / MOSI0 (30) | SPI data out | 25LC256 SI (5) |
| P0.7 (31) | GPIO chip select | 25LC256 CS (1) |
| P0.8 (33) | PWM output | L293D EN1 (1) |
| P0.12 (38) | Direction | L293D IN1 (2) |
| P0.13 (39) | Direction | L293D IN2 (7) |
| P0.1 (21) | Status LED | R1 → D1 → GND |

EEPROM WP (3) and HOLD (7) are tied to 3.3 V. All LPC2138 supply pins, including VBAT, are at 3.3 V. All grounds are common.

## How it works

![Block diagram](docs/images/block_diagram.png)

1. Start-up code sets the stack, initialises RAM and calls `main()`.
2. Timers and pins are configured; the LED blinks three times.
3. The profile is read from EEPROM address `0x0000` and validated. If it is invalid, the default profile is written, read back and validated again.
4. The steps run in a loop. Any error stops the motor and the LED blinks at 5 Hz.

### EEPROM profile format

![EEPROM layout](docs/images/eeprom_layout.png)

Default profile (29 bytes, written in a single page write):

| Step | Direction | Speed | Ramp | Run |
|---|---|---|---|---|
| 0 | Forward | 75 % | 500 ms | 3000 ms |
| 1 | Stop | 0 % | 300 ms | 1000 ms |
| 2 | Reverse | 40 % | 500 ms | 3000 ms |
| 3 | Stop | 0 % | 300 ms | 1000 ms |

To change it, edit `default_steps[]` in `firmware/main.c` and increase `PROFILE_VERSION`. The new defaults are then written on the next start.

## Repository structure

```
├── firmware/
│   ├── main.c             motor-profile controller
│   ├── startup.s          vector table and C runtime start-up
│   └── linker.ld          memory map (flash 0x00000000, RAM 0x40000000)
├── tests/
│   ├── spi_eeprom_test.c  SPI/EEPROM write-read self-test
│   └── motor_test.c       L293D and motor wiring test
├── docs/
│   ├── Project_Report.pdf full project report
│   └── images/            schematic, diagrams and results
├── Makefile               build on Linux / macOS
└── build.bat              build on Windows
```

## Build

Requires the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (`arm-none-eabi-gcc`) on `PATH`.

```bash
# Windows (Command Prompt)
build.bat                        # firmware\main.c
build.bat tests\motor_test.c     # a test program

# Linux / macOS
make
make SRC=tests/spi_eeprom_test.c
```

Output: `build/firmware.elf` (with debug symbols) and `build/firmware.hex`.

## Simulate in Proteus

1. Build the circuit according to the pin map, or open the Proteus project in `simulation/`.
2. In the properties of U1, set **Program File** to `build/firmware.elf` and **Clock Frequency** to **12 MHz**.
3. Set the motor's nominal voltage to 12 V and run the simulation.
4. To inspect the firmware, pause and open **Debug → Watch Window**, then add the `g_` variables listed below.

### Status indication

| Indication | Meaning |
|---|---|
| 3 blinks, then steady LED | Profile loaded, motor running |
| LED blinking at 5 Hz | EEPROM error, motor held off |
| `g_fail_code` | 1 SPI time-out · 2 SPI fault · 3 EEPROM not answering · 4 write-protected · 5 WEL not set · 6 write time-out · 9 invalid profile |
| `g_profile_source` | 1 = profile found in EEPROM, 2 = defaults written |
| `g_step`, `g_direction`, `g_speed`, `g_cycles` | Current step, direction, duty %, completed cycles |

## Results

| Proteus: motor running | Proteus: real-time simulation |
|---|---|
| ![Motor running](docs/images/proteus_motor_running.png) | ![Status bar](docs/images/proteus_status_bar.png) |

- **EEPROM self-test** (`tests/spi_eeprom_test.c`) passed in Proteus: two patterns, 0x5A and 0xA5, written and read back.
- **Final firmware** ran the motor in Proteus. At 3 min 36 s of simulated time the CPU load was 29 %, with the motor turning and the profile loaded from EEPROM.
- **Profile timing:** the duty cycle measured over one profile cycle (in an ARM emulator) matches the designed 9.9 s cycle:

![Profile cycle](docs/images/profile_cycle.png)

> Status: verified in Proteus simulation. Physical hardware has not been tested yet.

## Design notes

- **Software PWM at 100 Hz** instead of a 3 kHz hardware PWM: the motor and driver are simulated as analog circuits, and the high PWM rate made Proteus run far slower than real time. A hardware-PWM build is still available (`USE_HW_PWM 1`).
- **Every wait has a time-out**, so a wiring or model fault produces an error code instead of a hung CPU.
- **Two-pattern EEPROM test** (0x5A, then 0xA5), so stale data from an earlier run cannot give a false pass.
- **No libgcc:** the ARM7TDMI has no divide instruction, so the firmware includes a small `udiv()`.

## Future work

- Hardware build and testing
- Closed-loop speed control with an encoder
- UART or push-button interface to edit and select several stored profiles
- Timer-interrupt PWM and PLL configuration

## Team

Department of Electronics and Communication Engineering, **Motilal Nehru National Institute of Technology Allahabad**. Academic session 2026–2027.

| Name | Roll No. |
|---|---|
| Anurag Nirmal | 20244031 |
| Arin Saraf | 20244034 |
| Arun Kumar | 20244038 |
| Ashish Kumar Kashyap | 20244043 |
| Ashish Pandey | 20244044 |

Guide: **Dr. Shishir Maheshwari**

## License

Released under the [MIT License](LICENSE).
