# Erinne lighting API

This integration follows `C:\scripts\erinne\api-templates\api-package-guide.md` and the actual `mnemerynne.apis` template validator/request builder. No changes or imports into Erinne's running database are required to build it.

## What is included

- `lighting_api.py`: paginated light discovery, individual state, color, channel, and power actions.
- `api-templates/light-console-read.json`: read-only operations for discovery and state.
- `api-templates/light-console-control.json`: three explicit change actions.
- `export_erinne_templates.py`: regenerate both templates with your server address.

All light IDs and zone names come from `config.json`; no endpoint is limited to board 32001. An assistant asked to change "the left side" should find the zone named Left in **List lights** and pass its number as `zone`. Add future boards there and restart the app. Erinne should use **List lights** to match a requested name to its ID and capabilities before proposing a change. Duplicate names require clarification; the API never guesses a target. Relays support on/off only, not color.

## Enable access

From the project root, generate two independent credentials in the PowerShell window that will launch Light Console. Each command is a single line:

```powershell
$env:LIGHT_CONSOLE_READ_TOKEN = (py -c "import secrets; print(secrets.token_urlsafe(32))").Trim()
```

```powershell
$env:LIGHT_CONSOLE_CONTROL_TOKEN = (py -c "import secrets; print(secrets.token_urlsafe(32))").Trim()
```

These variables last for that PowerShell session and its child processes. For a persistent service, configure the same two variables in the service's environment and keep them in your credential store. Do not regenerate them on every restart: existing Erinne credentials would stop working. Never commit them or embed them in template JSON.

Start the server from that same window (stop an older Light Console process first if it already occupies port 8080):

```powershell
py python/app.py
```

Both credentials must be different, at least 32 ASCII characters, and contain no whitespace. Configuring only one fails startup. With both set, **all** existing `/api/config`, `/api/status`, `/api/boards/...`, `/api/all/off`, and new `/api/lights...` routes require a Bearer credential. Read tokens cannot POST, including to the legacy browser routes. The browser prompts for an access token and holds it in memory only; a control token enables changes. Reload to switch credentials. Public HTML/CSS/JS contain no credentials.

With neither variable set, the original trusted-LAN browser mode remains available, but `/api/lights...` returns 503 instead of exposing the external integration anonymously. This compatibility mode is not an authenticated deployment.

## Import into Erinne

1. Choose the Light Console address reachable **from the Erinne server**. The checked-in templates use the intentionally non-routable placeholder `https://lights.example.invalid`. Light Console itself serves HTTP on port 8080; use an HTTPS reverse proxy for credential protection across a network. Set up that proxy separately before using an HTTPS URL. For isolated trusted-network testing, an explicit `http://HOST-IP:8080` URL also works. `localhost` works only when both apps run on the same host.
2. Generate the files with your actual address (replace the example URL):

```powershell
py python/export_erinne_templates.py --base-url https://lights.your-domain.example
```

3. In Erinne, open **Access > APIs > Upload template**, upload both generated files, and **Add** each template as an API.
4. Set the read entry's Bearer credential to `LIGHT_CONSOLE_READ_TOKEN`; set the control entry's to `LIGHT_CONSOLE_CONTROL_TOKEN`. Copy their values privately from the launching shell or your credential store. Never paste credentials into chat. Configure the allowed AD group for each API.
5. If using a private TLS certificate, compare the server certificate's SHA-256 fingerprint with the reverse proxy's actual certificate before pinning it in Erinne. On the certificate host: `openssl x509 -in server.crt -noout -fingerprint -sha256`.
6. Test **List lights**, then **Light state**. Propose a color for an RGBW light and press **Run it** when you intend to change the real light. Erinne treats every write as `action: true`; its existing proposal workflow remains in force.
7. Only enable Claude access if configured light/room names may be sent to Anthropic. Responses contain names, IDs, timestamps, connectivity, and lighting state, but no API tokens or MQTT credentials.

**Upgrading from the single-zone templates:** the three change operations gained an optional `zone` parameter and their bodies now include `"zone":{{zone}}`. Re-upload both generated files and recreate the three control operations so Erinne can target one zone. Old operations keep working; they just always change every zone.

Re-uploading replaces a saved template, not already-created API operations. To update existing operations, edit/recreate them in Erinne as its guide describes. Changing the base URL in the generator likewise does not update an already-added API automatically.

