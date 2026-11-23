# Master Light Console: Pin Map

Every pin on every board, what's on it, and what's still free. It matches the sketches as built
(2026-11-22). If you change a pin in code, change it here too. The overview and protocols live
in [README.md](README.md).

**Golden rules**
- **Join all the GNDs.** Each board runs on its own USB power; the GND wires below tie them together.
- **No resistors anywhere.** 5 V boards (Uno, Mega) reach the 3.3 V ESP32 only through **open-drain D2**
  (they pull LOW or let go). **Never drive Mega D2 or Uno D2 HIGH.**
- **Flash the Uno and the Mega before you connect their D2 wires.**
- **Knob power follows the board:** the MAIN knob goes to **3.3V** (ESP32), the NAV knob to **5V** (Mega).

```
                ┌── GPIO32/33 ──── CYD face            (3.3 V ↔ 3.3 V, 115200)
ESP32 brain ────┤
                └── GPIO5/15 ───── Mega hub ──┬── 16/17 ─────── Uno + LCD shield   (5 V ↔ 5 V, 9600)
                   (open-drain, 19200)        ├── 51/52/53 ──── MAX7219 dot matrix
                                              ├── A8/A9/24 ──── NAV knob
                                              ├── 22 ────────── panic button
                                              └── 13 ────────── L LED (heartbeat)
```

## Wire-by-wire checklist (for the case)

Tick each wire as it goes in. "→" shows which way the signal travels.

| # | From | To | What |
|---|---|---|---|
| 1 | CYD **19** | ESP32 **GPIO32** | CYD → brain |
| 2 | ESP32 **GPIO33** | CYD **18** | brain → CYD |
| 3 | ESP32 GND | CYD GND | |
| 4 | Mega **D2** | ESP32 **GPIO5** | Mega → brain (open-drain) |
| 5 | ESP32 **GPIO15** | Mega **19** (RX1) | brain → Mega |
| 6 | ESP32 GND | Mega GND | |
| 7 | Mega **16** (TX2) | Uno **D3** | Mega → Uno |
| 8 | Uno **D2** | Mega **17** (RX2) | Uno → Mega |
| 9 | Mega GND | Uno GND | |
| 10 | Mega **5V** | Matrix **VCC** | Use the module's **DIN** (input) end |
| 11 | Mega GND | Matrix **GND** | |
| 12 | Mega **51** | Matrix **DIN** | |
| 13 | Mega **52** | Matrix **CLK** | |
| 14 | Mega **53** | Matrix **CS** | |
| 15 | Mega **5V** | NAV knob **+** | |
| 16 | Mega GND | NAV knob **GND** | |
| 17 | NAV knob **CLK** | Mega **A8** | |
| 18 | NAV knob **DT** | Mega **A9** | |
| 19 | NAV knob **SW** | Mega **24** | |
| 20 | Mega **22** | Panic button | |
| 21 | Panic button | Mega GND | |
| 22–25 | Keypad pins **1, 2, 3, 4** (rows) | ESP32 **GPIO13, 14, 27, 26** | Pins counted left → right, keys facing you |
| 26–29 | Keypad pins **5, 6, 7, 8** (columns) | ESP32 **GPIO25, 23, 22, 21** | |
| 30 | ESP32 **3.3V** | MAIN knob **+** | **Never 5V** |
| 31 | ESP32 GND | MAIN knob **GND** | |
| 32 | MAIN knob **CLK** | ESP32 **GPIO18** | |
| 33 | MAIN knob **DT** | ESP32 **GPIO19** | |
| 34 | MAIN knob **SW** | ESP32 **GPIO4** | |

Leave these **unconnected**: Mega **18** (TX1, 5 V), Uno **D12** (claimed by the serial library),
CYD **RX0/TX0** (its USB serial). The keypad takes no power wires.

## ESP32 brain (ESP32 Dev Module, `lcd_keypad_esp32/`)

| GPIO | Used for | Set in |
|---|---|---|
| 4 | MAIN knob SW (push, LOW = pressed) | `encoder.ino` |
| 5 | Link RX ← Mega D2 (UART1, pull-up on) | `Console.h` |
| 13, 14, 27, 26 | Keypad rows R1–R4 | `keypad4x4.ino` |
| 15 | Link TX → Mega 19 (UART1) | `Console.h` |
| 18 | MAIN knob CLK | `encoder.ino` |
| 19 | MAIN knob DT | `encoder.ino` |
| 21, 22, 23, 25 | Keypad columns C4, C3, C2, C1 (pull-ups on) | `keypad4x4.ino` |
| 32 | Link RX ← CYD 19 (UART2, pull-up on) | `Console.h` |
| 33 | Link TX → CYD 18 (UART2) | `Console.h` |
| 1, 3 | USB serial (Serial Monitor) | built in |
| 6–11 | Internal flash: never touch | — |

