"""Unit tests for the dependency-free light console."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PYTHON_ROOT))

from http.server import BaseHTTPRequestHandler  # noqa: E402

from app import ExclusiveHTTPServer, LightApplication, load_config  # noqa: E402
from mqtt_client import (  # noqa: E402
    build_publish_packet,
    encode_remaining_length,
)


def sample_config(command_mode: str = "direct") -> dict:
    """Return a minimal valid configuration for isolated tests."""
    return {
        "mqtt": {
            "host": "127.0.0.1",
            "port": 1883,
            "base_topic": "boxchat",
            "command_mode": command_mode,
        },
        "web": {"host": "127.0.0.1", "port": 8080},
        "boards": [{"id": 32001, "name": "Test Light", "kind": "rgbw"}],
    }


class FakeMQTT:
    """Capture publishes without opening a broker connection."""

    connected = True
    last_error = ""

    def __init__(self) -> None:
        self.messages: list[tuple[str, str, bool]] = []

    def publish(self, topic: str, payload: str, retain: bool = False) -> bool:
        """Record a successful publish."""
        self.messages.append((topic, payload, retain))
        return True


class MQTTEncodingTests(unittest.TestCase):
    """Verify MQTT wire helpers independently of a real broker."""

    def test_remaining_length_boundaries(self) -> None:
        self.assertEqual(encode_remaining_length(0), b"\x00")
        self.assertEqual(encode_remaining_length(127), b"\x7f")
        self.assertEqual(encode_remaining_length(128), b"\x80\x01")

    def test_publish_packet_contains_topic_and_payload(self) -> None:
        packet = build_publish_packet("boxchat/32001/set", b"3255")
        self.assertEqual(packet[0], 0x30)
        self.assertIn(b"boxchat/32001/set", packet)
        self.assertTrue(packet.endswith(b"3255"))


class ConfigurationTests(unittest.TestCase):
    """Verify invalid board addressing is rejected early."""

    def test_load_config_accepts_unique_board_ids(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            config_path = Path(temp_directory) / "config.json"
            config_path.write_text(json.dumps(sample_config()), encoding="utf-8")
            loaded = load_config(config_path)
        self.assertEqual(loaded["boards"][0]["id"], 32001)

    def test_load_config_rejects_non_activation_id(self) -> None:
        config = sample_config()
        config["boards"][0]["id"] = 42
        with tempfile.TemporaryDirectory() as temp_directory:
            config_path = Path(temp_directory) / "config.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "above 32000"):
                load_config(config_path)

    def test_load_config_rejects_unknown_board_kind(self) -> None:
        config = sample_config()
        config["boards"][0]["kind"] = "mystery"
        with tempfile.TemporaryDirectory() as temp_directory:
            config_path = Path(temp_directory) / "config.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "kind must be"):
                load_config(config_path)


class PowerRelayConfigurationTests(unittest.TestCase):
    """Verify a light's optional master-power relay link is validated."""

    @staticmethod
    def _config_with(power_relay, relay_kind: str = "relay", relay_id: int = 32002) -> dict:
        config = sample_config()
        config["boards"].append({"id": relay_id, "name": "Relay", "kind": relay_kind,
                                 "command_mode": "shared"})
        config["boards"][0]["power_relay"] = power_relay
        return config

    def _load(self, config: dict) -> dict:
        with tempfile.TemporaryDirectory() as temp_directory:
            config_path = Path(temp_directory) / "config.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            return load_config(config_path)

    def test_valid_power_relay_is_kept(self) -> None:
        loaded = self._load(self._config_with(32002))
        self.assertEqual(loaded["boards"][0]["power_relay"], 32002)

    def test_power_relay_must_point_at_a_relay(self) -> None:
        with self.assertRaisesRegex(ValueError, "power_relay"):
            self._load(self._config_with(32002, relay_kind="rgbw"))

    def test_power_relay_cannot_be_self(self) -> None:
        with self.assertRaisesRegex(ValueError, "power_relay"):
            self._load(self._config_with(32001))

    def test_power_relay_must_exist(self) -> None:
        with self.assertRaisesRegex(ValueError, "power_relay"):
            self._load(self._config_with(39999))

    def test_relay_board_cannot_have_power_relay(self) -> None:
        config = self._config_with(32002)
        config["boards"][1]["power_relay"] = 32002  # The relay itself names a power_relay.
        with self.assertRaisesRegex(ValueError, "cannot have a power_relay"):
            self._load(config)


