"""Tests for the Cync bridge's boxchat parsing, colour mapping, Telink crypto and MQTT last-will packet."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

PYTHON_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PYTHON_ROOT))

HAS_CRYPTO = importlib.util.find_spec("cryptography") is not None  # Erinne's venv may not have it.

from mqtt_client import build_connect_packet  # noqa: E402


@unittest.skipUnless(HAS_CRYPTO, "needs the cryptography package (py -m pip install --user cryptography)")
class CyncBridgeTests(unittest.TestCase):
    """Never touches Bluetooth or MQTT: pure logic only."""

    def setUp(self) -> None:
        from cync_bridge import BoxchatBoard
        self.board = BoxchatBoard(32005)

    def test_crypto_matches_published_vectors(self) -> None:
        from cync_mesh import self_test
        self.assertTrue(self_test())

    def test_direct_commands_follow_the_zone_1_standard(self) -> None:
        self.assertEqual(self.board.handle("boxchat/32005/set", b"1200"), (True, []))
        self.assertEqual(self.board.handle("boxchat/32005/set", b"4050\n"), (True, []))
        self.assertEqual(self.board.levels, [200, 0, 0, 50])
        self.assertEqual(self.board.handle("boxchat/32005/set", b"0"), (True, []))
        self.assertEqual(self.board.levels, [0, 0, 0, 0])

    def test_other_zones_and_bad_payloads_are_reported(self) -> None:
        status = "boxchat/32005/status"
        self.assertEqual(self.board.handle("boxchat/32005/set", b"5128"), (False, [(status, "unknown command")]))
        self.assertEqual(self.board.handle("boxchat/32005/set", b"1300"), (False, [(status, "unknown command")]))
        self.assertEqual(self.board.handle("boxchat/32005/set", b"red"), (False, [(status, "invalid numeric command")]))
        self.assertEqual(self.board.handle("boxchat/32005/set", b""), (False, [(status, "invalid payload length")]))
        self.assertEqual(self.board.levels, [0, 0, 0, 0])

    def test_shared_topic_needs_arming_for_exactly_one_command(self) -> None:
        self.assertEqual(self.board.handle("boxchat", b"2255"), (False, []))  # Not armed: ignored quietly.
        self.assertEqual(self.board.handle("boxchat", b"32005"), (False, [("boxchat/32005/status", "ready")]))
        self.assertEqual(self.board.handle("boxchat", b"2255"), (True, []))
        self.assertEqual(self.board.handle("boxchat", b"3255"), (False, []))  # Disarmed again.
        self.board.handle("boxchat", b"32005")
        self.board.handle("boxchat", b"32004")  # Another board's number disarms this one.
        self.assertEqual(self.board.handle("boxchat", b"3255"), (False, []))
        self.assertEqual(self.board.levels, [0, 255, 0, 0])

    def test_state_json_matches_the_firmware_shape(self) -> None:
        self.board.handle("boxchat/32005/set", b"1009")
        self.assertEqual(self.board.state_json(), '{"r":9,"g":0,"b":0,"w":0,"zones":[{"r":9,"g":0,"b":0,"w":0}]}')

    def test_colour_plans(self) -> None:
        from cync_bridge import WHITE_WARMTH, plan_strip
        from cync_mesh import OP_BRIGHTNESS, OP_COLOR, OP_POWER
        self.assertEqual(plan_strip([0, 0, 0, 0]), [(OP_POWER, b"\x00")])
        self.assertEqual(plan_strip([0, 0, 128, 0]),
                         [(OP_POWER, b"\x01"), (OP_COLOR, bytes([4, 0, 0, 255])), (OP_BRIGHTNESS, bytes([50]))])
        self.assertEqual(plan_strip([255, 0, 0, 255])[1], (OP_COLOR, bytes([4, 255, 255, 255])))  # W mixed in.
        self.assertEqual(plan_strip([0, 0, 0, 255]),
                         [(OP_POWER, b"\x01"), (OP_COLOR, bytes([5, WHITE_WARMTH])), (OP_BRIGHTNESS, bytes([100]))])
        self.assertEqual(plan_strip([0, 0, 0, 1])[2], (OP_BRIGHTNESS, bytes([1])))  # Never rounds a lit strip to 0.


class LastWillPacketTests(unittest.TestCase):
    def test_connect_without_will_is_unchanged(self) -> None:
        self.assertEqual(build_connect_packet("id", 30)[9], 0x02)  # Flags byte: clean session only.

    def test_connect_with_retained_will(self) -> None:
        packet = build_connect_packet("id", 30, will_topic="boxchat/32005/status", will_payload=b"offline", will_retain=True)
        self.assertEqual(packet[9], 0x02 | 0x04 | 0x20)  # Clean session + will + will retain.
        self.assertTrue(packet.endswith(b"\x00\x14boxchat/32005/status\x00\x07offline"))


if __name__ == "__main__":
    unittest.main()