## HTTP contract

Supply `Authorization: Bearer <credential>`. POST requests use `Content-Type: application/json`. JSON bodies are capped at 4096 bytes; chunked request bodies are unsupported.

| Method | Path | Input | Permission |
| --- | --- | --- | --- |
| GET | `/api/lights?offset=0&limit=100` | Offset >= 0; limit 1-100 | Read or control |
| GET | `/api/lights/{light_id}` | Configured numeric ID | Read or control |
| POST | `/api/lights/{light_id}/color` | `{"r":255,"g":0,"b":128,"w":0,"zone":2}` | Control, RGBW only |
| POST | `/api/lights/{light_id}/channel` | `{"channel":"w","value":128,"zone":1}` | Control, RGBW only |
| POST | `/api/lights/{light_id}/power` | `{"power":"on","zone":2}` or `{"power":"off"}` | Control, any light |

Every write accepts an optional `zone`: an integer from 1 to the light's `zone_count`. Leaving it out or sending `null` changes **every zone together**; the generated templates send `null` when the assistant doesn't fill it in. Relays have no zones, so a non-null `zone` on a relay is rejected. An out-of-range or non-integer zone is rejected before anything is published. Color requires integer `r`, `g`, and `b` from 0 to 255; `w` defaults to 0. Booleans, floats, numeric strings, missing required fields, and unknown fields are rejected before publication. Channel changes preserve the other channels on the board. RGBW **power on** explicitly sets `(0,0,0,255)`; it does not restore the previous color. RGBW **off** sets all channels to zero. Relay commands retain the configured direct/shared routing.

List responses contain `lights`, `total`, `offset`, and `next_offset` (null on the final page). Individual light rows contain: `id`, `name`, `kind`, `supports_color`, `supports_power`, `status`, `mqtt_connected`, `state_source`, `last_seen`, `last_state_at`, `power`, `r`, `g`, `b`, `w`, `zone_count` and `zones`. `zones` lists `{"zone":1,"name":"Left","r":…,"g":…,"b":…,"w":…}` for each RGBW zone in order (an empty list for relays). The top-level `r`/`g`/`b`/`w` describe the whole light only when every zone shows the same color; when zones differ they are null, so read `zones`. `power` is true when any channel of any zone is lit. Timestamps are Unix seconds. RGBW color and power are null until a valid full state message arrives. Relay color fields are always null, and its power is only the last accepted command. `last_state_at` is when the server received telemetry, not when hardware originally produced a retained message.

Successful writes return HTTP 202:

```json
{"accepted":true,"delivery":"queued","confirmed":false}
```

That means MQTT work was accepted locally, **not** that the device applied it. The transport uses QoS 0. Read state afterward and inspect connectivity/timestamps, but do not treat old retained telemetry as confirmation of a particular request. Four-channel colors (four commands per targeted zone, zone by zone) are serialized against other requests in this app, but the firmware receives individual commands; physical changes are not atomic. Other independent MQTT publishers are outside this process's lock.

Errors are JSON with an `error` field: 400 invalid input/unsupported capability, 401 missing or invalid credential, 403 read credential used for a write, 404 unknown light/route, 503 API credentials unconfigured or MQTT unavailable/queue full. A failed multi-command write may have partially queued/applied channels. Pending MQTT work is dropped on disconnect so it is not replayed after reconnection. Do not automatically retry partial failures without reading state and deciding on the intended full color.

## Verification

Local tests use loopback HTTP with a fake broker and never operate hardware:

```powershell
py -m unittest discover -s python/tests -v
```

To also run the contract test with Erinne's actual installed validator, use its Python environment:

```powershell
$env:ERINNE_ROOT='C:\scripts\erinne'; & 'C:\scripts\erinne\.venv\Scripts\python.exe' -m unittest discover -s python/tests -v
```

For a manual smoke test, start Light Console with credentials, open its browser panel, verify the password dialog and read-only denial, then import both templates. Check discovery against `config.json`. Change one RGBW light and inspect its state acknowledgement; try a relay color and expect rejection. Verify a different configured RGBW board receives only its own messages. Keep physical testing deliberate: successful tests here do not prove LAN reachability, TLS configuration, or hardware wiring.
