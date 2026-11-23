"""HTTP integration tests exercise real handlers without operating physical lights."""

from __future__ import annotations

import http.client
import json
import os
from pathlib import Path
import threading
import time
import unittest
from unittest.mock import patch
from http.server import ThreadingHTTPServer

from test_app import FakeMQTT, sample_config
from app import LightApplication, make_handler
from export_erinne_templates import build_templates
from lighting_api import load_tokens


READ_TOKEN = "r" * 40
CONTROL_TOKEN = "c" * 40


class LightingHTTPTests(unittest.TestCase):
    """Verify discovery, scope enforcement, routing, and failure responses over HTTP."""

    def setUp(self):
        config = sample_config()
        config["boards"] += [
            {"id": 32002, "name": "Relay", "kind": "relay", "command_mode": "shared"},
            {"id": 42003, "name": "Second RGBW", "kind": "rgbw", "command_mode": "shared"},
        ]
        with patch.dict(os.environ, {"LIGHT_CONSOLE_READ_TOKEN": READ_TOKEN,
                                    "LIGHT_CONSOLE_CONTROL_TOKEN": CONTROL_TOKEN}):
            self.app = LightApplication(config)
        self.mqtt = FakeMQTT()
        self.app.mqtt = self.mqtt  # No network broker or real hardware is contacted.
        handler = make_handler(self.app)
        handler.log_message = lambda *args: None
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": .01})
        self.worker.start()
        self.addCleanup(self.close_server)

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=2)

    def request(self, path, body=None, token=CONTROL_TOKEN, *, method=None, raw=None):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=2)
        try:
            headers = {"Content-Type": "application/json"}
            if token is not None:
                headers["Authorization"] = "Bearer " + token
            data = raw if raw is not None else (json.dumps(body) if body is not None else None)
            connection.request(method or ("POST" if data is not None else "GET"), path, data, headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def test_discovery_covers_all_configured_lights_and_unknown_telemetry(self):
        code, result = self.request("/api/lights", token=READ_TOKEN)
        self.assertEqual(code, 200)
        self.assertEqual([r["id"] for r in result["lights"]], [32001, 32002, 42003])
        self.assertIsNone(result["lights"][0]["r"])
        self.assertFalse(result["lights"][1]["supports_color"])
        self.assertTrue(result["lights"][2]["supports_color"])
        self.assertNotIn(READ_TOKEN, json.dumps(result))

    def test_discovery_pagination_and_validation(self):
        code, result = self.request("/api/lights?offset=1&limit=1")
        self.assertEqual(code, 200)
        self.assertEqual(result["lights"][0]["id"], 32002)
        self.assertEqual(result["next_offset"], 2)
        for query in ("limit=101", "offset=-1", "limit=x", "limit=1&limit=2", "unknown=1"):
            self.assertEqual(self.request("/api/lights?" + query)[0], 400)

    def test_authentication_covers_new_and_original_routes(self):
        for path in ("/api/lights", "/api/status", "/api/config", "http://localhost/api/lights"):
            for token in (None, "wrong"):
                self.assertEqual(self.request(path, token=token)[0], 401)
        for path, body in (("/api/lights/32001/color", {"r": 1, "g": 2, "b": 3}),
                           ("/api/boards/32001/color", {"r": 1, "g": 2, "b": 3, "w": 0}),
                           ("/api/boards/32001/channel", {"channel": "r", "value": 1}),
                           ("/api/boards/32002/power", {"power": True}), ("/api/all/off", {})):
            self.assertEqual(self.request(path, body, token=READ_TOKEN)[0], 403)
            self.assertEqual(self.request(path, body, token=None)[0], 401)
        self.assertEqual(self.mqtt.messages, [])

    def test_unconfigured_external_api_is_disabled_but_legacy_ui_still_works(self):
        self.app.api_tokens = ("", "")
        self.assertEqual(self.request("/api/lights", token=None)[0], 503)
        self.assertEqual(self.request("/api/lights/32001/color", {}, token=None)[0], 503)
        self.assertEqual(self.request("/api/status", token=None)[0], 200)

    def test_full_rgb_color_defaults_white_off_and_does_not_claim_confirmation(self):
        code, result = self.request("/api/lights/32001/color", {"r": 255, "g": 20, "b": 30})
        self.assertEqual(code, 202)
        self.assertFalse(result["confirmed"])
        self.assertEqual(self.mqtt.messages, [("boxchat/32001/set", p, False)
                                              for p in ("1255", "2020", "3030", "4000")])
        self.assertIsNone(self.request("/api/lights/32001")[1]["r"])

    def test_color_routes_to_another_shared_board(self):
        self.assertEqual(self.request("/api/lights/42003/color", {"r": 9, "g": 8, "b": 7, "w": 6})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages],
                         ["42003", "1009", "42003", "2008", "42003", "3007", "42003", "4006"])

    def test_bad_colors_publish_nothing(self):
        for body in ({"r": True, "g": 0, "b": 0}, {"r": 256, "g": 0, "b": 0},
                     {"r": 1.2, "g": 0, "b": 0}, {"r": "10", "g": 0, "b": 0},
                     {"r": 1, "b": 2}, {"r": 0, "g": 0, "b": 0, "w": -1},
                     {"r": 0, "g": 0, "b": 0, "topic": "arbitrary"}):
            self.assertEqual(self.request("/api/lights/32001/color", body)[0], 400)
        self.assertEqual(self.request("/api/lights/32002/color", {"r": 1, "g": 2, "b": 3})[0], 400)
        self.assertEqual(self.request("/api/lights/99999/color", {"r": 1, "g": 2, "b": 3})[0], 404)
        self.assertEqual(self.request("/api/lights/99999")[0], 404)
        self.assertEqual(self.mqtt.messages, [])

    def test_channel_and_power_actions(self):
        self.assertEqual(self.request("/api/lights/32001/channel", {"channel": "w", "value": 128})[0], 202)
        self.assertEqual(self.mqtt.messages[-1][1], "4128")
        self.assertEqual(self.request("/api/lights/32002/power", {"power": "on"})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages[-2:]], ["32002", "1"])
        relay = self.request("/api/lights/32002")[1]
        self.assertTrue(relay["power"])
        self.assertEqual(relay["state_source"], "last_command")
        self.assertEqual(self.request("/api/lights/32002/power", {"power": "off"})[0], 202)
        self.assertEqual(self.mqtt.messages[-1][1], "0")
        self.assertEqual(self.request("/api/lights/32001/power", {"power": "on"})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages[-4:]], ["1000", "2000", "3000", "4255"])
        self.assertEqual(self.request("/api/lights/32001/power", {"power": "off"})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages[-4:]], ["1000", "2000", "3000", "4000"])
        self.assertEqual(self.request("/api/lights/32002/power", {"power": True})[0], 400)

    def test_malformed_requests_are_json_errors(self):
        for raw in ("[1,2]", "null", "{", " " * 4097, b'\xff'):
            code, body = self.request("/api/lights/32001/color", raw=raw)
            self.assertEqual(code, 400)
            self.assertIn("error", body)
        self.assertEqual(self.mqtt.messages, [])

    def test_broker_failure_stops_shared_activation_and_reports_unconfirmed(self):
        self.mqtt.publish = lambda *args: False
        self.assertEqual(self.request("/api/lights/42003/color", {"r": 1, "g": 2, "b": 3})[0], 503)
        self.assertEqual(self.mqtt.messages, [])

    def test_rgbw_ack_and_invalid_telemetry(self):
        self.app._on_mqtt_message("boxchat/32001/state", b'{"r":1,"g":2,"b":3,"w":4}')
        result = self.request("/api/lights/32001")[1]
        self.assertEqual(result["state_source"], "reported")
        self.assertEqual(result["r"], 1)
        self.assertIsNotNone(result["last_state_at"])
        for payload in (b'[]', b'null', b'{"r":true}', b'invalid'):
            self.app._on_mqtt_message("boxchat/32001/state", payload)
        self.assertEqual(self.request("/api/lights/32001")[1]["r"], 1)

    def test_concurrent_colors_are_not_interleaved(self):
        original = self.mqtt.publish
        def slow_publish(*args):
            time.sleep(.001)  # Make concurrent requests overlap between channel publications.
            return original(*args)
        self.mqtt.publish = slow_publish
        values = [{"r": n, "g": n, "b": n, "w": n} for n in (10, 20)]
        threads = [threading.Thread(target=self.app.set_color, args=(32001, color)) for color in values]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=2)
        packets = [p for _, p, _ in self.mqtt.messages]
        groups = [[str(base + n) for base in (1000, 2000, 3000, 4000)] for n in (10, 20)]
        self.assertIn(packets, [groups[0] + groups[1], groups[1] + groups[0]])


