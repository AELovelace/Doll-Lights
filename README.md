# MrBox on QuinLED-Dig-Quad + Dig2Analog

This sketch is the MrBox firmware for QuinLED-Dig-Quad + QuinLED-Dig2Analog lighting units. Each **zone** is one Dig-Quad LED output wired to its own Dig2Analog converter and 12 V common-positive analog RGBW strip. For each zone, the ESP32 sends one RGBW WS28xx pixel, and that zone's Dig2Analog turns it into four power PWM outputs.

One sketch builds every unit. Set `#define BUILD_FOR_UNIT` at the top of `ssd1306_128x64_i2c.ino` before flashing; it chooses the activator number and the zone layout:

| Unit (`BUILD_FOR_UNIT`) | Name | Zone | Zone name | Dig-Quad output | ESP32 GPIO | Command range |
|---|---|---|---|---|---|---|
| `32004` (sketch default), Dig-Quad **v4** | Room Lights | 1 | Left | `LED1` | 2 | `1000`–`4255` |
| | | 2 | Right | `LED2` | 4 | `5000`–`8255` |
| `32001`, Dig-Quad v2/v3 | RGBW Light (original unit) | 1 | Main | `LED1` | 16 | `1000`–`4255` |

The two units are different Dig-Quad revisions with different LED GPIOs. 32004's QuinLED ESP32 v4 module is an ESP32-PICO-V3-02 (8 MB flash, 2 MB built-in PSRAM), and on that chip GPIO16/17 belong to the PSRAM. A build that drives GPIO16 hangs it in a watchdog reset loop before anything prints.

The deployed 32001 is still on its older single-zone firmware. Rebuilding it from this sketch with `BUILD_FOR_UNIT 32001` keeps the same commands; its state JSON just gains a one-entry `zones` list. A Dig-Quad has four outputs, so the firmware and the boxchat standard allow 1–4 zones. An unknown `BUILD_FOR_UNIT` refuses to compile until you add its layout.

## Required Arduino setup

- Board package: **esp32 by Espressif Systems**
- Board selection: **ESP32 Dev Module** (or the matching QuinLED ESP32 module option)
- Libraries: Adafruit NeoPixel and PubSubClient

## Wiring

Disconnect power while changing wiring. This table is for the two-zone unit 32004; a single-zone unit (32001) uses only the LED1 row and one converter.

| From | To | Purpose |
| --- | --- | --- |
| 12 V power supply `+` / `-` | Dig-Quad power input `+` / `-` | Powers the controller and fused distribution |
| Dig-Quad fused `+12 V` and `GND` | Each Dig2Analog input `V+` and `GND` | Powers both converters and their strips |
| Dig-Quad `LED1` data | **Left** Dig2Analog input `Data` | Zone 1's level-shifted 5 V data signal |
| Dig-Quad `LED2` data | **Right** Dig2Analog input `Data` | Zone 2's level-shifted 5 V data signal |
| Each Dig2Analog `VCC_F` | Its strip's common `+12 V` | Supplies fused strip positive |
| Each Dig2Analog `R`, `G`, `B`, `W` | Its strip's `R`, `G`, `B`, `W` | Switches the four negative color channels |

