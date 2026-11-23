"""boxchat <-> GE Cync bridge: makes the Cync Bluetooth light strip behave like boxchat board 32005.

It speaks the same MQTT protocol as the MrBox firmware (see the project README): commands on
boxchat/<board>/set (direct) or boxchat (shared, after arming with the board number), status on
boxchat/<board>/status (retained "online", last-will "offline") and state on boxchat/<board>/state.
Commands are turned into encrypted Telink-mesh writes over Bluetooth (python/cync_mesh.py).

Usage (from the project root, on a PC within Bluetooth range of the strip):
    py python/cync_bridge.py
Needs python/cync_secrets.json (created by `py python/cync_mesh.py --set-mesh ...`) and the MQTT
settings in python/config.json.
"""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path

from cync_mesh import BROADCAST, OP_BRIGHTNESS, OP_COLOR, OP_POWER, open_session

PYTHON_ROOT = Path(__file__).resolve().parent
WHITE_WARMTH = 50  # Cync white mode takes a 0-100 warmth; 50 is a neutral white for the W channel.
RETRY_SECONDS = 3  # Wait before reconnecting after a Bluetooth failure.


def plan_strip(levels: list[int]) -> list[tuple[int, bytes]]:
    """Turn boxchat r, g, b, w levels (0-255) into the Cync commands that show them."""
    r, g, b, w = levels
    if not any(levels):
        return [(OP_POWER, b"\x00")]  # 0 everywhere = off.
    if r or g or b:
        mixed = [min(255, c + w) for c in (r, g, b)]  # No separate white LED in colour mode: mix W in.
        peak = max(mixed)
        rgb = bytes(round(c * 255 / peak) for c in mixed)  # Full-strength hue...
        brightness = max(1, round(peak * 100 / 255))  # ...dimmed by brightness (0-100).
        return [(OP_POWER, b"\x01"), (OP_COLOR, b"\x04" + rgb), (OP_BRIGHTNESS, bytes([brightness]))]
    brightness = max(1, round(w * 100 / 255))
    return [(OP_POWER, b"\x01"), (OP_COLOR, bytes([0x05, WHITE_WARMTH])), (OP_BRIGHTNESS, bytes([brightness]))]
    # Completes the command plan for one colour.


class BoxchatBoard:
    """The boxchat side of one single-zone board: parses commands exactly like the MrBox firmware."""

    def __init__(self, board_id: int, topic: str = "boxchat") -> None:
        self.board_id, self.topic = board_id, topic
        self.direct_topic = f"{topic}/{board_id}/set"
        self.status_topic = f"{topic}/{board_id}/status"
        self.state_topic = f"{topic}/{board_id}/state"
        self.levels = [0, 0, 0, 0]  # r, g, b, w as last requested.
        self.armed = False  # Shared-topic activation lasts for exactly one command.

    def handle(self, topic: str, payload: bytes) -> tuple[bool, list[tuple[str, str]]]:
        """Process one message; returns (levels changed?, status messages to publish)."""
        shared = topic == self.topic
        text = payload.decode("utf-8", errors="replace").rstrip(" \t\r\n")
        if shared and not self.armed:
            if text.isdigit() and int(text) == self.board_id:
                self.armed = True
                return False, [(self.status_topic, "ready")]
            return False, []  # Other boards' traffic: stay quiet.
        if not 1 <= len(payload) <= 31:
            return False, [(self.status_topic, "invalid payload length")]
        if not (text.isascii() and text.isdigit()):
            return False, [(self.status_topic, "invalid numeric command")]
        number = int(text)
        if number > 32000:
            if shared:
                self.armed = number == self.board_id  # Another board's number disarms this one.
                return False, [(self.status_topic, "ready")] if self.armed else []
            return False, [(self.status_topic, "unknown command")]
        if shared:
            self.armed = False  # Exactly one command per activation.
        if number == 0:
            self.levels = [0, 0, 0, 0]
            return True, []
        block, level = divmod(number, 1000)
        if not 1 <= block <= 4 or level > 255:
            return False, [(self.status_topic, "unknown command")]  # Includes zones 2-4: this board has one.
        self.levels[block - 1] = level
        return True, []
        # Completes one command.

    def state_json(self) -> str:
        """State in the firmware's format: top-level zone 1 plus a one-entry zones list."""
        zone = dict(zip("rgbw", self.levels))
        return json.dumps({**zone, "zones": [zone]}, separators=(",", ":"))
        # Completes the retained state payload.


