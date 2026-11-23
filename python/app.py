"""Local-network web controller for RGBW and relay light boards."""

from __future__ import annotations

import argparse
import json
import mimetypes
import re
import socket
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from mqtt_client import MQTTClient
from lighting_api import authorize, handle_get, handle_post, load_tokens


APP_ROOT = Path(__file__).resolve().parent
CHANNEL_OFFSETS = {"r": 1000, "g": 2000, "b": 3000, "w": 4000}  # Zone 1 command blocks.
ZONE_SPAN = 4000  # Each later zone's R/G/B/W blocks follow the previous zone's: zone 2 = 5000-8255.
MAX_ZONES = 4  # The boxchat standard defines zones 1-4 (commands up to 16255).
BOARD_KINDS = {"rgbw", "relay"}
COMMAND_MODES = {"direct", "shared"}


class ExclusiveHTTPServer(ThreadingHTTPServer):
    """HTTP server that refuses to share its port with another running server."""

    # On Windows, SO_REUSEADDR lets a second process bind a port that is already listening,
    # so duplicates silently split the traffic. Linux's SO_REUSEADDR only skips TIME_WAIT.
    allow_reuse_address = not hasattr(socket, "SO_EXCLUSIVEADDRUSE")

    def server_bind(self) -> None:
        """Claim the port exclusively on Windows before binding."""
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)  # Blocks any later bind to this port.
        super().server_bind()
    # Completes exclusive port binding so a duplicate launch fails instead of hiding behind the first.


def load_config(path: Path) -> dict[str, Any]:
    """Load and validate the web, MQTT, and board configuration."""
    with path.open("r", encoding="utf-8") as config_file:
        config = json.load(config_file)

    mqtt = config.get("mqtt", {})
    web = config.get("web", {})
    boards = config.get("boards", [])
    if not mqtt.get("host") or not isinstance(mqtt.get("port"), int):
        raise ValueError("mqtt.host and mqtt.port are required")
    if mqtt.get("command_mode", "direct") not in COMMAND_MODES:
        raise ValueError("mqtt.command_mode must be 'direct' or 'shared'")
    if not isinstance(web.get("port"), int):
        raise ValueError("web.port is required")
    if not boards:
        raise ValueError("at least one board must be configured")

    seen_ids: set[int] = set()
    for board in boards:
        board_id = board.get("id")
        if not isinstance(board_id, int) or board_id <= 32_000:
            raise ValueError("every board id must be a unique integer above 32000")
        if board_id in seen_ids:
            raise ValueError(f"duplicate board id: {board_id}")
        seen_ids.add(board_id)
        board.setdefault("name", f"Light {board_id}")
        board.setdefault("kind", "rgbw")
        if board["kind"] not in BOARD_KINDS:
            raise ValueError(f"board {board_id} kind must be 'rgbw' or 'relay'")
        if board.get("command_mode", mqtt.get("command_mode", "direct")) not in COMMAND_MODES:
            raise ValueError(f"board {board_id} command_mode must be 'direct' or 'shared'")
        board["zones"] = validate_zones(board_id, board)

    # Second pass: a board may name a relay as its master power supply, shown pinned to
    # the left of that board's controls in the UI. The relay must already exist and be a
    # relay, so this is checked only after every board's id and kind are known.
    kinds = {board["id"]: board["kind"] for board in boards}
    for board in boards:
        relay_id = board.get("power_relay")
        if relay_id is None:
            continue
        if board["kind"] == "relay":
            raise ValueError(f"board {board['id']} is a relay and cannot have a power_relay")
        if relay_id == board["id"] or kinds.get(relay_id) != "relay":
            raise ValueError(f"board {board['id']} power_relay must be another board's relay id")
    return config


def validate_zones(board_id: int, board: dict[str, Any]) -> list[str]:
    """Return the board's zone names in protocol order (zone 1 first)."""
    if board.get("kind", "rgbw") == "relay":
        if board.get("zones"):
            raise ValueError(f"board {board_id} is a relay; zones apply to rgbw boards only")
        return []  # Relays switch power and have no RGBW zones.
    zones = board.get("zones", ["Main"])  # Older single-zone entries keep working unchanged.
    if (
        not isinstance(zones, list)
        or not 1 <= len(zones) <= MAX_ZONES
        or not all(isinstance(name, str) and name.strip() for name in zones)
    ):
        raise ValueError(f"board {board_id} zones must be a list of 1 to {MAX_ZONES} names")
    names = [name.strip() for name in zones]
    if len({name.lower() for name in names}) != len(names):
        raise ValueError(f"board {board_id} zone names must be unique")
    return names
    # Completes zone-name validation so every later command can trust the zone count.


