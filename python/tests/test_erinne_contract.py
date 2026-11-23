"""Optional contract check against the actual Erinne checkout and its request builder."""

import json
import os
from pathlib import Path
import sys
import unittest
from urllib.parse import urlencode, urlsplit

import test_lighting_api as fixtures
from export_erinne_templates import build_templates


@unittest.skipUnless(os.environ.get("ERINNE_ROOT"), "set ERINNE_ROOT and use Erinne's Python environment")
class ErinneContractTests(unittest.TestCase):
    def test_real_parser_and_request_builder_can_call_every_operation(self):
        sys.path.insert(0, os.environ["ERINNE_ROOT"])
        from mnemerynne.apis import build_request, parse_template, validate_params

        fixture = fixtures.LightingHTTPTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        base_url = "http://127.0.0.1:" + str(fixture.server.server_address[1])
        # Parse the distributed artifacts too, so the imported files are covered directly.
        for file in (Path(__file__).resolve().parents[1] / "api-templates").glob("*.json"):
            parse_template(file.read_text(encoding="utf-8"))
        for template in build_templates(base_url):
            parsed = parse_template(json.dumps(template))
            for operation in parsed["operations"]:
                given = {"light_id": 32001, "r": 12, "g": 34, "b": 56,
                         "channel": "w", "value": 78, "power": "off"}
                values = validate_params(operation["params"],
                                         {p["name"]: given[p["name"]] for p in operation["params"] if p["name"] in given})
                record = {**operation, "params": json.dumps(operation["params"])}
                method, url, query, body = build_request(parsed, record, values)
                path = urlsplit(url).path + ("?" + urlencode(query) if query else "")
                code, response = fixture.request(path, raw=body, method=method,
                                                 token=fixtures.CONTROL_TOKEN if operation["action"] else fixtures.READ_TOKEN)
                self.assertEqual(code, 202 if operation["action"] else 200,
                                 (operation["name"], response))
        self.assertEqual(len(fixture.mqtt.messages), 9)  # Full color, one channel, and full RGBW off.


if __name__ == "__main__":
    unittest.main()
