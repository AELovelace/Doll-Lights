#!/usr/bin/env python3
"""boxchat <-> OpenBeken bridge for the iHome IH-WW117 smart plug (board 32007).

The plug was flashed from Tuya's stock firmware to OpenBeken (open source), so it no
longer talks to any cloud. OpenBeken exposes a tiny HTTP command API on the LAN; this
bridge makes that plug look like a normal boxchat relay to Light Console:

  * It subscribes to the boxchat MQTT protocol and turns commands into HTTP calls that
    switch OpenBeken's relay (GPIO P24, channel 1).
  * It announces boxchat/<id>/status online (retained) and an offline last will, so the
    console shows the plug as connected.

Relay command set (identical to the ESP32 relay boards):
  * direct : boxchat/<id>/set  payload "1" = on, "0" = off
  * shared : publish <id> on `boxchat` to arm, then "1"/"0" as the next payload
A relay sends no RGBW state, so the console tracks its on/off locally; we only publish
status. Run this on the always-on NAS (see plug_bridge.service)."""

from __future__ import annotations

import sys
import time
import urllib.request

from mqtt_client import MQTTClient

# --- Configuration (edit here; kept inline so the bridge stays zero-dependency) --------
BROKER_HOST = "192.168.1.253"      # boxchat MQTT broker.
BROKER_PORT = 1883
BOARD_ID = 32007                   # Registry number claimed for this plug.
PLUG_HOST = "192.168.1.94"         # The plug's OpenBeken HTTP server on the LAN.
BASE_TOPIC = "boxchat"             # Shared/arming topic root.
HTTP_TIMEOUT = 4                   # Seconds to wait on the plug's web server.


def set_plug(power: bool) -> bool:
    """Switch the OpenBeken relay on or off over its HTTP command API."""
    command = "POWER%20ON" if power else "POWER%20OFF"  # OpenBeken Tasmota-style endpoint.
    url = f"http://{PLUG_HOST}/cm?cmnd={command}"
    try:
        with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT) as response:
            response.read()  # Drain the body so the connection closes cleanly.
        print(f"plug -> {'ON' if power else 'OFF'}", flush=True)
        return True
    except Exception as error:  # Network blips should never crash the bridge thread.
        print(f"plug HTTP error ({'ON' if power else 'OFF'}): {error}", flush=True)
        return False
# Completes one relay switch, reporting success so status can reflect reachability.


class PlugBridge:
    """Translate boxchat MQTT commands into OpenBeken relay switches."""

    def __init__(self) -> None:
        self.status_topic = f"{BASE_TOPIC}/{BOARD_ID}/status"
        self.set_topic = f"{BASE_TOPIC}/{BOARD_ID}/set"
        self._armed = False  # True once the shared topic has named our board id.
        self.client = MQTTClient(
            host=BROKER_HOST,
            port=BROKER_PORT,
            client_id=f"plug-bridge-{BOARD_ID}",
            subscriptions=(BASE_TOPIC, self.set_topic),  # Shared protocol plus direct set.
            on_message=self._on_message,
            keepalive=30,
            will_topic=self.status_topic,
            will_payload="offline",      # Broker publishes this if the bridge vanishes.
            will_retain=True,
            on_connect=self._on_connect,
        )

    def _on_connect(self) -> None:
        """Announce the plug as online after every (re)connect."""
        self.client.publish(self.status_topic, "online", retain=True)
        print("connected to broker; announced online", flush=True)
    # Completes reconnect handling so the console always sees a fresh status.

    def _apply(self, command: int) -> None:
        """Act on a decoded boxchat command (1 = on, 0 = off)."""
        if command in (0, 1):
            set_plug(command == 1)  # Any other value is ignored, matching the firmware.
    # Completes command dispatch for the single relay channel.

    def _on_message(self, topic: str, payload: bytes) -> None:
        """Handle direct-set and shared (arm-then-command) boxchat traffic."""
        text = payload.decode("utf-8", errors="replace").strip()
        if not text.lstrip("-").isdigit():
            return  # boxchat payloads are always plain integers.
        value = int(text)

        if topic == self.set_topic:
            self._apply(value)  # Direct mode: the value is the command itself.
            return

        if topic == BASE_TOPIC:  # Shared mode: activators arm, the next value commands.
            if value > 32000:
                self._armed = value == BOARD_ID  # Arm only when this board is addressed.
                return
            if self._armed:
                self._apply(value)
                self._armed = False  # One shared activation covers exactly one command.
    # Completes boxchat decoding for both command modes.

    def run(self) -> None:
        """Start the MQTT client and keep the process alive."""
        self.client.start()
        print(f"plug bridge for board {BOARD_ID} started", flush=True)
        try:
            while True:
                time.sleep(60)  # The MQTT client runs on its own reconnecting thread.
        except KeyboardInterrupt:
            pass
        finally:
            self.client.publish(self.status_topic, "offline", retain=True)
            self.client.stop()
    # Completes the service lifecycle, leaving a clean offline status on shutdown.


if __name__ == "__main__":
    sys.exit(PlugBridge().run())
