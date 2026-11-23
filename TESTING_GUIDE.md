# Hardware Testing Guide

These steps are written for the two-zone unit **32004** (`BUILD_FOR_UNIT 32004`). For the single-zone unit 32001, skip everything about the right zone and LED2. Before flashing, confirm `BUILD_FOR_UNIT` matches the unit on the bench; flashing a 32004 build onto 32001 would give it the wrong activator number.

## Before applying 12 V

1. Confirm power-supply polarity at the Dig-Quad input.
2. Confirm Dig-Quad LED1 reaches the **left** Dig2Analog's data input and LED2 reaches the **right** one's, each on the terminal selected by that converter's direction switch.
3. Confirm each strip's common positive wire goes to its converter's `VCC_F`, never to `R`, `G`, `B`, or `W`.
4. Confirm the Dig-Quad fuse and wire sizes suit the expected current.
5. Check for a short between `+12 V` and ground with a multimeter.

## First power-up

Start with a short strip section or current-limited supply. Open Serial Monitor at 115200 baud. The firmware first prints `Zone 1 (Left) data on GPIO 2` and `Zone 2 (Right) data on GPIO 4` (the Dig-Quad v4 pins). Then, before connecting to Wi-Fi, it briefly lights red, green, blue and white on the **left** strip, then the same four on the **right** strip, one at a time, printing `Hardware test zone N (...) <color>` for each, and turns everything off. The product has no screen or DHT sensor.

If the startup test works, the Dig-Quad/Dig2Analog hardware path is functional for both zones and any remaining problem is network or MQTT configuration. If one zone stays dark, inspect that zone's 12 V power, common ground, Dig2Analog direction switch, data connection (LED1 for left, LED2 for right) and strip polarity before debugging MQTT. If the right strip flashes during the "zone 1" lines, LED1 and LED2 are swapped in the wiring.

If Serial Monitor shows only repeating `rst:0x8 (TG1WDT_SYS_RESET)` boot lines and no `Zone ...` lines, the build drives a GPIO the chip can't give up. Most likely that's GPIO16/17 on a v4 board, where they belong to the built-in PSRAM. Check the board revision against the pin table in the README and fix `ZONE_DATA_PINS` for that unit.

For a longer local fade test, temporarily call `ledTest();` in `setup()`. The expected order is red, green, blue, then white on the left zone, then the same on the right, with each channel fading up and down. Remove the call afterward so normal MQTT startup is not delayed.

The expected Adafruit NeoPixel data order for the original Dig2Analog is `NEO_WRGB`. This is the W,R,G,B wire sequence produced when WLED applies QuinLED's documented `BRG` color order and `W & G` swap to its native GRBW RGBW driver. If colors are still permuted, verify the exact converter revision before changing this constant.

## MQTT acceptance test

For each row, publish activation `32004` to `boxchat` and then publish the listed command to `boxchat`:

| Expected result | Command |
| --- | --- |
| Left red off | `1000` |
| Left red full | `1255` |
| Left green full | `2255` |
| Left blue full | `3255` |
| Left white half | `4128` |
| Left white off | `4000` |
| Right red full (left unchanged) | `5255` |
| Right green full | `6255` |
| Right blue full | `7255` |
| Right white half | `8128` |
| `unknown command` on the status topic, no light change | `9255` (zone 3 does not exist on this board) |
| Both zones completely off | `0` |

After testing, confirm that `0` turned off every channel of both zones. Confirm that changing one channel never resets the other channels, and that a right-zone command never changes the left strip.

Alternatively, publish the command directly to `boxchat/32004/set` without activation. Watch `boxchat/32004/status` for `online` or `ready` and `boxchat/32004/state` for retained JSON acknowledgements. Also confirm unit 32001 does not react to any of these commands. The state must contain a `zones` array with two entries, left first, and its top-level `r`/`g`/`b`/`w` must always equal the left zone. If commands appear in Serial Monitor but state is not published, record the PubSubClient connection-state number printed by the firmware.

## Load test

Run the intended brightest sustained color for several minutes while monitoring supply voltage, connector temperature, strip temperature, and current. Stop immediately for flicker, odor, discoloration, hot terminals, or more than roughly 10% voltage drop at the far end of the strip.