class CyncBridge:
    """Connects the boxchat board to the strip: MQTT in one thread, Bluetooth in the asyncio loop."""

    def __init__(self, board: BoxchatBoard, secrets: dict, mqtt_config: dict) -> None:
        from mqtt_client import MQTTClient

        self.board, self.secrets = board, secrets
        self.loop: asyncio.AbstractEventLoop | None = None
        self.wake = asyncio.Event()  # Set whenever the requested colour changes.
        self.applied: list[int] | None = None  # Last colour the strip confirmed taking; None until the first.
        self.strip_ok = True  # False after a Bluetooth failure, until the next success.
        self.mqtt = MQTTClient(
            host=mqtt_config["host"], port=mqtt_config["port"], client_id=f"CyncBridge-{board.board_id}",
            subscriptions=(board.topic, board.direct_topic), on_message=self._on_mqtt,
            username=mqtt_config.get("username", ""), password=mqtt_config.get("password", ""),
            keepalive=mqtt_config.get("keepalive", 30),
            will_topic=board.status_topic, will_payload="offline", will_retain=True,
            on_connect=self._on_connect,
        )

    def _on_connect(self) -> None:
        self.mqtt.publish(self.board.status_topic, "online" if self.strip_ok else "strip unreachable", retain=True)
        if self.applied is not None:
            self.mqtt.publish(self.board.state_topic, self.board.state_json(), retain=True)
    # Completes the (re)connect announcement.

    def _on_mqtt(self, topic: str, payload: bytes) -> None:
        self.loop.call_soon_threadsafe(self._handle, topic, payload)  # Hands off to the asyncio thread.

    def _handle(self, topic: str, payload: bytes) -> None:
        changed, replies = self.board.handle(topic, payload)
        for reply_topic, text in replies:
            self.mqtt.publish(reply_topic, text)
        if changed:
            print(f"command {payload.decode(errors='replace').strip()} -> levels {self.board.levels}", flush=True)
            self.wake.set()
    # Completes one MQTT message on the asyncio side.

    async def run(self) -> None:
        """Start MQTT, then keep the strip in sync with the latest requested colour forever."""
        self.loop = asyncio.get_running_loop()
        self.mqtt.start()
        client = session = None
        destination = self.secrets.get("destination", BROADCAST)
        print(f"bridge for board {self.board.board_id} running; waiting for commands", flush=True)
        while True:
            await self.wake.wait()
            self.wake.clear()
            target = list(self.board.levels)  # Only the newest colour matters; older ones are skipped.
            try:
                if client is None or not client.is_connected:
                    client, session = await open_session(
                        self.secrets["address"], self.secrets["mesh_name"], self.secrets["mesh_password"])
                    print("strip connected", flush=True)
                for opcode, params in plan_strip(target):
                    await session.send(destination, opcode, params)
                self.applied = target
                self.mqtt.publish(self.board.state_topic, self.board.state_json(), retain=True)
                if not self.strip_ok:
                    self.strip_ok = True
                    self.mqtt.publish(self.board.status_topic, "online", retain=True)
            except Exception as exc:
                print(f"strip write failed ({type(exc).__name__}: {exc}); retrying in {RETRY_SECONDS} s", flush=True)
                if self.strip_ok:
                    self.strip_ok = False
                    self.mqtt.publish(self.board.status_topic, "strip unreachable", retain=True)
                if client is not None:
                    try:
                        await client.disconnect()
                    except Exception:
                        pass
                client = session = None
                await asyncio.sleep(RETRY_SECONDS)
                self.wake.set()  # Tries the newest colour again.
        # Never returns; stopped with Ctrl+C or by ending the process.


def main() -> None:
    """Load settings and run the bridge until interrupted."""
    parser = argparse.ArgumentParser(description="boxchat <-> GE Cync Bluetooth strip bridge.")
    parser.add_argument("--board", type=int, default=32005, help="boxchat board number (default 32005)")
    parser.add_argument("--secrets", type=Path, default=PYTHON_ROOT / "cync_secrets.json")
    parser.add_argument("--config", type=Path, default=PYTHON_ROOT / "config.json", help="Light Console config (MQTT settings)")
    args = parser.parse_args()

    secrets = json.loads(args.secrets.read_text(encoding="utf-8"))
    mqtt_config = json.loads(args.config.read_text(encoding="utf-8"))["mqtt"]
    bridge = CyncBridge(BoxchatBoard(args.board, mqtt_config.get("base_topic", "boxchat")), secrets, mqtt_config)
    try:
        asyncio.run(bridge.run())
    except KeyboardInterrupt:
        bridge.mqtt.publish(bridge.board.status_topic, "offline", retain=True)
        bridge.mqtt.stop()
        print("bridge stopped")
    # Completes the command-line entry point.


if __name__ == "__main__":
    main()