**Free:** only awkward ones. 0, 2 and 12 are boot-mode pins. 16/17 are free on WROOM modules but
PSRAM on WROVER. 34–39 are input-only with no pull-ups. **New hardware goes on the Mega.**

## CYD face (ESP32-2432S028, `cyd_face/`)

| Pin | Used for | Notes |
|---|---|---|
| Header **18** | Link RX ← ESP32 GPIO33 | Shared with the microSD slot: leave the slot empty |
| Header **19** | Link TX → ESP32 GPIO32 | Same |
| 4 / 16 / 17 | RGB LED red / green / blue (on board, active LOW) | Glows the selected colour |
| RX0 / TX0 | USB serial | Don't wire |
| Screen, touch, backlight | On board (LovyanGFX autodetect) | Nothing to wire |

## Mega hub (Mega 2560, `mega_accessories/`)

| Pin | Used for | Set in |
|---|---|---|
| 0, 1 | USB serial (Serial Monitor) | built in |
| **2** | Link TX → ESP32 GPIO5, **open-drain bit-bang** (never HIGH) | `mega_accessories.ino` |
| 13 | L LED: heartbeat (solid = boxchat up, blinking = offline) | brain, `MODE 13 OUT` |
| 16 (TX2) | Uno link → Uno D3 | `mega_accessories.ino` |
| 17 (RX2) | Uno link ← Uno D2 (pull-up on) | `mega_accessories.ino` |
| 18 (TX1) | **Unused, leave empty** (5 V) | — |
| 19 (RX1) | Link RX ← ESP32 GPIO15 | `mega_accessories.ino` |
| 22 | Panic ALL OFF button to GND (pull-up on) | brain, `MODE 22 PULLUP` |
| 24 | NAV knob SW (knob 0) | `knobs.ino` |
| 26 / 28 / 30 | Spare knob 1 / 2 / 3 SW | `knobs.ino` |
| 50 | SPI MISO: the SPI hardware claims it, so don't use it (the matrix sends only) | — |
| 51 / 52 / 53 | Dot matrix DIN / CLK / CS | `matrix.ino` |
| A8 / A9 | NAV knob CLK / DT (knob 0) | `knobs.ino` |
| A10 / A11 | Spare knob 1 CLK / DT | `knobs.ino` |
| A12 / A13 | Spare knob 2 CLK / DT | `knobs.ino` |
| A14 / A15 | Spare knob 3 CLK / DT | `knobs.ino` |

**Free for new accessories** (the brain can drive them with `MODE`/`DW`/`PWM`/`DR`/`AR`/`WATCH`,
no Mega reflash): **3–12** (all PWM), **14, 15** (Serial3), **20, 21** (I²C SDA/SCL), **23, 25, 27, 29,
31–49** (44–46 are PWM), and analog **A0–A7**. The sketch refuses commands on reserved pins
(0–2, 16–19, 51–53 and all knob pins).

**Adding a knob:** wire it to the next spare slot (CLK/DT/SW above, + to 5V) and give it a job in
`handleMegaLine()` in the brain. It arrives as `KN <knob> <clicks>` and `KP <knob>`.

## Uno detail screen (Uno + LCD Keypad Shield, `lcd_keypad_uno/`)

| Pin | Used for | Notes |
|---|---|---|
| 0, 1 | USB serial | built in |
| **2** | Link TX → Mega 17, **open-drain bit-bang** (never HIGH) | |
| 3 | Link RX ← Mega 16 (SoftwareSerial, pull-up switched off) | |
| 4, 5, 6, 7 | LCD D4–D7 | Shield |
| 8 | LCD RS | Shield |
| 9 | LCD EN | Shield |
| 10 | Backlight transistor | **Leave alone** (driving it can overload the pin) |
| 12 | SoftwareSerial's dummy TX | **Leave unconnected** |
| A0 | 5 shield buttons (resistor ladder) | RIGHT ≈0, UP ≈206, DOWN ≈407, LEFT ≈625, SELECT ≈824 |

**Free:** 11, 13 (L LED), A1–A5. The shield's RST button just resets the Uno.