## Companion web app

1. Run `py -m unittest discover -s python/tests -v` and confirm every test passes.
2. Run `py python/app.py` and open `http://localhost:8080`.
3. Confirm the header reports MQTT online and the correct broker address.
4. Confirm boards 32001 and 32004 report online after their firmware publishes its retained status.
5. Move each RGBW slider separately and confirm the board's state JSON follows the requested values.
6. On board 32004 (Room Lights), select **LEFT** and move a slider: only the left strip and the left swatch change, and MQTT shows a `1xxx`–`4xxx` command. Select **RIGHT** and repeat: only the right strip changes (`5xxx`–`8xxx`). Click a swatch and confirm it selects that zone's tab.
7. Select **BOTH**, pick a preset, and confirm both strips and both swatches show the same color and the readout loses its `MIXED` note. Make the zones different, select **BOTH** again, and confirm the readout says `MIXED` and the glow shows both colors side by side.
8. Confirm the single-zone boards (32001 and 32003) show no zone tabs and behave exactly as before.
9. Click `LIGHTS ON` for relay 32002 and confirm MQTT receives `32002` followed by `1` on `boxchat` and the physical relay turns on.
10. Click `LIGHTS OFF` and confirm MQTT receives `32002` followed by `0` and the physical relay turns off.
11. Open the app from a second device using the host computer's LAN IP and verify the controls remain synchronized.
12. Disconnect the MQTT broker and confirm the app reports offline and rejects commands instead of replaying them later.

## Windows boot task acceptance

1. Stop any manual server, then run the installer documented in `python/README.md` from administrator PowerShell.
2. Confirm `Get-ScheduledTask -TaskName 'Light Console Server'` reports `Running`, a boot trigger with a 30-second delay, and the LOCAL SERVICE principal (`S-1-5-19`).
3. Open `http://localhost:8080` and confirm `logs/stdout.log` contains the startup message; inspect `logs/stderr.log` for errors. This read-only check must not issue light commands.
4. Confirm task settings allow battery operation, ignore duplicate launches, have no execution time limit, and retry failures every minute.
5. At the next convenient reboot, verify the site responds from another LAN device before Windows sign-in, then check the task's last-run time. An immediate task start verifies the service account and launcher but does not replace this reboot check.
6. Run `ps/restart_light_console.ps1` as administrator and confirm it reports exactly one PID. Then run `Stop-ScheduledTask -TaskName 'Light Console Server'` and confirm `Get-NetTCPConnection -State Listen -LocalPort 8080` returns nothing, which proves no orphaned Python server survives a stopped task. Start the task again afterwards.
7. With the task running, start a second copy manually (`py python/app.py`) and confirm it exits with "Port 8080 is already in use" instead of running beside the first.
8. To disable the feature, stop and unregister only `Light Console Server` using the commands in `python/README.md`.

For a read-only installation check, run `powershell -ExecutionPolicy Bypass -File ps/test_light_console_startup.ps1` as administrator. It checks the running task, boot trigger, and HTTP page, and saves installed settings in `logs/startup-verification.json`. Administrator access is needed to inspect tasks owned by service accounts.

## Erinne API acceptance

Run `py -m unittest discover -s python/tests -v` for HTTP discovery, zone routing and zone validation, pagination, scope restrictions on both new and legacy routes, invalid inputs, per-board routing, concurrent colors, telemetry handling and MQTT disconnect cleanup. `test_erinne_contract.py` is skipped unless `ERINNE_ROOT` is set; run it using Erinne's virtual environment to exercise its real parser and request builder. See `python/ERINNE_API.md` for the exact command. Tests use fake MQTT and never operate lights.

For browser acceptance, configure both tokens and restart the app. Verify the password dialog, status with a read token, denied read-token writes, and successful controls with a control token after reloading. Import both templates into Erinne, discover light IDs and capabilities, and deliberately run a proposed color change on an RGBW board. Confirm a relay rejects color changes and supports on/off. Verify the correct physical board receives each action; 202 alone does not confirm hardware delivery. Disconnect/reconnect MQTT and verify unsent commands are not replayed.