class CommandTests(unittest.TestCase):
    """Verify web actions become the firmware's numeric MQTT commands."""

    def test_direct_blue_command(self) -> None:
        application = LightApplication(sample_config("direct"))
        fake = FakeMQTT()
        application.mqtt = fake
        self.assertTrue(application.set_channel(32001, "b", 255))
        self.assertEqual(fake.messages, [("boxchat/32001/set", "3255", False)])

    def test_shared_command_activates_before_each_value(self) -> None:
        application = LightApplication(sample_config("shared"))
        fake = FakeMQTT()
        application.mqtt = fake
        self.assertTrue(application.set_channel(32001, "g", 128))
        self.assertEqual(
            fake.messages,
            [("boxchat", "32001", False), ("boxchat", "2128", False)],
        )

    def test_relay_power_uses_its_shared_protocol_override(self) -> None:
        config = sample_config("direct")
        config["boards"].append(
            {
                "id": 32002,
                "name": "Relay Lights",
                "kind": "relay",
                "command_mode": "shared",
            }
        )
        application = LightApplication(config)
        fake = FakeMQTT()
        application.mqtt = fake

        self.assertTrue(application.set_power(32002, True))
        self.assertEqual(
            fake.messages,
            [("boxchat", "32002", False), ("boxchat", "1", False)],
        )
        self.assertIs(application.status()["boards"]["32002"]["power"], True)

    def test_relay_power_requires_boolean(self) -> None:
        config = sample_config()
        config["boards"].append({"id": 32002, "kind": "relay", "command_mode": "shared"})
        application = LightApplication(config)
        with self.assertRaisesRegex(ValueError, "true or false"):
            application.set_power(32002, 1)

    def test_state_acknowledgement_updates_board(self) -> None:
        application = LightApplication(sample_config())
        application._on_mqtt_message(  # pylint: disable=protected-access
            "boxchat/32001/state",
            b'{"r":12,"g":34,"b":56,"w":78}',
        )
        state = application.status()["boards"]["32001"]
        self.assertEqual((state["r"], state["g"], state["b"], state["w"]), (12, 34, 56, 78))


def zoned_config(command_mode: str = "direct") -> dict:
    """Return a configuration whose only board has a left and a right zone."""
    config = sample_config(command_mode)
    config["boards"][0]["zones"] = ["Left", "Right"]
    return config


