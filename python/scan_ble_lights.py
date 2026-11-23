"""Passively list nearby Bluetooth Low Energy devices and flag likely LED lights.

It only listens to advertisements: it never connects, pairs, or writes to any device.

Usage (from the project root):
    py python/scan_ble_lights.py            # 20-second scan, likely lights only
    py python/scan_ble_lights.py -s 40 -a   # 40 seconds, and list every device seen

Needs the bleak library:  py -m pip install --user bleak
"""

from __future__ import annotations

import argparse
import asyncio
import re

from bleak import BleakScanner

# Name patterns used by common Bluetooth LED controllers and bulbs (a hint, not proof).
LIGHT_NAME = re.compile(
    r"LED|BLEDOM|ELK-|Triones|QHM-|HappyLighting|LEDnet|LEDBLE|Zengge|MELK|BJ_LED|SP1\d\d|Govee|ihoment|GBK_|"
    r"Hue|Magic|Lamp|Light|Bulb|Strip|iLink|Lotus|Duoco|Mohuan|SYLVANIA|Yeelight|Nanoleaf|Wiz",
    re.IGNORECASE,
)

# 16-bit service IDs that LED controllers commonly advertise (a hint, not proof).
LIGHT_SERVICES = {
    "fe0f": "Philips Hue (Signify) service",
    "ffd5": "Triones/HappyLighting-style service",
    "ffd0": "Triones/HappyLighting-style service",
    "fff0": "ELK-BLEDOM/UART-style service",
    "ffe0": "UART-style (HM-10 type) service",
    "1828": "Bluetooth Mesh proxy (mesh bulbs/strips)",
    "1827": "Bluetooth Mesh provisioning (unpaired mesh device)",
}

# A few Bluetooth SIG company IDs: Telink makes the mesh chips in many smart bulbs; the rest only
# explain common non-light devices.
KNOWN_MAKERS = {0x0211: "Telink", 0x004C: "Apple", 0x0006: "Microsoft", 0x0075: "Samsung", 0x00E0: "Google"}


def short_uuid(uuid: str) -> str:
    """Return 'fe0f' for a standard 0000fe0f-0000-1000-8000-00805f9b34fb UUID, else the UUID."""
    match = re.fullmatch(r"0000([0-9a-f]{4})-0000-1000-8000-00805f9b34fb", uuid.lower())
    return match.group(1) if match else uuid.lower()
    # Completes the short form used for matching and display.


async def scan(seconds: float) -> dict[str, dict]:
    """Listen for advertisements and merge everything heard from each address."""
    seen: dict[str, dict] = {}

    def on_advert(device, adv) -> None:
        entry = seen.setdefault(device.address, {"name": "", "rssi": -999, "services": set(), "makers": set(), "count": 0})
        entry["count"] += 1
        entry["name"] = adv.local_name or device.name or entry["name"]  # Keeps the latest non-empty name.
        entry["rssi"] = max(entry["rssi"], adv.rssi)  # Strongest signal = nearest reading.
        entry["services"].update(short_uuid(u) for u in adv.service_uuids)
        entry["makers"].update(adv.manufacturer_data.keys())  # Bluetooth SIG company IDs.
    # Completes recording of one advertisement.

    scanner = BleakScanner(detection_callback=on_advert, scanning_mode="active")  # Active = also ask for names.
    await scanner.start()
    await asyncio.sleep(seconds)
    await scanner.stop()
    return seen
    # Completes the timed scan.


def describe(address: str, entry: dict) -> dict:
    """Turn one device's merged adverts into a printable row with the reasons it may be a light."""
    reasons = ["name"] if entry["name"] and LIGHT_NAME.search(entry["name"]) else []
    reasons += [LIGHT_SERVICES[s] for s in sorted(entry["services"]) if s in LIGHT_SERVICES]
    makers = ", ".join(KNOWN_MAKERS.get(m, f"0x{m:04X}") for m in sorted(entry["makers"]))
    return {
        "likely": bool(reasons), "name": entry["name"] or "(no name)", "address": address,
        "rssi": entry["rssi"], "why": "; ".join(reasons), "services": ",".join(sorted(entry["services"])),
        "makers": makers, "count": entry["count"],
    }
    # Completes one row.


def print_rows(rows: list[dict]) -> None:
    """Print rows as a simple aligned table, nearest first."""
    for row in sorted(rows, key=lambda r: r["rssi"], reverse=True):
        print(f"  {row['rssi']:>4} dBm  {row['address']}  {row['name'][:28]:<28}  "
              f"{row['why'] or '-':<34}  services={row['services'] or '-'}  maker={row['makers'] or '-'}")
    # Completes the table.


def main() -> None:
    """Scan, then report likely lights (and optionally everything)."""
    parser = argparse.ArgumentParser(description="Passively find Bluetooth LE LED lights nearby.")
    parser.add_argument("-s", "--seconds", type=float, default=20, help="how long to listen (default 20)")
    parser.add_argument("-a", "--all", action="store_true", help="also list devices that don't look like lights")
    args = parser.parse_args()

    print(f"Listening for Bluetooth LE devices for {args.seconds:g} seconds (passive; nothing is changed)...")
    rows = [describe(address, entry) for address, entry in asyncio.run(scan(args.seconds)).items()]
    likely = [row for row in rows if row["likely"]]
    print(f"\nDevices seen: {len(rows)}. Likely lights: {len(likely)}.")
    print("RSSI: about -40 dBm is right beside the PC, -90 dBm is far away.\n")
    if likely:
        print("Likely lights:")
        print_rows(likely)
    else:
        print("Nothing looked like a light. Check the lights are powered and not connected to a phone app "
              "(many stop advertising while connected), then try -s 40 -a.")
    if args.all:
        print("\nAll devices:")
        print_rows(rows)
    # Completes the report.


if __name__ == "__main__":
    main()
