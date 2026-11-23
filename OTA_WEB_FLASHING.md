# OTA / web flashing plan (wall-mounted ESP32 boards)

Goal: update the wall-mounted ESP32 boards (MrBox units **32001**, **32004**, and the
**32006** button panel) over Wi-Fi instead of USB, since they're already mounted.

Status: **2026-11-22 — code is staged and the MrBox sketch compiles. Nothing has been
flashed to any board yet. The wall boards are untouched and running their old firmware.**
Picking this up later = set a password, do one USB flash per board, then OTA forever.

## The one catch (why a USB flash is still needed once)

OTA only works if OTA code is **already running** on the board. The wall boards are running
firmware **without** OTA, so each one needs **exactly one more USB flash** to install it.
After that first flash, every future update goes over Wi-Fi. There is no way around the
first wired flash — it's chicken-and-egg.

## What was chosen

- **Two update paths** (both added, both password-protected):
  - **ArduinoOTA** — push a build over Wi-Fi with `arduino-cli upload` (how Lumi will push).
  - **ElegantOTA** — a browser page at `http://<host>/update` to drag-drop a `.bin` (for Doll).
- **mDNS hostnames** so DHCP IPs don't matter:
  - MrBox units: `mrbox-<id>.local` (e.g. `mrbox-32004.local`, `mrbox-32001.local`)
  - Button panel: `btnpanel-32006.local`
- **Dual-app OTA partition** (`PartitionScheme=min_spiffs`, 1.9 MB app + OTA) so a failed or
  interrupted upload rolls back to the firmware that's still running — a bad flash can't brick
  a board on the wall.
- OTA is started **before MQTT** (and serviced during MQTT reconnect waits), so a board stays
  reflashable even if the broker is down — exactly when you'd need to fix it.

## What is already in the repo (done)

- `ota.ino` (MrBox) and `button_panel/ota.ino` — define `startOTA()` + `handleOTA()`.
- `ssd1306_128x64_i2c.ino` and `button_panel/button_panel.ino` — call `startOTA()` in `setup()`
  (after Wi-Fi, before MQTT) and `handleOTA()` in `loop()`.
- `mqtt.ino` — services OTA during the 5 s reconnect wait.
- **ElegantOTA 4.0.0** library installed (`arduino-cli lib install ElegantOTA`).
- MrBox sketch **compiles clean** with the OTA partition (exit 0). The button panel still needs
  a compile check (identical OTA code, so expected fine).

## TODO when picking this back up

1. **Set the OTA password.** Both sketches default to `OTA_PASSWORD "boxchat-ota"` (a placeholder)
   in their `ota.ino`. Change it to something of Doll's choosing (one `#define`, same value in
   both), or pass `-DOTA_PASSWORD="..."` at build time. The web page user is `doll`.
   - Note: Wi-Fi creds and this password live in the sketch source. Before any git commit, move
     them to a git-ignored header.
2. **Compile-check the button panel:**
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs button_panel
   ```
3. **The one USB flash per board** (installs OTA). Pop the board off the wall or reach its USB:
   ```
   # MrBox unit (pick the right unit via BUILD_FOR_UNIT in ssd1306_128x64_i2c.ino):
   arduino-cli compile --upload -p COMx --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs .
   # Button panel:
   arduino-cli compile --upload -p COMx --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs button_panel
   ```
   The board prints its OTA name on serial, e.g. `OTA ready: ... http://mrbox-32004.local/update`.
4. **From then on, update over Wi-Fi** (no USB):
   - Web page (Doll): open `http://mrbox-32004.local/update`, log in (`doll` + the password),
     drop the compiled `.bin` (found under the sketch's build output), done.
   - CLI push (Lumi):
     ```
     arduino-cli upload -p mrbox-32004.local --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
       --upload-field password=<OTA_PASSWORD> .
     ```
   - **Keep the same `PartitionScheme=min_spiffs`** on every later build, or the app won't fit the
     partition the board already has.

## Boards

| Board | Sketch | OTA host | Notes |
|---|---|---|---|
| 32001 Doll-Office Lights | `ssd1306_128x64_i2c.ino` (`BUILD_FOR_UNIT 32001`) | `mrbox-32001.local` | older v2/v3 board |
| 32004 Living Room Lights | `ssd1306_128x64_i2c.ino` (`BUILD_FOR_UNIT 32004`) | `mrbox-32004.local` | Dig-Quad v4 |
| 32006 Button Panel | `button_panel/` | `btnpanel-32006.local` | |
| 32007 Living Room Lamp | — (OpenBeken) | already has OTA at `/update` | the iHome plug |
| 32002 Doll-Office Relay | not in this repo | — | needs its source to add OTA |
