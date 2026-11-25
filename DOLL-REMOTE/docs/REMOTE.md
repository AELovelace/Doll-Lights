# DOLL-REMOTE: a pocket remote for Mnemerynne

DOLL-REMOTE is a slim fork of DOLL-OS for the M5Stack Cardputer ADV (ESP32-S3,
no PSRAM). It turns the Cardputer into a hardware remote for Mnemerynne's Media
screens (TVs and speakers). It uses the same remote-device API as the ESP32 touch
panel (Mnemerynne-tab).

It is a fork, not a DOLL-OS app. DOLL-OS runs short on RAM once TLS is in play,
so the fork keeps only what a remote needs.

| Kept from DOLL-OS | Cut from the fork |
| --- | --- |
| Status bar, scrollback, command bar (`terminal.ino`) | SD card, `ls`/`cd`/`cp`/... |
| Keyboard input, Fn-history (`hardware.ino`) | FTP server, USB mass storage |
| Wi-Fi scan/connect/save (`wifi.ino`) | Bluetooth keyboard (`btkbd`) |
| LittleFS settings | ssh, telnet, Motoko MQTT |
| LED pulses (`Led.ino`) | `.dapp` runtime, `apps`, Dapper, aliases, editor |
| Motoko MQTT chat (`motoko.ino`), unchanged | |

