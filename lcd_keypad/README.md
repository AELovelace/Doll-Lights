# Master Light Console (boxchat 32008)

Doll's physical control panel for the boxchat lights. An **ESP32** (the brain) joins Wi-Fi and the boxchat MQTT broker, follows every light's live state, and gives **every part of the panel a job**:

| Part | Board | Job |
|---|---|---|
| 🧠 Brain | **ESP32 Dev Module** (`lcd_keypad_esp32/`) | Wi-Fi + boxchat as **controller 32008**, live light state, owns the *selection* (light, zone, channel) |
| 🖥️ Face | **CYD** 2.8" touch, ESP32-2432S028 (`cyd_face/`) | 4 light tiles with live colour, zone tabs, BRI/R/G/B/W bars (tap = knob channel, drag = set level), ⏻ power badges, pop-ups; its back RGB LED glows the selected colour |
| 🎛️ MAIN knob | KY-040 on the ESP32 | **Turn** = adjust the selected channel, **push** = next channel (BRI → R → G → B → W) |
| 🔢 Keypad | 4x4 matrix on the ESP32 | **Digits** type a level 0–255 for the selected channel (the Uno's top row shows `#SET R   128_`, the CYD pops it up), **#** applies it. With nothing typed, **#** = the selected room's **relay ON** and **\*** = **relay OFF** (lights without a relay just switch on/off); while typing, **\*** cancels. **A** WHITE, **B** SUNSET, **C** VIOLET, **D** = **ALL OFF** (relays too). Lights and zones are picked on the CYD, Uno buttons or NAV menu |
| 🔌 Hub | **Mega 2560** (`mega_accessories/`) | 5 V accessories, the dot matrix, the NAV knob, and the Uno relay |
| 🎛️ NAV knob | KY-040 on the Mega | Drives the Uno LCD menu: **turn** = move / change, **push** = enter / confirm |
| 📟 Detail screen | **Uno** + LCD Keypad Shield (`lcd_keypad_uno/`) | Top row: NAV menu. Bottom row: the selection's `R255G060B000W000`, with the knobs' channel blinking. **UP/DOWN** = channel (BRI → R → G → B → W, like the CYD's bar rows), **LEFT/RIGHT** = light (like the CYD's tiles), **SELECT** = next zone (BOTH → LEFT → RIGHT, on the Living Room) |
| 🔴 Dot matrix | MAX7219, 2 × 8x8, on the Mega | Level meter: R, G, B, W bars (top to bottom, 2 rows each) filling **left → right**; the knobs' channel blinks, and on BRI all four blink |
| 💡 Mega L LED | Mega pin 13 | Network heartbeat: **solid** = on boxchat, **blinking** = offline |
| 🔘 Panic button | Mega pin 22 → GND | **ALL OFF** |

## The lights

The same boards as Light Console's `python/config.json`, as of 2026-11-22:

| Key | Tile | Board | Zones | Master power relay |
|---|---|---|---|---|
| 1 | Doll-Office | 32001 | 1 | 32002 (shared-mode relay) |
| 2 | Mnemerynne | 32003 | 1 | — |
| 3 | Living Room | 32004 | Left / Right | 32007 (Living Room Lamp plug) |
| 4 | Cync Strip | 32005 | 1 | — |

To change them, edit `lights[]` in `lcd_keypad_esp32/Console.h`. Each entry is the id, full name (CYD), short name (5 letters, Uno LCD), zone count, relay id (0 = none), and whether the relay only understands the shared topic.

## What everything does

**The selection** is the light, the zone (BOTH, or LEFT/RIGHT on the Living Room) and the **channel**. Every screen shows it, and every input acts on it:

- **BRI** scales the whole colour and keeps its hue. **R / G / B / W** set one level.
- **On** (NAV POWER menu, or `#` on a light without a relay) brings back the light's last colour at full brightness (white if it was never lit). **Off** sets it to 0 but remembers the colour.
- **Scenes** (keypad A–C, or the NAV menu's SCENE item): **A** WHITE, **B** SUNSET, **C** VIOLET; AQUA is in the NAV menu only (the same colours as Light Console's presets).
- **ALL OFF** (keypad `D`, panic button, NAV menu): every light gets the firmware all-off `0`, and both master relays switch off.
- **Power badge ⏻** on a CYD tile switches that light's master relay. For a light without a relay, it toggles the light on/off. Colour: green = on, red = off, grey = unknown (relays don't report their state, so the console shows the last command it sent).

