"""Generate the two importable Erinne API templates for a Light Console address."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from urllib.parse import urlsplit


def build_templates(base_url: str) -> list[dict]:
    """Describe dynamic board IDs, strict numeric colors, and explicitly marked actions."""
    address = urlsplit(base_url)
    if (address.scheme not in ("http", "https") or not address.hostname or
            address.username or address.password or address.query or address.fragment or
            any(c.isspace() for c in base_url)):
        raise ValueError("base URL must be an http(s) server address without credentials, query, or fragment")
    setup = (
        "Light Console controls the configured RGBW and relay boards. Configure distinct random "
        "LIGHT_CONSOLE_READ_TOKEN and LIGHT_CONSOLE_CONTROL_TOKEN environment variables (at least 32 "
        "characters each) on the Light Console process and restart it. Never put credentials in this file. "
        "Set this API's base URL to the address reachable from the Erinne server, not localhost unless "
        "both services run on the same host. Use an HTTPS reverse proxy when sending credentials across "
        "a network; Light Console itself serves HTTP. For a private certificate, compare the SHA-256 "
        "fingerprint with the certificate on the proxy before pinning it in Access > APIs. On that host: "
        "openssl x509 -in server.crt -noout -fingerprint -sha256. Assign your lighting operators' AD group "
        "in Erinne. Responses contain configured light names, IDs, telemetry and timestamps; names may "
        "identify private rooms. Leave Claude access off unless those names may be sent to Anthropic. "
        "Discover IDs with List lights; never infer them from example questions. An RGBW light may list "
        "named zones (for example Left and Right): pass that zone number to change one, or leave zone "
        "empty to change every zone together. Relay lights cannot "
        "change color. A 202 response means queued, not physically confirmed. Check Light state after "
        "changes; reported RGBW telemetry can be retained/stale, while relay power is only the last command."
    )
    light_id = {"name": "light_id", "type": "int", "in": "path", "required": True,
                "min": 32001, "help": "Exact light ID from List lights; match the requested name first"}
    # Optional: a missing zone renders as JSON null, which Light Console treats as every zone together.
    zone = {"name": "zone", "type": "int", "in": "body", "min": 1, "max": 4,
            "help": "Zone number from the light's zones in List lights (e.g. the Left zone); "
                    "leave empty to change every zone together"}
    read_ops = [
        {"name": "List lights", "method": "GET", "path": "/api/lights", "cache_seconds": 0,
         "description": "Discover every configured light's ID, name, RGBW/relay capabilities, named zones (e.g. Left and Right) and current reported state; use next_offset for another page.",
         "params": [{"name": "offset", "type": "int", "in": "query", "min": 0, "default": 0},
                    {"name": "limit", "type": "int", "in": "query", "min": 1, "max": 100, "default": 100}],
         "example_questions": ["Which lights can you control?", "Find the light in my room.",
                               "Which lights support color?", "List the configured lighting boards."]},
        {"name": "Light state", "method": "GET", "path": "/api/lights/{light_id}", "cache_seconds": 0,
         "description": "Read one discovered light, including color capability, MQTT connection, last reported RGBW values for each zone (top-level values are null when zones differ) or last commanded relay power, and telemetry time.",
         "params": [light_id],
         "example_questions": ["What color is light 32001 reporting?", "Is the relay light on?",
                               "When did this light last report its state?", "Check the bedroom light before changing it."]},
    ]
    color_params = [light_id] + [
        {"name": c, "type": "int", "in": "body", "min": 0, "max": 255,
         **({"default": 0} if c == "w" else {"required": True}),
         "help": label + " intensity, 0 off to 255 full"}
        for c, label in (("r", "Red"), ("g", "Green"), ("b", "Blue"), ("w", "Dedicated white"))
    ] + [zone]
    write_ops = [
        {"name": "Set light color", "action": True, "method": "POST",
         "path": "/api/lights/{light_id}/color", "cache_seconds": 0,
         "description": "Set any color-capable light's complete RGBW output, on every zone or on one named zone. Discover its ID and zones first. Red/green/blue are required integers; white defaults to zero. Relay lights are rejected.",
         "body": '{"r":{{r}},"g":{{g}},"b":{{b}},"w":{{w}},"zone":{{zone}}}', "params": color_params,
         "example_questions": ["Make the bedroom light pink.", "Set light 32001 to blue.",
                               "Change the desk strip to warm orange.", "Set the RGBW light to purple with white off.",
                               "Make only the left side of the room red."]},
        {"name": "Set light channel", "action": True, "method": "POST",
         "path": "/api/lights/{light_id}/channel", "cache_seconds": 0,
         "description": "Change one RGBW channel while preserving the others on a color-capable light, on every zone or on one named zone; use only for a specifically requested channel adjustment.",
         "body": '{"channel":"{{channel}}","value":{{value}},"zone":{{zone}}}',
         "params": [light_id, {"name": "channel", "type": "enum", "in": "body", "required": True,
                              "options": ["r", "g", "b", "w"], "help": "Channel to change"},
                    {"name": "value", "type": "int", "in": "body", "required": True,
                     "min": 0, "max": 255, "help": "Intensity from 0 off to 255 full"}, zone],
         "example_questions": ["Set only the white channel to 128.", "Turn off just the red channel.",
                               "Raise the blue channel to 200 without changing the others.", "Set green to full on the desk strip.",
                               "Dim the white channel on the right zone to 40."]},
        {"name": "Set light power", "action": True, "method": "POST",
         "path": "/api/lights/{light_id}/power", "cache_seconds": 0,
         "description": "Turn any configured light on or off, or one zone of an RGBW light. RGBW on explicitly sets full dedicated white (RGB zero); it does not restore a previous color. Relay on/off switches its power; relays have no zones.",
         "body": '{"power":"{{power}}","zone":{{zone}}}',
         "params": [light_id, {"name": "power", "type": "enum", "in": "body", "required": True,
                              "options": ["on", "off"], "help": "Requested power state"}, zone],
         "example_questions": ["Turn off the bedroom light.", "Switch on the relay lights.",
                               "Turn light 32002 off.", "Turn the RGBW strip on with full white.",
                               "Turn off just the right side."]},
    ]
    return [
        {"format": "mnemerynne-api-template", "name": "Light Console" + suffix,
         "base_url": base_url.rstrip("/"), "auth_type": "bearer", "one_at_a_time": True,
         "description": setup + credential, "operations": operations}
        for suffix, credential, operations in (
            (" (read)", " Paste LIGHT_CONSOLE_READ_TOKEN as this entry's Bearer credential. It cannot make changes.", read_ops),
            (" (control)", " Paste LIGHT_CONSOLE_CONTROL_TOKEN as this separate entry's Bearer credential. "
             "Every operation is an action: Erinne proposes it and the person presses Run it. "
             "Enable the read template too so the assistant can discover IDs and capabilities.", write_ops),
        )
    ]


def main() -> None:
    """Write portable templates without reading credentials or contacting any lights."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True, help="Light Console URL reachable from Erinne")
    parser.add_argument("--output", type=Path, default=Path(__file__).parent / "api-templates")
    args = parser.parse_args()
    templates = build_templates(args.base_url)
    args.output.mkdir(parents=True, exist_ok=True)
    for filename, template in zip(("light-console-read.json", "light-console-control.json"), templates):
        target = args.output / filename
        target.write_text(json.dumps(template, indent=2) + "\n", encoding="utf-8")
        print(target)  # Only filenames are logged; credentials never enter the templates.


if __name__ == "__main__":
    main()
