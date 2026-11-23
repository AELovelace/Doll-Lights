# Button Panel (boxchat unit 32006)

An ESP32 classic with an **LED&KEY** board (TM1638: 8 seven-segment digits, 8 LEDs, 8 buttons) that controls **Living Room Lights (32004)** over MQTT. It's a controller, not a light, so it isn't listed in Light Console's boards; it only reports `online`/`offline` on `boxchat/32006/status`.

| Button | Action | Display | LED |
|---|---|---|---|
| S1 (button 0) | Living Room off: sends `0` to `boxchat/32004/set` | idle readout (below) | LED 0 |
| S2 (button 1) | Living Room on: full white on both zones (`1000 2000 3000 4255 5000 6000 7000 8255`) | idle readout (below) | LED 1 |
| S3 / S4 / S5 | Build a colour for **All** / **Left** / **Right** | `rEd A---` / `rEd L---` / `rEd r---` | that button's LED |
| S6–S8 | Nothing outside a colour sequence | | |

**Building a colour:** after S3/S4/S5 the panel asks for **red**, then **green**, then **blue** (`rEd L---` → `Grn L---` → `bLuE L---`; `A` = All, `L` = Left, `r` = Right). Answer each with any button: **S1 = 0, S2 = 36, S3 = 73, S4 = 109, S5 = 146, S6 = 182, S7 = 219, S8 = 255**. Each answer is sent at once as zone-standard commands (Left R/G/B = `1xxx`/`2xxx`/`3xxx`, Right = `5xxx`/`6xxx`/`7xxx`, All = both), so the colour builds up live. The red answer also switches that section's white off (`4000`/`8000`) so the colour isn't washed out by the full white from S2. While the next question waits, the eight LEDs show the previous answer **in binary** (left LED = top bit, so the left four LEDs are the first hex digit and the right four the second: 255 = `FF` = all on, 182 = `B6` = ●○●● ○●●○). After blue, the display shows the finished colour **as hex**, side then RRGGBB (`L FF2449`), for 3 s, then returns to the idle readout. A question left unanswered for 5 s cancels the rest of the sequence (colours already sent stay). While a sequence is running, every button is a level key, so S1/S2 are not off/on.

**Idle readout:** when nothing is being picked, the display shows 32004's real levels from its retained `boxchat/32004/state` (so it follows Light Console too): digit 1 = **white** on (`1`) or off (`0`), digit 2 = **side** (`A` when Left and Right match, otherwise `L` and `r` alternate every 3 s), digits 3–8 = **RRGGBB in hex**. Examples: `1A000000` = full white only, both sides; `0LFF006D` / `0r00FF00` = different colours per side. LED 0 lights when everything is off, LED 1 when anything is on. Other messages: `boot`, `UUiFi` (joining Wi-Fi), `Conn` (connecting to MQTT), `Err` (MQTT failed; it retries every 5 s and the buttons keep working), `no nEt` (a button was pressed while offline, so nothing was sent), `----` (state not known yet).

## Wiring

| LED&KEY | ESP32 classic |
|---|---|
| VCC | **3.3 V** |
| GND | GND |
| STB | GPIO25 |
| CLK | GPIO26 |
| DIO | GPIO27 |

Power the LED&KEY from **3.3 V**, not 5 V. At 5 V the TM1638 expects 5 V logic, and its DIO line would put 5 V on an ESP32 pin, which is 3.3 V only. The display is a little dimmer at 3.3 V; raise `TM_BRIGHTNESS` (0–7) in `tm1638.ino` if needed.

## Build and flash

The sketch lives in this `button_panel/` folder. The MrBox sketch in the parent folder ignores it.

```
arduino-cli compile --upload -p COMx --fqbn esp32:esp32:esp32 button_panel
```

Board: **ESP32 Dev Module**. No extra libraries beyond PubSubClient; the TM1638 driver is in `tm1638.ino`. Wi-Fi and broker settings are at the top of `button_panel.ino`.

## Testing

1. Power up with Serial Monitor at 115200: expect `WiFi connected. IP: …` then `MQTT connected`, and `boxchat/32006/status` = `online`.
2. The idle readout should match Living Room Lights (e.g. `1A000000` after S2, `0A000000` after S1).
3. Press S1: Living Room turns off, display `0A000000`, LED 0 lit. Press S2: both zones full white, display `1A000000`, LED 1 lit.
4. Press S4 (Left), then S8, S1, S4: the left strip turns red, stays red, then gets blue added (`4000 1255`, `2000`, `3109`); the display ends on `L FF006D`. Press S3 (All) then S8, S8, S8: both strips white via RGB, `A FFFFFF`. Press S5 and wait 5 s: the sequence is cancelled and nothing is sent.
5. Change Living Room from Light Console: the display follows within a moment.
6. Unplug the panel: `boxchat/32006/status` becomes `offline` (last-will).
