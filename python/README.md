# Light Console web app

Light Console is a zero-dependency Python 3.11 web controller for the project's QuinLED Dig2Analog and ESP32 relay firmware. It runs on a computer inside the local network and serves a responsive browser interface for RGBW sliders, color presets, relay on/off buttons, multiple boards, and all-off control.

The interface uses an original sakura-CRT theme inspired by the requested SadGirlsClub aesthetic: monochrome terminal panels, hot-pink highlights, scanlines, and a locally bundled blossom illustration. It does not download fonts, images, or scripts from the internet, so the complete interface remains available when the LAN has no internet connection.

## Configure

Edit `config.json`:

- `mqtt.host`: local MQTT broker address
- `mqtt.port`: broker port, normally `1883`
- `mqtt.username` and `mqtt.password`: optional broker credentials
- `mqtt.base_topic`: firmware command root, currently `boxchat`
- `mqtt.command_mode`: `direct` for `boxchat/<board-id>/set`, or `shared` for the two-message activation protocol
- `web.host`: keep `0.0.0.0` to allow other LAN devices to open the app
- `web.port`: HTTP port, currently `8080`
- `boards`: one entry for every unique firmware activator value
- `boards[].kind`: `rgbw` for a Dig2Analog controller or `relay` for a simple on/off receiver
- `boards[].command_mode`: optional per-board override; the relay receiver uses `shared`
- `boards[].zones`: optional for `rgbw` boards, a list of 1–4 unique zone names in protocol order (zone 1 first). Leave it out for a single-zone board. Relays cannot have zones.

Example two-zone board, the Room Lights unit (left on Dig-Quad LED1, right on LED2):

```json
{
  "id": 32004,
  "name": "Room Lights",
  "kind": "rgbw",
  "zones": ["Left", "Right"]
}
```

A board with more than one zone gets **BOTH / LEFT / RIGHT** tabs (**ALL ZONES** when there are more than two) and a swatch showing each zone's current color. The selected tab decides what the sliders, color picker and presets change:

- **BOTH**: a slider sets that channel on every zone; a preset or picked color makes every zone the same color. If the zones differ, the sliders show zone 1, the readout says `MIXED`, and the glow shows the zone colors side by side.
- **LEFT** / **RIGHT**: only that zone changes. Clicking a zone's swatch selects it too.

The app turns zone *z*, channel *c* (R, G, B, W = 0–3) into command `(4 × (z − 1) + c + 1) × 1000 + level`, e.g. right red 128 → `5128`. That's the boxchat zone standard described in the project README.

Example relay board:

```json
{
  "id": 32002,
  "name": "Relay Lights",
  "kind": "relay",
  "command_mode": "shared"
}
```

`direct` is recommended for the RGBW firmware because it subscribes to `boxchat/<cmdActVal>/set`. The relay firmware listens only on `boxchat`, so its board entry overrides the global setting with `shared`. The app keeps each shared activator and command pair together.

## Run on Windows

From the project directory:

```powershell
py python/app.py
```

Or run:

```powershell
powershell -ExecutionPolicy Bypass -File ps/start_light_console.ps1
```

Open `http://localhost:8080` on the host computer. From another device on the same LAN, open `http://HOST-COMPUTER-IP:8080`. Windows Firewall may ask for permission the first time; allow access only on private networks.

The original LAN mode has no authentication until API credentials are configured. The Erinne integration adds separate read/control Bearer credentials and protects the browser routes too. TLS requires a reverse proxy; do not expose the built-in HTTP server directly to the public internet. See [Erinne API setup](ERINNE_API.md).

## Start automatically at Windows boot

From an administrator PowerShell in the project directory, install the startup task with the absolute path to a machine-accessible Python 3.11+ interpreter:

```powershell
powershell -ExecutionPolicy Bypass -File ps/install_light_console_startup.ps1 -PythonExecutable "C:\Program Files\Python311\python.exe"
```

The **Light Console Server** task starts immediately and at each boot after a 30-second delay, including before sign-in. It runs hidden as the limited Windows LOCAL SERVICE account, retries failures every minute (up to 999 times), permits battery operation, and has no runtime limit. Keep this project and interpreter at their installed paths on a drive available at boot. Stop any manually started server before installation.

Output is written to `logs/stdout.log` and `logs/stderr.log`; each restart preserves one previous copy. Installation diagnostics are in `logs/startup-install.log`. Logs are excluded from Git. LOCAL SERVICE must have read access to the project and interpreter; the installer grants it write access to the log folder. The task uses machine-level environment variables, not your user's API credentials; configure both `LIGHT_CONSOLE_READ_TOKEN` and `LIGHT_CONSOLE_CONTROL_TOKEN` at machine scope if authentication is needed, then reboot to refresh the task environment.

To apply `config.json` changes, restart it from an administrator PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File ps/restart_light_console.ps1
```

The restart script stops the task, removes any leftover servers from this project, starts the task again, and confirms exactly one server is listening (it prints the board names that server is sending). The web page loads the board list once, so press Ctrl+F5 afterwards.

The launcher places Python in a Windows kill-on-close job, so stopping the task also stops the server. The app also claims its port exclusively: if another server already holds port 8080, the new one exits with a "Port 8080 is already in use" error in `logs/stderr.log` instead of quietly sharing the port and serving stale settings.

Other task commands, also from an administrator PowerShell:

```powershell
Get-ScheduledTask -TaskName 'Light Console Server'
Stop-ScheduledTask -TaskName 'Light Console Server'
Start-ScheduledTask -TaskName 'Light Console Server'
# To remove automatic startup, stop the task first, then unregister it:
Unregister-ScheduledTask -TaskName 'Light Console Server' -Confirm:$false
```

## MQTT behavior

The app subscribes to:

- `boxchat/+/status` for online/offline state
- `boxchat/+/state` for retained RGBW acknowledgements

State acknowledgements may list every zone under `zones`; a flat `{"r","g","b","w"}` from single-zone firmware updates zone 1. In direct mode it publishes numeric firmware commands to `boxchat/<board-id>/set`. A change to every zone is published as one command per zone, in zone order. In shared mode it publishes the board activator followed by one command to `boxchat`; relay `32002` receives `1` for on and `0` for off. Slider values are debounced in the browser, and commands are rejected rather than stored whenever the broker is disconnected, preventing stale lighting changes after reconnection.

The simple relay firmware does not publish state acknowledgements, so its panel card reports the last command sent rather than claiming to know the physical relay state.

## Test

```powershell
py -m unittest discover -s python/tests -v
```

The tests cover exclusive port binding, MQTT packet encoding, board and zone validation, RGBW routing, zone command numbers, per-zone state parsing, relay on/off routing, direct and shared protocols, and state acknowledgement parsing.

## Erinne integration

[ERINNE_API.md](ERINNE_API.md) documents authenticated discovery and control for every configured light, credential setup, HTTP responses, and importing the two JSON templates under `api-templates/`. RGBW lights support full color and individual channels; relay lights support on/off. Generate templates for your own address with `py python/export_erinne_templates.py --base-url https://YOUR-LIGHT-HOST`.