**Knob speed:** turning slowly moves 4 levels per click, spinning fast moves 16.

### NAV menu (Uno LCD, NAV knob)

Turn to move through the items, push to edit one (`>` becomes `*`), turn to change it, and push again to finish. An item left alone for 10 s stops editing.

| Item | Turning while editing | Push to finish |
|---|---|---|
| LIGHT | Picks a light | — |
| ZONE | BOTH / LEFT / RIGHT (Living Room only) | — |
| CHAN | BRI / R / G / B / W (what the knobs adjust) | — |
| LEVEL | Adjusts that channel (with acceleration) | — |
| SCENE | Picks a scene name | **Applies** the scene |
| POWER | Clockwise = on, anticlockwise = off (the relay, or the light itself) | — |
| MATRIX | Dot-matrix brightness 0–15 | — |
| ALL OFF | — | **Everything off** (shows `PUSH!` while armed) |

While the console is off boxchat, the bottom row alternates with `NO BOXCHAT LINK`.

## Wiring

> 📌 **Every pin on every board, plus a numbered wire-by-wire checklist for the case: [PINMAP.md](PINMAP.md).**

All GNDs are joined. Every board has its own USB power. **No resistors anywhere:** 5 V boards talk to 3.3 V boards with an *open-drain* trick (the 5 V side only ever pulls the line LOW, and the 3.3 V side's pull-up makes HIGH = 3.3 V).

```
                ┌── UART2 GPIO32/33 ── CYD face          (3.3 V ↔ 3.3 V, 115200)
ESP32 brain ────┤
 + MAIN knob    └── UART1 GPIO5/15 ─── Mega hub ──┬── Serial2 16/17 ── Uno + LCD shield   (5 V ↔ 5 V, 9600)
 + 4x4 keypad      (open-drain, 19200)            ├── SPI 51/52/53 ─── MAX7219 dot matrix
                                                  ├── A8/A9/24 ──────── NAV knob
                                                  ├── 22 ───────────── panic button
                                                  └── 13 ───────────── L LED (heartbeat)
```

### ESP32 brain

| ESP32 | To | Notes |
|---|---|---|
| GPIO32 | CYD **19** | CYD → brain |
| GPIO33 | CYD **18** | brain → CYD |
| GPIO5 | Mega **D2** | Mega → brain, open-drain |
| GPIO15 | Mega **19** (RX1) | brain → Mega |
| GPIO13, 14, 27, 26 | Keypad pins 1–4 (rows) | Pins counted left → right, keys facing you |
| GPIO25, 23, 22, 21 | Keypad pins 5–8 (columns) | |
| GPIO18 / 19 / 4 | MAIN knob CLK / DT / SW | Knob **+** to **3.3V**, never 5V |

The ESP32 is full. The only spares are its boot-mode pins (0, 2, 12), 16/17 (PSRAM on WROVER modules) and the input-only 34–39. New accessories go on the Mega.

### CYD face

Use the pins labelled **18** and **19** on the CYD's header (shared with the microSD slot, so leave the slot empty), plus GND. Don't use RX0/TX0 (its USB serial) or 17 (the blue part of its RGB LED).

### Mega hub

| Mega | To | Notes |
|---|---|---|
| D2 | ESP32 GPIO5 | Open-drain: never drive D2 HIGH in the Mega sketch |
| 19 (RX1) | ESP32 GPIO15 | Leave **18 (TX1)** unconnected (it's 5 V) |
| 16 (TX2) | Uno **D3** | |
| 17 (RX2) | Uno **D2** | The Mega's pull-up is on; the Uno only pulls LOW |
| 51 / 52 / 53 | Dot matrix DIN / CLK / CS | Matrix VCC → 5V. Use the module's **DIN** end |
| A8 / A9 / 24 | NAV knob CLK / DT / SW | Knob + → 5V (the Mega is 5 V) |
| 22 | Panic button → GND | |
| 13 | (onboard L LED) | Heartbeat |

**Spare knob slots:** the Mega supports 4 knobs on port K (all on one interrupt, so no lost clicks): knob 1 = A10/A11/26, knob 2 = A12/A13/28, knob 3 = A14/A15/30. Wire one up and give it a job in `handleMegaLine()` (`KN <knob> <clicks>`, `KP <knob>`).

### Uno detail screen

The LCD Keypad Shield sits on the Uno. **D2** → Mega 17, **D3** → Mega 16, GND → GND. Leave **D12** unconnected (the serial library claims it). The shield uses D4–D10 and A0; leave **D10** (backlight) alone.

> ⚠️ **Flash the Uno and Mega before connecting their D2 wires.** An old sketch might drive D2 to 5 V, which the ESP32 can't take.

## Build and flash

| Sketch | Board (`--fqbn`) | Port on Doll's PC |
|---|---|---|
| `lcd_keypad_esp32` | `esp32:esp32:esp32` (ESP32 Dev Module) | COM14 |
| `cyd_face` | `esp32:esp32:esp32` | COM13 since the case build (was COM18; depends on the USB port) |
| `mega_accessories` | `arduino:avr:mega:cpu=atmega2560` | COM16 or COM17 (depends on the USB port) |
| `lcd_keypad_uno` | `arduino:avr:uno` | COM15 |

```
arduino-cli compile --upload -p COM14 --fqbn esp32:esp32:esp32 lcd_keypad/lcd_keypad_esp32
```

Libraries: **PubSubClient**, **ArduinoJson 7** (brain), **LovyanGFX** (CYD), and LiquidCrystal, SoftwareSerial and SPI (built in). If `arduino-cli` can't find `LiquidCrystal.h`, add `--libraries "%LOCALAPPDATA%\Arduino15\libraries"`.

**Wi-Fi:** the brain reads `lcd_keypad_esp32/secrets.h`, which is **git-ignored**. Copy `secrets.example.h` to `secrets.h` and fill in the network. Doll's copy has the same Wi-Fi as the Button Panel.

**Flashing trouble:**
- An ESP32 that doesn't show a COM port: try a different cable (charge-only cables are common).
- An ESP32 that shows a port but `No serial data received`, and prints nothing at all after a reset: its 3.3 V is being pulled down. Check for a stray wire on 3.3V/GND/EN.

## How the boards talk

All links are text lines ending in `\n`. Each Arduino/CYD says `HI` when it starts, so the brain resends everything it needs. The brain pings every link every 5 s (`PING` → `PO`) and prints `... link up` / `... link lost` on its Serial Monitor (115200).

### boxchat (MQTT, broker 192.168.1.253:1883)

- **Publishes** zone-standard numbers to `boxchat/<id>/set`: zone *z*, channel *c* (R G B W = 0–3), level *v* → `(4(z−1)+c+1)·1000+v`. It also sends `0` for all-off and `1`/`0` to the relays. The 32002 relay only understands the shared topic: `boxchat` ← `32002`, then `1`/`0`.
- **Subscribes** to `boxchat/+/state` (live colours) and `boxchat/+/status` (online/offline).
- **Announces** `boxchat/32008/status` = `online` (retained), with last-will `offline`.
- Knob changes update the screens instantly and are published at most every 50 ms per light, only for channels that changed. For 800 ms after its own change, the brain ignores that light's state echoes, so a slow echo can't yank the knob backwards.

### Brain ↔ Mega (19200 baud)

| Brain → Mega | Meaning |
|---|---|
| `MODE <pin> OUT\|IN\|PULLUP`, `DW <pin> 0\|1`, `PWM <pin> 0-255`, `DR <pin>`, `AR <n>`, `WATCH <pin>`, `UNWATCH <pin>`, `RESET` | General accessory I/O (pins are Mega numbers; reserved pins are refused with `ERR`) |
| `MXCLR`, `MXBRI <0-15>`, `MXPIX <x> <y> 0\|1`, `MXROW <y> <hhhh>`, `MXALL <32 hex>`, `MXTEST` | Dot matrix (16x8, row 0 top, bit 15 = leftmost) |
| `U <line>` | Passed on to the Uno |

| Mega → Brain | Meaning |
|---|---|
| `IN <pin> <v>`, `DV <pin> <v>`, `AV <n> <v>` | Watched input changed / read answers |
| `KN <knob> <clicks>`, `KP <knob>` | Knob turned (bundled every 30 ms) / pushed |
| `U <line>` | From the Uno |
| `ERR <reason>` | Command refused |

### Brain ↔ Uno (through the Mega)

`T0:<text>` / `T1:<text>` (LCD rows), `CLR`, `PING`. The Uno sends `HI`, `PO`, and `KU` `KD` `KL` `KR` `KS` for button presses.

### Brain ↔ CYD (115200 baud)

| Brain → CYD | Meaning |
|---|---|
| `TN <i> <name>` | Tile name |
| `T <i> <online> <relay> <zones> <RRGGBBWW>...` | Tile state (relay: 1 on, 0 off, −1 unknown, 9 none) |
| `S <light> <zone> <channel>` | Selection |
| `K <net> <mega> <uno>` | Link chips |
| `MSG <text>` | Pop-up |

| CYD → Brain | Meaning |
|---|---|
| `TAP T <i>` / `TAP P <i>` | Tile / its power badge |
| `TAP Z <0\|1\|2>` | Zone tab (BOTH / LEFT / RIGHT) |
| `TAP C <ch>` / `TAP L <ch> <level>` | Channel label / bar tapped or dragged to a level |

## Calibration and tuning

- **Shield buttons:** this shield measured RIGHT 0, UP 206, DOWN 407, LEFT 625, SELECT about 824. The Uno logs `A0=<value> -> <letter>` on every press (Serial Monitor, 115200); adjust `decodeButton()` if a button reads wrong.
- **Dot-matrix orientation:** send `MXTEST` (e.g. temporarily from `megaSetup()`). You should see a long arm across the top-left and a short arm down the left edge, plus a dot bottom-right. Fix with the four `MATRIX_` switches in `matrix.ino`. On Doll's module, `MATRIX_FIRST_IS_LEFT = true`.
- **Knobs:** if one counts backwards, flip `ENCODER_REVERSE` (MAIN, `encoder.ino`) or `KNOB_REVERSE` (Mega, `knobs.ino`). If it only moves every second click, set its edges-per-click to 2.
- **CYD touch:** LovyanGFX's built-in CYD calibration is used. If taps land in the wrong place, that's the thing to tune.

## Testing

1. Flash all four boards (Uno and Mega first, before their D2 wires go on).
2. The brain's Serial Monitor shows `Master Light Console (32008) started`, then `boxchat connected`, `Mega link up`, `CYD link up` and `Uno link up (via Mega)`. The CYD's NET/MEGA/UNO chips turn green and the Mega's L LED goes solid.
3. The CYD tiles show the lights' real colours (from their retained state) and online dots. Change a light in the Light Console web app, and its tile follows.
4. On the CYD, tap the Living Room tile, then the LEFT tab. Turn MAIN: the left strip dims and brightens, and the CYD bar, Uno numbers and dot-matrix bars follow. Push MAIN and turn again: now just R changes.
5. **B**: Living Room left goes SUNSET and a SUNSET pop-up shows. With R selected, type **1 2 8 #**: R jumps to 128 (`#SET R   128_` on the Uno while typing). Type **5 \***: cancelled, nothing changes. Push MAIN back to BRI and type **0 #**: off. **\*** alone: the Living Room plug (32007) clicks off. **#** alone: back on.
6. CYD: tap the Cync tile, then drag the G bar. The strip follows your finger.
7. NAV: turn to **SCENE**, push, turn to AQUA, push. The selection goes aqua.
8. Tap the Living Room ⏻ badge: the lamp plug (32007) switches.
9. **D** (or the panic button): everything off, relays too, with an ALL OFF pop-up.
10. Unplug the router or broker: the L LED blinks and the Uno shows `NO BOXCHAT LINK`. Colour changes made while offline wait and go out on reconnect. ALL OFF and relay switches need the network, so they do nothing while offline.