def zone_command(zone: int, channel: str, value: int) -> int:
    """Encode one channel level for one zone using the boxchat zone standard."""
    return CHANNEL_OFFSETS[channel] + (zone - 1) * ZONE_SPAN + value
    # Completes the mapping, e.g. zone 2 red 128 -> 5128 and zone 1 white 255 -> 4255.


class LightApplication:
    """Coordinate configuration, MQTT messages, and HTTP API state."""

    def __init__(self, config: dict[str, Any]) -> None:
        self.config = config
        self.mqtt_config = config["mqtt"]
        self.web_config = config["web"]
        for board in config["boards"]:
            board["zones"] = validate_zones(board["id"], board)  # Idempotent; also covers configs built in code.
        self.boards = {board["id"]: board for board in config["boards"]}
        self.base_topic = self.mqtt_config.get("base_topic", "boxchat").strip("/")
        self.command_mode = self.mqtt_config.get("command_mode", "direct")
        self.api_tokens = load_tokens()  # Read credentials only from the server environment.
        self._state_lock = threading.Lock()
        self._command_lock = threading.RLock()  # Serialize complete colors as well as shared activator pairs.
        self._board_state: dict[int, dict[str, Any]] = {}
        for board_id, board in self.boards.items():
            state: dict[str, Any] = {
                "status": "unknown",
                "last_seen": None,
                "last_state_at": None,
            }
            if board["kind"] == "relay":
                state["power"] = None  # Records the last command because the relay sends no acknowledgement.
            else:
                state.update({"r": 0, "g": 0, "b": 0, "w": 0})  # Mirrors zone 1 for older status readers.
                state["zones"] = [{"r": 0, "g": 0, "b": 0, "w": 0} for _ in board["zones"]]
            self._board_state[board_id] = state
        client_id = f"LightConsole-{socket.gethostname()}-{int(time.time()) % 100000}"
        subscriptions = (
            f"{self.base_topic}/+/status",
            f"{self.base_topic}/+/state",
        )
        self.mqtt = MQTTClient(
            host=self.mqtt_config["host"],
            port=self.mqtt_config["port"],
            client_id=client_id,
            subscriptions=subscriptions,
            on_message=self._on_mqtt_message,
            username=self.mqtt_config.get("username", ""),
            password=self.mqtt_config.get("password", ""),
            keepalive=self.mqtt_config.get("keepalive", 30),
        )

    def start(self) -> None:
        """Start MQTT background processing."""
        self.mqtt.start()

    def stop(self) -> None:
        """Stop MQTT background processing."""
        self.mqtt.stop()

    def public_config(self) -> dict[str, Any]:
        """Return browser-safe configuration without MQTT credentials."""
        return {
            "boards": list(self.boards.values()),
            "broker": f"{self.mqtt_config['host']}:{self.mqtt_config['port']}",
            "base_topic": self.base_topic,
            "command_mode": self.command_mode,
        }

    def status(self) -> dict[str, Any]:
        """Return current broker and per-board state for browser polling."""
        with self._state_lock:
            boards = {
                str(key): {**value, **({"zones": [dict(z) for z in value["zones"]]} if "zones" in value else {})}
                for key, value in self._board_state.items()
            }  # Copies the nested zone dictionaries too, so callers never see a half-applied update.
        return {
            "mqtt_connected": self.mqtt.connected,
            "mqtt_error": self.mqtt.last_error,
            "boards": boards,
            "server_time": time.time(),
        }

    def zone_targets(self, board_id: int, zone: Any = None) -> list[int]:
        """Return the 1-based zones a command applies to; None means every zone together."""
        count = len(self.boards[board_id]["zones"])
        if zone is None:
            return list(range(1, count + 1))
        if type(zone) is not int or not 1 <= zone <= count:
            raise ValueError(f"zone must be an integer from 1 through {count}, or omitted for every zone")
        return [zone]
        # Completes zone validation before anything is published.

    def set_channel(self, board_id: int, channel: str, value: int, zone: Any = None) -> bool:
        """Validate and send one channel command to one zone or to every zone."""
        if (
            board_id not in self.boards
            or self.boards[board_id]["kind"] != "rgbw"
            or channel not in CHANNEL_OFFSETS
        ):
            raise ValueError("unknown RGBW board or channel")
        if type(value) is not int or not 0 <= value <= 255:
            raise ValueError("channel value must be an integer from 0 through 255")
        targets = self.zone_targets(board_id, zone)
        with self._command_lock:
            return all(self._send_command(board_id, zone_command(z, channel, value)) for z in targets)
        # Completes the channel change; a failed publish stops the remaining zones.

    def set_color(self, board_id: int, values: dict[str, Any], zone: Any = None) -> bool:
        """Send a complete RGBW color as four ordered channel commands per targeted zone."""
        if board_id not in self.boards or self.boards[board_id]["kind"] != "rgbw":
            raise ValueError("unknown RGBW board")
        normalized: dict[str, int] = {}
        for channel in CHANNEL_OFFSETS:
            value = values.get(channel)
            if type(value) is not int or not 0 <= value <= 255:
                raise ValueError("r, g, b, and w must be integers from 0 through 255")
            normalized[channel] = value
        targets = self.zone_targets(board_id, zone)

        with self._command_lock:
            return all(
                self.set_channel(board_id, channel, value, z)
                for z in targets
                for channel, value in normalized.items()
            )
        # Completes the color change zone by zone, keeping each zone's four commands together.

    def set_power(self, board_id: int, power: bool) -> bool:
        """Turn one relay-controlled light on or off."""
        if board_id not in self.boards or self.boards[board_id]["kind"] != "relay":
            raise ValueError("unknown relay board")
        if not isinstance(power, bool):
            raise ValueError("power must be true or false")

        with self._command_lock:
            accepted = self._send_command(board_id, 1 if power else 0)
            if accepted:
                with self._state_lock:
                    self._board_state[board_id]["power"] = power
            return accepted

    def all_off(self) -> bool:
        """Send the firmware's all-off command to every configured board."""
        results = []
        for board_id, board in self.boards.items():
            if board["kind"] == "relay":
                results.append(self.set_power(board_id, False))
            else:
                results.append(self._send_command(board_id, 0))
        return all(results)

    def _send_command(self, board_id: int, command: int) -> bool:
        """Publish through the board's configured direct or shared protocol."""
        with self._command_lock:
            command_mode = self.boards[board_id].get("command_mode", self.command_mode)
            if command_mode == "direct":
                return self.mqtt.publish(f"{self.base_topic}/{board_id}/set", str(command))
            if not self.mqtt.publish(self.base_topic, str(board_id)):
                return False  # A shared command must never follow a failed activator.
            return self.mqtt.publish(self.base_topic, str(command))

    def _on_mqtt_message(self, topic: str, payload: bytes) -> None:
        """Apply retained status and RGBW acknowledgements from firmware."""
        match = re.fullmatch(
            rf"{re.escape(self.base_topic)}/(\d+)/(status|state)",
            topic,
        )
        if not match:
            return
        board_id = int(match.group(1))
        message_type = match.group(2)
        if board_id not in self.boards:
            return

        with self._state_lock:
            state = self._board_state[board_id]
            state["last_seen"] = time.time()
            if message_type == "status":
                state["status"] = payload.decode("utf-8", errors="replace")
                return

            try:
                incoming = json.loads(payload.decode("utf-8"))
                if not isinstance(incoming, dict):
                    raise ValueError("state must be an object")
                if self.boards[board_id]["kind"] != "rgbw":
                    return
                # Multi-zone firmware lists every zone; single-zone firmware reports one flat object.
                reported = incoming["zones"] if "zones" in incoming else [incoming]
                if not isinstance(reported, list) or not reported or not all(
                    isinstance(zone, dict)
                    and all(type(zone.get(c)) is int and 0 <= zone[c] <= 255 for c in CHANNEL_OFFSETS)
                    for zone in reported
                ):
                    raise ValueError("state must contain four RGBW values for each zone")
                state["last_state_at"] = time.time()
                for zone_state, zone in zip(state["zones"], reported):  # Extra reported zones are ignored.
                    zone_state.update({c: zone[c] for c in CHANNEL_OFFSETS})
                state.update(state["zones"][0])  # Keeps the legacy top-level values equal to zone 1.
            except (UnicodeDecodeError, ValueError):
                state["status"] = "invalid state payload"


