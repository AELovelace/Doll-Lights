# Contributor Guide

The sketch is split into Arduino tabs:

- `ssd1306_128x64_i2c.ino`: hardware constants (zones, pins, names), shared state, and the main loop
- `inits.ino`: network, MQTT, and per-zone Dig2Analog initialization
- `led.ino`: per-zone RGBW state retention and WS28xx transmission
- `mqtt.ino`: message parsing, zone-standard command decoding, and per-zone state publishing

Vocabulary: a **zone** is one Dig-Quad LED output with its own Dig2Analog and strip (one ESP32 GPIO). A **channel** is one of that converter's four outputs (R, G, B, W), not a GPIO pin. Keep the Dig2Analog abstraction in `led.ino`. Any new control source should call `setZoneChannel()` or `setZoneLevels()` so all unchanged values in that zone are preserved when its one-pixel frame is resent; `setAllZones()` is for whole-board changes such as all-off.

The boxchat zone standard is shared by every device and by Light Console: command = `block × 1000 + level` with `block = 4 × (zone − 1) + channel + 1`, so zone 1 is `1000`–`4255` (the original protocol), zone 2 `5000`–`8255`, zone 3 `9000`–`12255`, zone 4 `13000`–`16255`, and `0` turns off every zone. Never reuse these ranges for something else, and keep zone 1 identical so single-zone devices stay compatible. State JSON keeps top-level `r`/`g`/`b`/`w` equal to zone 1 and lists every zone under `zones`. Each physical unit has its own `#if BUILD_FOR_UNIT == ...` block (currently 32004 with Left/Right and 32001 with one zone), and `cmdActVal` comes from `BUILD_FOR_UNIT`. Never change an existing unit's block to suit a new unit; add a new block with the next free activator number instead. Inside a block, `ZONE_COUNT`, `ZONE_DATA_PINS` and `ZONE_NAMES` must agree; the `static_assert`s stop mismatched lists from compiling, and an unknown unit hits `#error`. Update the Light Console `zones` entry and the NAS "MQTT Light Devices - boxchat" reference too.

MQTT shared-topic commands on `boxchat` require the unique `cmdActVal` followed by one channel command. Direct commands use the board-specific `boxchat/<cmdActVal>/set` topic. Keep acknowledgements off command topics to prevent loops; use `boxchat/<cmdActVal>/status` and `boxchat/<cmdActVal>/state` so retained messages from different boards cannot overwrite each other.

When changing board pins, cross-check the exact Dig-Quad revision. On v4 (QuinLED ESP32 v4, an ESP32-PICO-V3-02 with built-in PSRAM), LED1–LED4 are GPIO2, 4, 12 and 14, and GPIO16/17 must never be driven because they belong to the PSRAM; doing so causes a watchdog reset loop. On v2/v3 with the QuinLED-ESP32, LED1–LED4 are GPIO16, 3, 1 and 4. GPIO3 and GPIO1 are the UART receive and transmit pins, so `Serial.begin()` runs before `startPins()`, and using LED3 (GPIO1) silences Serial output. LED1 through LED4 are unidirectional level-shifted outputs and cannot be repurposed as sensor inputs. Update both `README.md` and `TESTING_GUIDE.md` whenever wiring, command ranges, power assumptions, or startup behavior changes.

This product intentionally has no OLED, LCD, or DHT dependency. Keep operational reporting on the USB Serial connection unless the product hardware specification changes.

The companion control panel lives in `python/`. `app.py` owns configuration, HTTP routes, board command mapping, and state storage; `mqtt_client.py` implements the dependency-free MQTT 3.1.1 transport. Browser assets belong under `python/templates` and `python/static`, PowerShell launchers belong under `ps`, and all new Python scripts and tests must remain under `python`.

The browser interface is intentionally self-contained. Keep visual assets in `python/static/`; do not add CDN dependencies because the controller must remain usable on an isolated local network. The current original sakura-CRT theme is defined in `python/static/styles.css` and `python/static/sakura-terminal.svg`.

Keep API validation server-side even when the browser already constrains a value. MQTT lighting commands must not be queued while disconnected because stale commands could unexpectedly operate lights after a broker reconnect. Run `py -m unittest discover -s python/tests -v` after changes to either MQTT or HTTP behavior.

Web-panel board entries use `kind: "rgbw"` for channel controllers and `kind: "relay"` for on/off receivers. An RGBW entry may list `zones` (1–4 unique names in protocol order; it defaults to one zone named `Main`). `app.py` validates them and encodes commands with `zone_command()`. Requests without a zone (or with `"zone": null`) go to every zone, and all of a request's zone commands are sent under the command lock. In `static/app.js`, `card.target` is `null` for "together" or a 1-based zone number, and `card.zoneValues` holds each zone's last known levels. A board may override the global MQTT command mode. Relay 32002 uses the shared `boxchat` protocol, so the server must publish activator `32002` and command `1` or `0` as one locked pair to prevent interleaving across HTTP request threads.

## Windows startup

Windows boot setup lives in `ps/install_light_console_startup.ps1`; its task calls `ps/start_light_console.ps1` with an absolute Python path and log directory. Preserve the LOCAL SERVICE account, boot trigger, duplicate-instance protection, failure retries, and unlimited runtime. Keep the launcher's kill-on-close job (stopping the task must also stop Python) and `ExclusiveHTTPServer` in `python/app.py` (a second server must fail rather than share the port); without them a restart can leave an old server answering with stale configuration. Use `ps/restart_light_console.ps1` to apply configuration changes. Keep startup instructions in `python/README.md` and checks in `TESTING_GUIDE.md` current. Never put API tokens in task arguments or tracked files. Changes to installation require an administrator PowerShell; ordinary interactive launch does not.

## External lighting API

`python/lighting_api.py` owns Erinne discovery, credential scopes and typed actions; it delegates hardware changes to `LightApplication`. Keep numeric IDs dynamic and advertise hardware capabilities. Never implement color by guessing which relay to switch. Unknown telemetry must stay null, and HTTP acceptance must not claim hardware confirmation.

When credentials are configured, protect legacy browser API routes as well as external routes. Keep tokens outside templates/configuration returned to clients. Complete color operations use the reentrant command lock; shared activation failure must stop publication. On MQTT disconnect, clear queued work under the publish lock.

Generate both importable JSON files through `python/export_erinne_templates.py`; every write must have `action: true`, numeric placeholders must render as JSON integers, and reads/changes use separate credentials. Update `python/ERINNE_API.md` and run HTTP tests plus the optional real Erinne contract test after changing the contract.
