# Shimano EP8 Battery Display

A small standalone display that shows the battery level of a Shimano EP8 e-bike system. An ESP32-C3 connects over Bluetooth LE to the Shimano wireless unit (e.g. `SWEN600`), reads the standard Battery Level characteristic and draws it on a 128×64 SSD1306 OLED.

```
EP8 BATTERY          BLE
------------------------
          87%
 ┌───────────────────┐
 │█████████████████  │▌
 └───────────────────┘
```

## Hardware

- Seeed Studio XIAO ESP32-C3 (any ESP32-C3 board works; adjust the GPIOs)
- 0.96" SSD1306 OLED, 128×64, I²C

| OLED | XIAO ESP32-C3     |
|------|-------------------|
| VCC  | 3V3               |
| GND  | GND               |
| SDA  | D4 (GPIO6)        |
| SCL  | D5 (GPIO7)        |

The internal I²C pull-ups are enabled; most OLED modules also have their own.

## Building and flashing

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/) v5.x (developed with 5.5.3).

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c3   # only needed once
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Configuration

Run `idf.py menuconfig` and open **EP8 Display**:

| Option | Default | Description |
|--------|---------|-------------|
| `EP8_TARGET_NAME_PREFIX` | `SWEN600` | Connect to the first device whose advertised name starts with this |
| `EP8_TARGET_MAC` | *(empty)* | Fixed BLE address, e.g. `DF:8A:B5:19:68:81`; overrides the name prefix |
| `EP8_PASSKEY` | `0` | Passkey used if the unit asks for passkey entry |
| `EP8_POLL_INTERVAL_S` | `60` | Fallback read interval in addition to notifications |
| `EP8_I2C_SDA_GPIO` | `6` | OLED SDA pin |
| `EP8_I2C_SCL_GPIO` | `7` | OLED SCL pin |
| `EP8_OLED_I2C_ADDR` | `0x3C` | OLED I²C address |

Bluetooth defaults (NimBLE, central role, bonding persisted in NVS) are in [sdkconfig.defaults](sdkconfig.defaults).

## How it works

1. Scans for the Shimano unit by name prefix or MAC address.
2. Connects and discovers Battery Service `0x180F` / Battery Level `0x2A19`.
3. If the unit demands encryption, pairs (Just Works, or the configured passkey) and stores the bond so later reconnects encrypt straight away. A stale bond is deleted automatically and pairing is retried.
4. Reads the level once, subscribes to notifications and additionally polls every `EP8_POLL_INTERVAL_S` seconds.
5. On disconnect it rescans with exponential backoff (1 s up to 30 s), so it doesn't hammer the unit while e.g. the phone app holds the connection.

### Display

- Header: `EP8 BATTERY` and connection state — `SCAN`, `CONN`, `PAIR` or `BLE` (connected).
- Large percentage and a battery bar.
- While reconnecting, the last known value stays on screen with a small `?` marking it as stale; `--%` means no value has been received yet.

## Project layout

| File | Purpose |
|------|---------|
| [main/main.c](main/main.c) | Startup and UI drawing |
| [main/ble_battery.c](main/ble_battery.c) | NimBLE central: scan, connect, pairing, GATT discovery, notifications |
| [main/oled.c](main/oled.c) | SSD1306 driver via `esp_lcd` with a framebuffer and simple drawing primitives |
| [main/font5x7.h](main/font5x7.h) | 5×7 ASCII font, scalable |
| [main/Kconfig.projbuild](main/Kconfig.projbuild) | menuconfig options |

## Troubleshooting

- **Stuck on `SCAN`** – check the unit's advertised name with a BLE scanner app (e.g. nRF Connect) and adjust `EP8_TARGET_NAME_PREFIX` or set `EP8_TARGET_MAC`. The unit may only advertise while the bike is switched on and no other device (E-Tube app) is connected.
- **Cycling between `CONN`/`PAIR`** – erase stored bonds with `idf.py erase-flash` and flash again.
- **Blank display** – check wiring and the I²C address (some modules use `0x3D`); the serial log prints `OLED init failed` in that case.