The fork also adds a **lights** app (`Lights.ino`) for the Doll-Lights boxchat
lights. See [Lights](#lights-doll-lights) below.

Scrollback is also smaller (50 rows of 48 chars instead of 120 rows of 96), and
the build uses the S3's built-in USB serial instead of TinyUSB.

## Files

| File | What it does |
| --- | --- |
| `DOLL-REMOTE.ino` | Boot: sprites, LittleFS, joins the saved Wi-Fi, opens the remote if paired |
| `Remote.ino` | Settings, HTTPS, pairing, the remote's pages and keys, and the `remote`/`pair`/`server`/`forget` commands |
| `System.ino` (also) | `g0Poll()` (G0 tap / 3 s hold) and `runApps()`, the remote ↔ lights switcher |
| `ca_roots.h` | ISRG Root X1 + X2, the CAs the server certificate is checked against |
| `CommandProcessor.ino` | Tokenizer, command history, command table |
| `System.ino` | LittleFS mount, heap checkpoints, `battery`/`free`/`uptime`/`reboot`/`status` |
| `terminal.ino`, `hardware.ino`, `Output.ino`, `wifi.ino`, `Led.ino` | From DOLL-OS, trimmed |
| `Lights.ino` | `lights`: boxchat MQTT light controller (Doll-Lights) |
| `motoko.ino` | `motoko`: MQTT chat client, from DOLL-OS unchanged |
| `config.h` | Default server address (`https://beepsquad.wtf`), Motoko and boxchat brokers |
| `sketch.yaml`, `partitions.csv` | Build profile (same board/partitions as DOLL-OS) |

## Build and flash

The sketch lives in the Doll-Lights repo at `DOLL-REMOTE/` (moved there from the
Arduino sketchbook on 2026-11-25). Build it with the `sketch.yaml` profile
`stamps3`: the board and partitions are the same as DOLL-OS, on esp32 core 3.3.10.
The profile pins the libraries by version (M5Cardputer 1.1.1, M5Unified 0.2.18,
M5GFX 0.2.25, ArduinoJson 7.4.3, PubSubClient 2.8). arduino-cli downloads them on
the first build, so it doesn't depend on the sketchbook's `libraries` folder.

- **Arduino IDE:** open `DOLL-REMOTE.ino`, pick the **stamps3** profile, then Upload.
- **arduino-cli** (the copy bundled with the Arduino IDE works):

```powershell
& "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" compile --profile stamps3 .
```

```powershell
& "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" upload --profile stamps3 -p COM5 .
```

Swap `COM5` for the Cardputer's port. Flashing this fork replaces DOLL-OS on the
device. LittleFS sits in the same place, so a saved `/wifi.cfg` carries over.

## The G0 button: remote, lights, shell

The remote is the default app, and it always opens at boot. The **G0** button on
the Cardputer moves between the apps:

| G0 | Does |
| --- | --- |
| tap, in the remote | switch to **lights** |
| tap, in lights | switch back to the **remote** |
| hold 3 s, anywhere | drop to the **shell** (it appears while you're still holding) |
| tap, in the shell | back to the **remote** |

Fn+Q also drops to the shell. The shell commands `remote` and `lights` open
either app, and G0 taps switch between them from there too.

At boot, with no Wi-Fi, the remote shows a **No Wi-Fi** page. It keeps retrying
the saved network in the background, so a router that was still starting up is
picked up by itself. If the remote isn't paired yet, it shows pairing.

## First-time setup

1. **Wi-Fi.** Hold G0 for 3 s to get the `remote >` prompt, then:
   ```
   wifi scan
   wifi connect "My Network" "my password"
   wifi save
   ```
   Quote names and passwords that have spaces. Once saved, the remote joins this
   network at every boot. Tap G0 to go back to the remote.
2. **Server** (optional). The default is `https://beepsquad.wtf`. For a
   different server, run `server https://other.host`. It must be `https://`.
   Changing the server unpairs the remote, because keys belong to one server.
3. **Pair.** Once it's on Wi-Fi, the unpaired remote asks the server for a code
   and shows it with a QR code:
   - On your phone, open Mnemerynne → **Media → Screens → Pair a TV or remote**.
   - Scan the QR code, or type the 8-character code (`ABCD-EFGH`), and name the remote.
   - The Cardputer checks back every 4 seconds. Once you approve, it saves its key and
     opens the remote.

   A code lasts 10 minutes. If it runs out, press Enter for a new one. A G0 tap
   switches to lights, and holding G0, Fn+Q, Del or `` ` `` cancels to the shell.
   The next time the remote opens, it shows a new code.

## Using the remote

The arrow keys are the Cardputer's printed arrows: `;` up, `.` down, `,` left,
`/` right. Fn is optional.

**Screens list:** up/down choose and Enter controls that screen. `r` refreshes now.
With only one screen, the remote goes straight to it. Grey screens are offline,
meaning Media isn't open on that TV. A guest TV shows as `Name @owner`.

**Now playing:**

| Key | Does |
| --- | --- |
The TV's functions are on the number row. Space, the arrows and Enter (OK) work
as they would on a TV remote.

| Key | Does |
| --- | --- |
| arrows | `press` up/down/left/right (moves around the TV page); the bottom bar shows "up sent" |
| Enter | OK (`press enter`) |
| Space | play / pause (`toggle`) |
| `-` / `=` | volume down / up 5% |
| `` ` `` | back to the screens list (Del too) |
| `1` | stop |
| `2` / `3` | previous / next |
| `4` / `5` | skip back / forward 30 s |
| `6` | mute |
| `7` | night filter, next step |
| `8` / `9` | next audio track / next subtitle track |
| `0` | channels (live streams and radio stations) |
| `g` | gamepad mode |
| `r` | refresh now |
| `h` | key help |
| G0 tap | switch to lights |
| G0 hold 3 s, Fn+Q | shell |

If presses say "sent" but the TV doesn't react, reload the TV's Media page. A TV
showing another page, or an old copy of Media, ignores remote presses.

**Channels:** up/down scroll through the list, six at a time, loading pages as you
go. Enter starts the channel on the selected screen. Del goes back.

**Gamepad mode** (`g`) sends a SNES-style pad to the TV. Each key sends `down`
when pressed and `up` when released, so held buttons stay held.

| Pad | Key |
| --- | --- |
| D-pad | `;` `.` `,` `/` |
| A (right) / B (bottom) / X (top) / Y (left) | `d` / `s` / `e` / `a` (the rows are staggered, so `e` sits above `s`/`d`) |
| L / R | `q` / `r` |
| Start / Select | Enter / Space |

Fn+Q or `` ` `` leaves gamepad mode and releases any held buttons. Each press is
an HTTPS request, so expect some lag. A very quick tap that starts and ends during
the previous request can be missed.

## Shell commands

| Command | Does |
| --- | --- |
| `remote` | open the remote (pairs first if needed) |
| `pair` | pair again; the owner approves a new code |
| `server [url]` | show or set the server; shows whether it's paired (never the key) |
| `forget` | remove this remote on the server, then wipe the key here |
| `forget local` | wipe the key here only (also remove it under Media → Screens) |
| `lights [broker-ip] [port]` | the light controller (default broker `192.168.1.253:1883`) |
| `motoko [broker-ip] [port]` | Motoko MQTT chat, as in DOLL-OS (`/quit` leaves) |
| `wifi ...` | `scan`, `connect <ssid> <pass>`, `save`, `disconnect`, `forget` |
| `status`, `battery`, `free [details]`, `uptime`, `reboot`, `clear`, `help` | system |

## Lights (Doll-Lights)

`lights` controls the boxchat MQTT lights from `C:\Scripts\Doll-Lights`. It's a
port of that project's master keypad console (`lcd_keypad/lcd_keypad_esp32`), with
the same light list, scenes and publishing rules.

| Light | boxchat id | Zones | Power relay |
| --- | --- | --- | --- |
| Doll-Office | 32001 | 1 | 32002 (shared topic: arm, then `1`/`0`) |
| Mnemerynne | 32003 | 1 | none |
| Living Room | 32004 | Left, Right | 32007 |
| Cync Strip | 32005 | 1 | none |

**Adding rooms and scenes.** Each room gets a line in `lightUnits[]` at the top of
`Lights.ino` (boxchat id, name, 1–4 zones, relay id, zone names). The list scrolls,
four rooms at a time, with arrows at the edge when there are more. Scenes are lines
in `LIGHTS_SCENES[]` (name plus R, G, B, W). They're numbered in the order they're
listed, and the menu scrolls too. Both lists mirror `python/config.json` and the
console's `Console.h`, so add new rooms and scenes there as well.

| Key | Does |
| --- | --- |
| up / down (`;` `.`) | choose a light |
| left / right (`,` `/`) | lower / raise the adjusted channel by 16 (Fn+left/right: by 4) |
| Space or Enter | on/off (back on at its last colour, or white) |
| `1` | **scene menu** (see below) |
| `2` | zone: ALL → each zone by name (Left, Right…) |
| `3` or Tab | adjusted channel: BRI → R → G → B → W (BRI scales the colour, keeping its hue) |
| `4` | toggle the light's power relay |
| `0` | ALL OFF: every light to `0` and every relay off |
| `h` | key help |
| G0 tap | switch to the remote |
| G0 hold 3 s, Fn+Q or `` ` `` | shell |

**Scene menu** (`1`). It lists every scene numbered, with a colour swatch. Type a
scene's number and press Enter (`12` then Enter is scene 12), or move with
up/down and press Enter. The scene goes on the selected light, or only on the
selected zone if you picked one with `2`. Del rubs out a typed digit, and `` ` ``
closes the menu.

How it talks to boxchat:
- **Commands** use the zone standard: `(4×(zone−1) + channel + 1) × 1000 + level`,
  published to `boxchat/<id>/set`. Only the channels that changed are sent, at most
  once every 50 ms per light.
- **Listening:** it subscribes to `boxchat/+/state` (retained colours, which fill
  in the swatches) and `boxchat/+/status` (the online dot). For 800 ms after a
  change, the light's own echo is ignored so it can't snap the level back.
- **Relays** report nothing, so `PWR ?` means unknown until you switch one here.
- The Cardputer only listens and commands. It publishes no status topic, so it
  doesn't need a boxchat number. Its client ID is `DollRemote-<MAC>`, so it never
  knocks another board off the broker.

## How it talks to Mnemerynne

| Call | When |
| --- | --- |
| `POST /media/remote/pair` `{"speaker":false}` | start pairing → `{code, expires_in, url, secret}` |
| `POST /media/remote/pair/status` `{"secret"}` | every 4 s while pairing → `waiting` / `paired` (+`key`, `name`) / `expired` |
| `GET /api/media/remote` | every 5 s on the list and now-playing pages, and 0.9 s after a command |
| `GET /api/media/remote/channels?offset=&limit=6` | opening and scrolling the channel list |
| `POST /api/media/remote/screens/{id}/command` | every key action (`toggle`, `skip` `by`, `volume` `level`, `mute`, `night`, `next`, `prev`, `stop`, `press` `button`, `play_source` `source_id`, `play_station` `key`, `audio_track`, `subtitles`, `pad` `button`+`down`) |
| `DELETE /api/media/remote` | `forget` |

- **Security.** Every request uses HTTPS, and the certificate is checked against
  ISRG Root X1/X2 (`ca_roots.h`). Certificate checking is never turned off. The
  clock is synced over NTP first so certificate dates can be checked. A server
  whose certificate doesn't chain to Let's Encrypt fails with a TLS error until its
  root is added to `ca_roots.h`.
- **The key.** It is a bearer credential. It is kept only in `/remote.cfg` on the
  internal flash and sent as `Authorization: Bearer …`. Nothing prints or shows it,
  and the fork has no FTP or USB storage that could read it off the device.
- **401** means the owner removed the remote, or left the screens group. The key is
  wiped and the remote goes back to pairing.
- **409** means the TV isn't connected. The server's message ("… isn't connected: is
  Media open on it?") shows in the bottom bar.
- **Volume.** The API takes an absolute level (0–1). The remote adds or subtracts 5%
  from the last level the TV reported, or starts from 50% if the TV hasn't reported one.
- **Polling.** The remote polls only on the list and now-playing pages. The channel
  and gamepad pages don't poll, which keeps them responsive. One TLS connection is
  reused for the whole session.

## Debugging

The remote logs every request to USB serial: the method, the path, the status,
the body for commands, and the server's reply for errors. Pairing requests and
replies are never logged, because they carry the secret and the key. To watch it:

```powershell
.\ps\serial_log.ps1 -Port COM5
```

The script doesn't reset the board when it opens the port. Close it before
flashing, because it holds the port.

## Tested so far

Paired with beepsquad.wtf on the Cardputer ADV. Arrows and Enter drive the TV
once the TV's Media page is current. Not tried yet: gamepad timing over HTTPS,
the channel list, and the lights app against the live broker.