LED output GPIOs by Dig-Quad revision ([v4 guide](https://quinled.info/quinled-dig-quad-pinout-guide/), [v2/v3 guide](https://quinled.info/quinled-dig-quad-pinout-guide-v3/)):

| Revision | LED1 | LED2 | LED3 | LED4 |
|---|---|---|---|---|
| v4 (QuinLED ESP32 v4, ESP32-PICO-V3-02) | GPIO2 | GPIO4 | GPIO12 | GPIO14 |
| v2/v3 (QuinLED-ESP32) | GPIO16 | GPIO3 (serial RX) | GPIO1 (serial TX) | GPIO4 |

Check the revision before adding a unit. To identify the chip, flash it once with any sketch: the upload log prints `Chip type` and `Embedded PSRAM` for a v4 module. On v2/v3, LED3 (GPIO1) is the serial transmit pin, so avoid it while you still need Serial Monitor; on a D1 Mini32, GPIO1 and GPIO3 can be swapped.

Set each Dig2Analog direction switch toward the three-pin terminal receiving data. Leave `W` disconnected for an RGB strip. For a two-wire single-color strip, connect strip positive to `VCC_F`, strip negative to one output (normally `W`), and use that channel's MQTT command.

Use wire and a Dig-Quad fuse sized for the lower of the expected load or the wire rating. A standard Dig2Analog has a 3 A self-resetting fuse on its strip output; do not plan a strip load above that board's documented limit. All 12 V connections in this setup use the same power supply and common ground.

The product does not require a screen or environmental sensor. Status and diagnostics are available through Serial Monitor at 115200 baud.

## Startup hardware test

On every boot, the firmware briefly lights red, green, blue and white at 50%, **left zone first, then right**, one output at a time, then turns everything off before connecting to Wi-Fi. This confirms each zone's Dig-Quad data output, Dig2Analog direction, power wiring and strip wiring without involving MQTT. If the right strip flashes first, LED1 and LED2 are swapped. Set `RUN_STARTUP_LIGHT_TEST` to `false` after commissioning if the boot flashes are not wanted in normal operation.

## MQTT message format

Every command payload is a **plain decimal number sent as text** (for example `1255`), not JSON. Payloads must be 1–31 characters. Trailing spaces or line endings are allowed. Empty, oversized, or non-numeric payloads are rejected and reported on the status topic.

Broker: `192.168.1.253:1883`. Client ID: `MrBox-<Wi-Fi MAC>`, so multiple boards never knock each other offline by sharing an ID.

### Command numbers (boxchat zone standard)

Every command is `block × 1000 + level`, where the level is 0–255. Each zone owns four blocks in a row (red, green, blue, white), and the next zone continues counting:

| Payload | Zone 1 (Left) | Zone 2 (Right) | Zone 3 | Zone 4 |
|---|---|---|---|---|
| Red 0–255 | `1000`–`1255` | `5000`–`5255` | `9000`–`9255` | `13000`–`13255` |
| Green 0–255 | `2000`–`2255` | `6000`–`6255` | `10000`–`10255` | `14000`–`14255` |
| Blue 0–255 | `3000`–`3255` | `7000`–`7255` | `11000`–`11255` | `15000`–`15255` |
| White 0–255 | `4000`–`4255` | `8000`–`8255` | `12000`–`12255` | `16000`–`16255` |

| Payload | Effect |
|---|---|
| `0` | Turn every channel of every zone off |
| above `32000` | Board activation number (shared topic only) |

So `1128` is left red at 128 (about 50%) and `5128` is right red at 128. In general, for zone *z* (1–4) and channel *c* (red 0, green 1, blue 2, white 3), the block is `4 × (z − 1) + c + 1`. Zone 1 is exactly the original single-zone protocol, so older boards and tools keep working. Setting one channel keeps every other level as it is. A board answers `unknown command` for a zone it doesn't have: 32004 has zones 1 and 2, and 32001 has only zone 1.

### Two ways to send commands

Each board has a unique activation number above 32000; `cmdActVal` takes it from `BUILD_FOR_UNIT`. The examples below use the two-zone unit `32004`.

**Direct topic, `boxchat/32004/set` (recommended).** Publish the command number and it is applied immediately, with no activation step.

```
boxchat/32004/set   4128    -> left white = 128
boxchat/32004/set   8128    -> right white = 128
```

To change both zones together, send the command for each zone (Light Console does this for you when **BOTH** is selected).

**Shared topic, `boxchat`.** Every board listens here, so arm the target board first and then send **exactly one** command:

```
boxchat   32004   -> board 32004 arms and publishes "ready" on its status topic
boxchat   4128    -> white = 128, then the board disarms again
```

- Activation lasts for one command only.
- Receiving another board's activation number disarms this board, so a command meant for one unit cannot change another.
- Commands sent to `boxchat` without arming the board first are ignored.

### What the board publishes

| Topic | Payload | Retained |
|---|---|---|
| `boxchat/<unit>/status` | `online`, or `offline` (the MQTT last-will message) | Yes |
| `boxchat/<unit>/status` | `ready`, `invalid payload length`, `invalid numeric command`, `unknown command` | No |
| `boxchat/<unit>/state` | JSON with every zone's levels (see below) | Yes |

State JSON keeps the original top-level `r`, `g`, `b`, `w` (always zone 1, so older listeners keep working) and adds a `zones` array in zone order:

```json
{"r":255,"g":0,"b":0,"w":0,"zones":[{"r":255,"g":0,"b":0,"w":0},{"r":0,"g":0,"b":255,"w":0}]}
```

Single-zone devices such as the OpenRGB bridge (32003) may still send only the flat object; Light Console treats that as zone 1. The board publishes state after every applied command and again on each reconnect. Because it is retained, new subscribers see the current colors right away.

### Other boards on the network

| ID | Name in the web UI | Kind | Protocol |
|---|---|---|---|
| `32001` | RGBW Light | One Dig2Analog RGBW zone (older build of this firmware) | Direct or shared, as above; zone 1 commands only |
| `32002` | Relay Lights | Relay receiver | Shared `boxchat` only: activation `32002`, then `1` (on) or `0` (off). No state acknowledgement |
| `32003` | Mnemerynne Server | MQTT-to-OpenRGB bridge on the Mnemerynne server | Same standard as `32001`: direct `boxchat/32003/set` or shared activation `32003`; reports on `boxchat/32003/status` and `boxchat/32003/state` |
| `32004` | Room Lights (zones Left, Right) | Two Dig2Analog RGBW zones (this firmware, `BUILD_FOR_UNIT 32004`) | Direct or shared, as above; zones 1 and 2 |

## Configuration points

- `BUILD_FOR_UNIT` picks the unit. Each unit's `#if` block sets `ZONE_COUNT`, `ZONE_DATA_PINS` and `ZONE_NAMES` in protocol order. To add a unit, take the next free activator number from the NAS boxchat reference and add a block. The compiler refuses a zone count outside 1–4, or a pin or name list of the wrong length.
- `DIG2ANALOG_BOARD_COUNT` is `1` per zone. Increase it only when daisy-chaining converters on one output that should mirror the same zone.
- `MQTT_BUFFER_SIZE` is `512` bytes, enough for the state JSON of four zones. PubSubClient's 256-byte default is not.
- `DIG2ANALOG_PIXEL_TYPE` is `NEO_WRGB + NEO_KHZ800`. WLED uses a native GRBW driver; after QuinLED's required `BRG` order and `W & G` swap are applied, the transmitted byte sequence is W,R,G,B.
- `RUN_STARTUP_LIGHT_TEST` controls the diagnostic RGBW flashes at boot.
- `cmdActVal` comes from `BUILD_FOR_UNIT`, so each unit's activator number is unique as long as each unit has its own block.
- Update the Wi-Fi and MQTT constants in `ssd1306_128x64_i2c.ino` for the installation network.

Hardware references: [Dig-Quad pinout](https://quinled.info/quinled-dig-quad-pinout-guide/), [Dig2Analog wiring](https://quinled.info/quinled-dig2analog-wiring-guide/), and [Dig2Analog specifications](https://quinled.info/quinled-dig2analog-pre-assembled-specifications/).

## Companion web app

The `python` directory contains a zero-dependency local-network control panel. It supports RGBW sliders and presets for boards 32001 and 32003, and for the two-zone unit 32004 adds **BOTH / LEFT / RIGHT** tabs to control the zones together or separately, with a live swatch per zone. It also supports relay on/off control for board 32002, global all-off control, broker status, and retained RGBW state acknowledgements.

Start it on Windows with `py python/app.py`, then open `http://localhost:8080`. See `python/README.md` for LAN access, board configuration, MQTT modes, and security notes.

For automatic startup before Windows sign-in, use `ps/install_light_console_startup.ps1` from an administrator PowerShell with your Python interpreter path. See [boot installation and task management](python/README.md#start-automatically-at-windows-boot).

## Erinne lighting API

The companion app provides authenticated light discovery (including each light's named zones), RGBW color/channel changes for one zone or all zones, and power control for all configured boards. Importable read/control templates match the API contract in `C:\scripts\erinne`. See [setup and API documentation](python/ERINNE_API.md).