class ZoneHTTPTests(unittest.TestCase):
    """Exercise zone routing over real HTTP using the shared fixture plus a left/right board."""

    def setUp(self):
        fixture = LightingHTTPTests()  # Reuses the authenticated server fixture without re-running its tests.
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        board = {"id": 32004, "name": "Room", "kind": "rgbw", "zones": ["Left", "Right"]}
        fixture.app.boards[32004] = board
        with fixture.app._state_lock:
            fixture.app._board_state[32004] = {"status": "unknown", "last_seen": None, "last_state_at": None,
                                               "r": 0, "g": 0, "b": 0, "w": 0,
                                               "zones": [{"r": 0, "g": 0, "b": 0, "w": 0} for _ in range(2)]}
        self.app, self.mqtt, self.request = fixture.app, fixture.mqtt, fixture.request

    def test_zone_color_and_null_zone_routes(self):
        self.assertEqual(self.request("/api/lights/32004/color", {"r": 10, "g": 0, "b": 0, "zone": 2})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages], ["5010", "6000", "7000", "8000"])
        self.mqtt.messages.clear()
        self.assertEqual(self.request("/api/lights/32004/channel", {"channel": "w", "value": 5, "zone": None})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages], ["4005", "8005"])
        self.mqtt.messages.clear()
        self.assertEqual(self.request("/api/lights/32004/power", {"power": "off", "zone": 1})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages], ["1000", "2000", "3000", "4000"])

    def test_browser_routes_accept_zone(self):
        self.assertEqual(self.request("/api/boards/32004/channel", {"channel": "r", "value": 200, "zone": 2})[0], 202)
        self.assertEqual(self.request("/api/boards/32004/color", {"r": 1, "g": 2, "b": 3, "w": 4, "zone": 1})[0], 202)
        self.assertEqual([p for _, p, _ in self.mqtt.messages], ["5200", "1001", "2002", "3003", "4004"])

    def test_bad_zones_are_rejected_before_publishing(self):
        for path, body in (("/api/lights/32004/color", {"r": 1, "g": 2, "b": 3, "zone": 3}),
                           ("/api/lights/32004/channel", {"channel": "r", "value": 1, "zone": "left"}),
                           ("/api/boards/32004/channel", {"channel": "r", "value": 1, "zone": 0}),
                           ("/api/lights/32002/power", {"power": "on", "zone": 1})):
            self.assertEqual(self.request(path, body)[0], 400, path)
        self.assertEqual(self.mqtt.messages, [])
        self.assertEqual(self.request("/api/lights/32002/power", {"power": "on", "zone": None})[0], 202)

    def test_light_row_lists_zones_and_hides_mixed_top_level_color(self):
        row = self.request("/api/lights/32004")[1]
        self.assertEqual(row["zone_count"], 2)
        self.assertEqual([(z["zone"], z["name"], z["r"]) for z in row["zones"]], [(1, "Left", None), (2, "Right", None)])
        self.app._on_mqtt_message("boxchat/32004/state",
                                  b'{"r":0,"g":0,"b":0,"w":0,"zones":[{"r":0,"g":0,"b":0,"w":0},{"r":255,"g":0,"b":0,"w":0}]}')
        row = self.request("/api/lights/32004")[1]
        self.assertIsNone(row["r"])  # Zones differ, so no single color describes the light.
        self.assertTrue(row["power"])
        self.assertEqual(row["zones"][1]["r"], 255)
        self.app._on_mqtt_message("boxchat/32004/state",
                                  b'{"r":7,"g":0,"b":0,"w":0,"zones":[{"r":7,"g":0,"b":0,"w":0},{"r":7,"g":0,"b":0,"w":0}]}')
        self.assertEqual(self.request("/api/lights/32004")[1]["r"], 7)
        self.assertEqual(self.request("/api/lights/32002")[1]["zones"], [])


