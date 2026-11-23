"""Erinne-facing discovery and controls; all device commands use the existing app."""

from __future__ import annotations

import os
import re
import secrets
from http import HTTPStatus
from urllib.parse import parse_qs, urlsplit


CHANNELS = ("r", "g", "b", "w")


def load_tokens() -> tuple[str, str]:
    """Require a distinct read and control credential when API access is enabled."""
    tokens = (os.environ.get("LIGHT_CONSOLE_READ_TOKEN", ""),
              os.environ.get("LIGHT_CONSOLE_CONTROL_TOKEN", ""))
    if not any(tokens):
        return tokens  # Existing installations can still use the original LAN-only UI.
    if (not all(tokens) or tokens[0] == tokens[1] or
            any(len(t) < 32 or not t.isascii() or any(c.isspace() for c in t) for t in tokens)):
        raise ValueError("set distinct LIGHT_CONSOLE_READ_TOKEN and LIGHT_CONSOLE_CONTROL_TOKEN "
                         "values of at least 32 ASCII characters without whitespace")
    return tokens


def authorize(handler, application, *, write: bool) -> bool:
    """Apply credential scopes to legacy controls too, preventing a write bypass."""
    read_token, control_token = application.api_tokens
    if not control_token:
        if urlsplit(handler.path).path.startswith("/api/lights"):
            handler._send_json({"error": "configure LIGHT_CONSOLE_READ_TOKEN and LIGHT_CONSOLE_CONTROL_TOKEN"},
                               HTTPStatus.SERVICE_UNAVAILABLE)
            return False
        return True
    scheme, _, value = handler.headers.get("Authorization", "").partition(" ")
    supplied = value.encode("utf-8") if scheme.lower() == "bearer" else b""
    control = secrets.compare_digest(supplied, control_token.encode("ascii"))
    read = secrets.compare_digest(supplied, read_token.encode("ascii"))
    if not (read or control):
        handler._send_json({"error": "a valid Bearer token is required"}, HTTPStatus.UNAUTHORIZED)
        return False
    if write and not control:
        handler._send_json({"error": "this credential is read-only"}, HTTPStatus.FORBIDDEN)
        return False
    return True


def describe_light(application, board_id: int, snapshot: dict | None = None) -> dict:
    """Return flat, capability-aware state without exposing credentials or initial zeros."""
    board = application.boards[board_id]  # Unknown IDs become HTTP 404 at the request boundary.
    snapshot = snapshot if snapshot is not None else application.status()
    state = snapshot["boards"][str(board_id)]
    rgbw = board["kind"] == "rgbw"
    known = state["last_state_at"] is not None
    zone_states = state.get("zones", [])
    # Top-level r/g/b/w describe the whole light only when every zone shows the same color.
    uniform = rgbw and known and all(z == zone_states[0] for z in zone_states)
    return {
        "id": board_id, "name": board["name"], "kind": board["kind"],
        "supports_color": rgbw, "supports_power": True,
        "status": state["status"], "mqtt_connected": snapshot["mqtt_connected"],
        "state_source": "reported" if rgbw and known else (
            "last_command" if not rgbw and state["power"] is not None else "unknown"),
        "last_seen": state["last_seen"], "last_state_at": state["last_state_at"],
        "power": (any(z[c] for z in zone_states for c in CHANNELS) if known else None) if rgbw else state["power"],
        **{c: zone_states[0][c] if uniform else None for c in CHANNELS},
        "zone_count": len(board["zones"]),
        "zones": [{"zone": number, "name": name, **{c: zone[c] if known else None for c in CHANNELS}}
                  for number, (name, zone) in enumerate(zip(board["zones"], zone_states), start=1)],
    }
    # Completes one flat light row plus its per-zone detail (an empty list for relays).


def handle_get(handler, application) -> bool:
    """Serve paginated discovery and one-light reads; leave other routes to the UI."""
    parsed = urlsplit(handler.path)
    match = re.fullmatch(r"/api/lights/(\d+)", parsed.path)
    if parsed.path != "/api/lights" and not match:
        return False
    try:
        if match:
            handler._send_json(describe_light(application, int(match.group(1))))
            return True
        query = parse_qs(parsed.query, keep_blank_values=True)
        if set(query) - {"offset", "limit"} or any(len(v) != 1 for v in query.values()):
            raise ValueError("only one offset and limit may be supplied")
        offset = int(query.get("offset", ["0"])[0])
        limit = int(query.get("limit", ["100"])[0])
        if offset < 0 or not 1 <= limit <= 100:
            raise ValueError("offset must be nonnegative; limit must be 1 through 100")
        ids = sorted(application.boards)
        snapshot = application.status()  # One snapshot keeps all rows in a response consistent.
        handler._send_json({
            "lights": [describe_light(application, i, snapshot) for i in ids[offset:offset + limit]],
            "total": len(ids), "offset": offset,
            "next_offset": offset + limit if offset + limit < len(ids) else None,
        })
    except KeyError:
        handler._send_json({"error": "unknown light"}, HTTPStatus.NOT_FOUND)
    except ValueError as exc:
        handler._send_json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
    return True


def handle_post(path: str, payload: dict, application) -> bool | None:
    """Validate one typed lighting action before sending any MQTT command."""
    match = re.fullmatch(r"/api/lights/(\d+)/(color|channel|power)", path)
    if not match:
        return None
    board_id, operation = int(match.group(1)), match.group(2)
    board = application.boards[board_id]
    fields = {"color": {"r", "g", "b", "w", "zone"}, "channel": {"channel", "value", "zone"},
              "power": {"power", "zone"}}[operation]
    if set(payload) - fields:
        raise ValueError("unexpected request fields")
    zone = payload.get("zone")  # Null or omitted means every zone together; templates send null.
    if operation == "color":
        color = {c: v for c, v in payload.items() if c != "zone"}
        return application.set_color(board_id, {"w": 0, **color}, zone)  # White defaults off for RGB colors.
    if operation == "channel":
        if not isinstance(payload.get("channel"), str):
            raise ValueError("channel must be r, g, b, or w")
        return application.set_channel(board_id, payload["channel"], payload.get("value"), zone)
    power = payload.get("power")
    if power not in ("on", "off"):
        raise ValueError("power must be 'on' or 'off'")
    if board["kind"] == "relay":
        if zone is not None:
            raise ValueError("relay lights have no zones")
        return application.set_power(board_id, power == "on")
    # Explicit full white for RGBW on avoids deriving a command from stale telemetry.
    return application.set_color(board_id, {"r": 0, "g": 0, "b": 0, "w": 255 if power == "on" else 0}, zone)
    # Completes the typed action after validating zone, channel and power before any publish.