def make_handler(application: LightApplication) -> type[BaseHTTPRequestHandler]:
    """Create an HTTP request handler bound to one application instance."""

    class LightRequestHandler(BaseHTTPRequestHandler):
        server_version = "LightConsole/1.0"

        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API name.
            """Serve the app shell, static assets, and read-only JSON APIs."""
            if urlsplit(self.path).path.startswith("/api/") and not authorize(self, application, write=False):
                return
            if handle_get(self, application):
                return
            if self.path == "/":
                self._send_file(APP_ROOT / "templates" / "index.html")
            elif self.path.startswith("/static/"):
                relative = self.path.removeprefix("/static/")
                asset = (APP_ROOT / "static" / relative).resolve()
                static_root = (APP_ROOT / "static").resolve()
                if static_root not in asset.parents:
                    self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)
                else:
                    self._send_file(asset)
            elif self.path == "/api/config":
                self._send_json(application.public_config())
            elif self.path == "/api/status":
                self._send_json(application.status())
            else:
                self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)

        def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API name.
            """Validate controls and accept work only with write access."""
            if not authorize(self, application, write=True):
                return
            try:
                payload = self._read_json()
                channel_match = re.fullmatch(r"/api/boards/(\d+)/channel", self.path)
                color_match = re.fullmatch(r"/api/boards/(\d+)/color", self.path)
                power_match = re.fullmatch(r"/api/boards/(\d+)/power", self.path)
                external = handle_post(self.path, payload, application)
                if external is not None:
                    accepted = external
                elif channel_match:
                    accepted = application.set_channel(
                        int(channel_match.group(1)),
                        payload.get("channel", ""),
                        payload.get("value"),
                        payload.get("zone"),  # Omitted means every zone together.
                    )
                elif color_match:
                    accepted = application.set_color(int(color_match.group(1)), payload, payload.get("zone"))
                elif power_match:
                    accepted = application.set_power(
                        int(power_match.group(1)),
                        payload.get("power"),
                    )
                elif self.path == "/api/all/off":
                    accepted = application.all_off()
                else:
                    self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)
                    return

                if not accepted:
                    self._send_json(
                        {"error": "MQTT unavailable or queue full; command may be partially accepted", "accepted": False},
                        HTTPStatus.SERVICE_UNAVAILABLE,
                    )
                    return
                self._send_json({"accepted": True, "delivery": "queued", "confirmed": False}, HTTPStatus.ACCEPTED)
            except KeyError:
                self._send_json({"error": "unknown light"}, HTTPStatus.NOT_FOUND)
            except (ValueError, TypeError) as exc:
                self._send_json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)

        def _read_json(self) -> dict[str, Any]:
            """Read a small JSON request body while rejecting oversized input."""
            if self.headers.get("Transfer-Encoding"):
                raise ValueError("Transfer-Encoding is not supported")
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError as exc:
                raise ValueError("invalid Content-Length") from exc
            if length < 0 or length > 4096:
                raise ValueError("request body must be 4096 bytes or less")
            body = self.rfile.read(length)
            parsed = json.loads(body or b"{}")
            if not isinstance(parsed, dict):
                raise ValueError("request JSON must be an object")
            return parsed

        def _send_json(self, payload: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
            """Send one UTF-8 JSON response with no browser caching."""
            body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            if status == HTTPStatus.UNAUTHORIZED:
                self.send_header("WWW-Authenticate", 'Bearer realm="Light Console"')
            self.end_headers()
            self.wfile.write(body)

        def _send_file(self, path: Path) -> None:
            """Serve one known app asset with its inferred MIME type."""
            if not path.is_file():
                self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)
                return
            body = path.read_bytes()
            content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", f"{content_type}; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, message_format: str, *args: Any) -> None:
            """Keep concise request logs with the client address."""
            print(f"HTTP {self.client_address[0]} - {message_format % args}")

    return LightRequestHandler


def parse_args() -> argparse.Namespace:
    """Parse the optional configuration-file override."""
    parser = argparse.ArgumentParser(description="Run the local QuinLED light console")
    parser.add_argument(
        "--config",
        type=Path,
        default=APP_ROOT / "config.json",
        help="path to the JSON configuration file",
    )
    return parser.parse_args()


def main() -> None:
    """Start MQTT and serve the control panel until interrupted."""
    args = parse_args()
    application = LightApplication(load_config(args.config.resolve()))
    host = application.web_config.get("host", "0.0.0.0")
    port = application.web_config["port"]
    try:
        server = ExclusiveHTTPServer((host, port), make_handler(application))  # Binds before MQTT so a duplicate never connects.
    except OSError as error:
        raise SystemExit(
            f"Port {port} is already in use ({error}). Another Light Console may still be running; "
            "stop it before starting a new one."
        ) from error
    application.start()
    print(f"Light Console listening on http://{host}:{port}")
    print(f"MQTT broker: {application.mqtt_config['host']}:{application.mqtt_config['port']}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Stopping Light Console")
    finally:
        server.server_close()
        application.stop()


if __name__ == "__main__":
    main()