class AccessAndTransportTests(unittest.TestCase):
    def test_tokens_fail_closed_if_partial_short_or_equal(self):
        for read, control in ((READ_TOKEN, ""), ("short", CONTROL_TOKEN), (READ_TOKEN, READ_TOKEN),
                              ("x" * 40 + "\n", CONTROL_TOKEN)):
            with patch.dict(os.environ, {"LIGHT_CONSOLE_READ_TOKEN": read,
                                        "LIGHT_CONSOLE_CONTROL_TOKEN": control}):
                with self.assertRaises(ValueError):
                    load_tokens()

    def test_unsent_mqtt_commands_are_discarded_on_disconnect(self):
        with patch.dict(os.environ, {"LIGHT_CONSOLE_READ_TOKEN": "", "LIGHT_CONSOLE_CONTROL_TOKEN": ""}):
            app = LightApplication(sample_config())
        mqtt = app.mqtt
        mqtt._connected.set()
        self.assertTrue(mqtt.publish("boxchat/32001/set", "1255"))
        mqtt._discard_pending()
        self.assertTrue(mqtt._outgoing.empty())
        self.assertFalse(mqtt.publish("boxchat/32001/set", "1255"))
        mqtt._connected.set()
        self.assertTrue(mqtt._outgoing.empty())

    def test_packaged_templates_match_generator_and_separate_changes(self):
        templates = build_templates("https://lights.example.invalid")
        root = Path(__file__).resolve().parents[1] / "api-templates"
        for filename, template in zip(("light-console-read.json", "light-console-control.json"), templates):
            self.assertEqual(json.loads((root / filename).read_text()), template)
        self.assertTrue(all(not op.get("action") for op in templates[0]["operations"]))
        self.assertTrue(all(op["action"] for op in templates[1]["operations"]))
        for template in templates:
            self.assertEqual(template["auth_type"], "bearer")
            self.assertNotIn("secret", template)
            self.assertTrue(all(len(op["example_questions"]) >= 4 for op in template["operations"]))


if __name__ == "__main__":
    unittest.main()