class ZoneCommandTests(unittest.TestCase):
    """Verify the boxchat zone standard: zone N's R/G/B/W blocks start at (4N-3)*1000."""

    def make_app(self, command_mode: str = "direct") -> tuple[LightApplication, FakeMQTT]:
        application = LightApplication(zoned_config(command_mode))
        fake = FakeMQTT()
        application.mqtt = fake
        return application, fake

    def test_zone_command_numbers_follow_the_standard(self) -> None:
        from app import zone_command  # noqa: E402 - imported here to keep the module header unchanged.
        self.assertEqual(zone_command(1, "r", 0), 1000)
        self.assertEqual(zone_command(1, "w", 255), 4255)
        self.assertEqual(zone_command(2, "r", 128), 5128)
        self.assertEqual(zone_command(2, "w", 255), 8255)
        self.assertEqual(zone_command(4, "w", 255), 16255)

    def test_channel_to_one_zone(self) -> None:
        application, fake = self.make_app()
        self.assertTrue(application.set_channel(32001, "g", 77, zone=2))
        self.assertEqual(fake.messages, [("boxchat/32001/set", "6077", False)])

    def test_channel_without_zone_reaches_every_zone(self) -> None:
        application, fake = self.make_app()
        self.assertTrue(application.set_channel(32001, "r", 128))
        self.assertEqual([p for _, p, _ in fake.messages], ["1128", "5128"])

    def test_color_to_right_zone_only(self) -> None:
        application, fake = self.make_app()
        self.assertTrue(application.set_color(32001, {"r": 1, "g": 2, "b": 3, "w": 4}, zone=2))
        self.assertEqual([p for _, p, _ in fake.messages], ["5001", "6002", "7003", "8004"])

    def test_shared_mode_arms_before_every_zone_command(self) -> None:
        application, fake = self.make_app("shared")
        self.assertTrue(application.set_channel(32001, "b", 9))
        self.assertEqual([p for _, p, _ in fake.messages], ["32001", "3009", "32001", "7009"])

    def test_invalid_zones_publish_nothing(self) -> None:
        application, fake = self.make_app()
        for zone in (0, 3, -1, True, "2", 1.0):
            with self.assertRaisesRegex(ValueError, "zone must be"):
                application.set_channel(32001, "r", 1, zone=zone)
            with self.assertRaisesRegex(ValueError, "zone must be"):
                application.set_color(32001, {"r": 1, "g": 1, "b": 1, "w": 1}, zone=zone)
        self.assertEqual(fake.messages, [])

    def test_single_zone_board_is_unchanged(self) -> None:
        application = LightApplication(sample_config())
        fake = FakeMQTT()
        application.mqtt = fake
        self.assertEqual(application.boards[32001]["zones"], ["Main"])
        self.assertTrue(application.set_channel(32001, "w", 10))
        self.assertEqual([p for _, p, _ in fake.messages], ["4010"])
        with self.assertRaisesRegex(ValueError, "1 through 1"):
            application.set_channel(32001, "w", 10, zone=2)

    def test_multi_zone_state_updates_each_zone(self) -> None:
        application, _ = self.make_app()
        application._on_mqtt_message(  # pylint: disable=protected-access
            "boxchat/32001/state",
            b'{"r":1,"g":2,"b":3,"w":4,"zones":[{"r":1,"g":2,"b":3,"w":4},{"r":9,"g":8,"b":7,"w":6}]}',
        )
        state = application.status()["boards"]["32001"]
        self.assertEqual(state["zones"], [{"r": 1, "g": 2, "b": 3, "w": 4}, {"r": 9, "g": 8, "b": 7, "w": 6}])
        self.assertEqual((state["r"], state["w"]), (1, 4))  # Top level mirrors zone 1.

    def test_flat_state_from_single_zone_firmware_updates_zone_one_only(self) -> None:
        application, _ = self.make_app()
        application._on_mqtt_message("boxchat/32001/state", b'{"r":5,"g":5,"b":5,"w":5}')  # pylint: disable=protected-access
        zones = application.status()["boards"]["32001"]["zones"]
        self.assertEqual(zones[0], {"r": 5, "g": 5, "b": 5, "w": 5})
        self.assertEqual(zones[1], {"r": 0, "g": 0, "b": 0, "w": 0})

    def test_bad_zone_state_is_rejected_without_partial_updates(self) -> None:
        application, _ = self.make_app()
        for payload in (b'{"r":1,"g":1,"b":1,"w":1,"zones":[{"r":2,"g":2,"b":2,"w":2},{"r":300,"g":0,"b":0,"w":0}]}',
                        b'{"r":1,"g":1,"b":1,"w":1,"zones":[]}',
                        b'{"r":1,"g":1,"b":1,"w":1,"zones":{"r":1}}'):
            application._on_mqtt_message("boxchat/32001/state", payload)  # pylint: disable=protected-access
        state = application.status()["boards"]["32001"]
        self.assertEqual(state["status"], "invalid state payload")
        self.assertEqual(state["zones"][0], {"r": 0, "g": 0, "b": 0, "w": 0})

    def test_status_copies_cannot_change_live_zone_state(self) -> None:
        application, _ = self.make_app()
        application.status()["boards"]["32001"]["zones"][0]["r"] = 99
        self.assertEqual(application.status()["boards"]["32001"]["zones"][0]["r"], 0)


class ZoneConfigurationTests(unittest.TestCase):
    """Verify zone lists are validated when the configuration loads."""

    def load(self, config: dict) -> dict:
        with tempfile.TemporaryDirectory() as temp_directory:
            config_path = Path(temp_directory) / "config.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            return load_config(config_path)

    def test_zone_names_are_trimmed_and_kept_in_order(self) -> None:
        config = sample_config()
        config["boards"][0]["zones"] = [" Left ", "Right"]
        self.assertEqual(self.load(config)["boards"][0]["zones"], ["Left", "Right"])

    def test_invalid_zone_lists_are_rejected(self) -> None:
        for zones in ([], ["A", "B", "C", "D", "E"], ["Left", "left"], ["Left", ""], "Left", [1, 2]):
            config = sample_config()
            config["boards"][0]["zones"] = zones
            with self.assertRaisesRegex(ValueError, "zone"):
                self.load(config)

    def test_relay_cannot_have_zones(self) -> None:
        config = sample_config()
        config["boards"].append({"id": 32002, "kind": "relay", "command_mode": "shared", "zones": ["A"]})
        with self.assertRaisesRegex(ValueError, "relay"):
            self.load(config)


class ServerBindingTests(unittest.TestCase):
    """Verify a second console cannot silently share the first one's port."""

    def test_second_server_on_same_port_is_refused(self) -> None:
        first = ExclusiveHTTPServer(("127.0.0.1", 0), BaseHTTPRequestHandler)  # Port 0 picks any free port.
        try:
            port = first.server_address[1]
            with self.assertRaises(OSError):
                ExclusiveHTTPServer(("0.0.0.0", port), BaseHTTPRequestHandler).server_close()  # Mirrors the real 0.0.0.0 bind.
        finally:
            first.server_close()
    # Completes the duplicate-server regression check.


if __name__ == "__main__":
    unittest.main()
