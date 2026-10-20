# Rise Hills

Firmware and tools for the **Rise Hills** interactive residential-complex model: LED building lighting, the "Building Layout" lift mechanism and a retractable alley. Every node is an **ESP32**, and they talk to each other over **ESP-NOW**. You control the system through the Master over USB Serial or Bluetooth.

## Architecture

```
              USB Serial / Bluetooth
                       │
               ┌───────▼───────┐
               │    Master     │  ESP32
               └───────┬───────┘
                       │ ESP-NOW
     ┌─────────────┬───┴─────────┬──────────────┐
     ▼             ▼             ▼              ▼
 LED Slave ×4   Building      Alley          (LED TEST —
 blue / red /   Layout        Stepper         desktop app that
 yellow/orange  (3 stepper    (stepper +      sends commands
 RGBW strip     motors)       LED strip)      over Serial)
```

## Repository layout

| Folder | Description |
|---|---|
| `Rise Hills Msater/` | Master controller: takes commands over Serial/Bluetooth and forwards them to the slaves via ESP-NOW (`src/Master.cpp`) |
| `Rise Hills slave/` | LED slave: NeoPixel RGBW strip (up to 3700 pixels) with non-blocking effects (`src/Slave.cpp`) |
| `Rise Hills Building Layout Slave/` | Raises/lowers the building model: 3 stepper motors (AccelStepper), limit switches, settings stored in Preferences (`src/Building.cpp`) |
| `Rise Hills Aleya Slave/` | Retractable alley: stepper motor with an endstop plus LED lighting (`src/Alley.cpp`) |
| `LED TEST/` | PyQt6 desktop app for controlling the system over Serial (`esp_tester.py`, layout in `building.json`) |
| `CUT/` | Cutting drawings (DWG/DXF/CDR): ceiling, top LED lighting, power supply mounts |
| `COMMANDS.txt` | Master command cheat sheet |

## Hardware

- Board: ESP32 (`upesy_wroom`)
- LEDs: addressable RGBW strips (NeoPixel, `NEO_GRBW`)
- Mechanics: stepper motors on STEP/DIR drivers (TMC), limit switches

## Build and flash

Each firmware folder is a separate [PlatformIO](https://platformio.org/) project. The easiest way to work with them is to open `Rise hills.code-workspace` in VS Code with the PlatformIO extension installed.

```bash
cd "Rise Hills Msater"
pio run -t upload
pio device monitor   # 115200 baud
```

Before flashing:

1. **LED slaves**: in `Slave.cpp`, set `MY_DEVICE_ID` (`blue` | `red` | `yellow` | `orange`) for each device.
2. **Master**: put the slave MAC addresses into `ledDevices[]`, `buildingMAC` and `alleyMAC`. Each slave prints its MAC to Serial on boot.

## LED TEST (desktop app)

```bash
cd "LED TEST"
python3 -m venv .venv && source .venv/bin/activate
pip install PyQt6 pyserial
python esp_tester.py
```

## Master commands

### LED devices

Global (all 4 devices):

| Command | Description |
|---|---|
| `off` | turn all off |
| `on` / `on white` | turn all on (color optional) |
| `allOn` | turn all on in white |
| `random` | random effect |
| `brightness 128` | brightness 0–255 |
| `fill red` | fill with a color |
| `blink white 500 10` | blink: color, period ms, count |
| `breath blue 50 8` | breathe: color, steps, pause ms |
| `wipe cyan 5` | wipe: color, pause ms |
| `chase white 30 2` | chase: color, tail length, pause ms |
| `sparkle yellow 20 60 15` | sparkle: color, count, pause ms, iterations |
| `strobe white 50 50 8` | strobe: color, on ms, off ms, count |
| `sections 4` | split the strip into N sections |
| `reffect 0 500 chase white 30 2` | effect on a pixel range |
| `range 0 500 red` | color a pixel range |

For a specific device, prefix the command with its name:

```
blue on red
red fill cyan
yellow solo                         # yellow on, others off
orange reffect 0 500 chase white 30 2
blue range 0 200 red
```

Colors: `white red green blue yellow cyan magenta orange purple warm off`, or RGB(W): `255,100,0,0`.

### Building Layout (lift)

| Command | Description |
|---|---|
| `building down` | lower: M1 → pause → M3 → pause → M2 |
| `building up` | raise: M2 → pause → M3 → pause → M1 |
| `building stop` | emergency stop |

### Alley

| Command | Description |
|---|---|
| `alley 1` | POS1: retract (LED turns off) |
| `alley 2` | POS2: extend (LED + running light) |
| `alley help` | status and configuration |

### System

| Command | Description |
|---|---|
| `status` | list devices with MAC addresses |
| `help` | help |
| `all <command>` | explicitly send a command to all LED devices |
