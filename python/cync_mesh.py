"""Telink-mesh protocol for GE Cync / "C by GE" Bluetooth lights, plus a read-only login probe.

This is our own implementation of the protocol as published by these Apache-2.0 projects (credit to them
for the reverse engineering): juanboro/cync2mqtt (src/acync/mesh.py), google/python-dimond, and
dustindog101/cync-cli (native/main.swift, including the crypto self-test vectors used below).

Usage (from the project root):
    py python/cync_mesh.py --self-test                       # crypto check, no hardware
    py python/cync_mesh.py --probe 34:13:43:73:FC:C1         # try login with common factory defaults
    py python/cync_mesh.py --probe ADDR --name N --password P  # try one specific mesh name/password

The probe only logs in: it never changes the light's settings or sends light commands.
Needs:  py -m pip install --user bleak cryptography
"""

from __future__ import annotations

import argparse
import asyncio
import os

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# GATT characteristics of the Telink mesh service 00010203-0405-0607-0809-0a0b0c0d1910.
CHAR_NOTIFY = "00010203-0405-0607-0809-0a0b0c0d1911"  # Status notifications.
CHAR_COMMAND = "00010203-0405-0607-0809-0a0b0c0d1912"  # Encrypted light commands.
CHAR_PAIR = "00010203-0405-0607-0809-0a0b0c0d1914"  # Login (and, in the Telink SDK, pairing).

LOGIN_REQUEST = 0x0C  # First byte of the login packet written to CHAR_PAIR.
VENDOR_ID = (0x11, 0x02)  # 0x0211 (Telink), little-endian, in every command packet.

# Opcodes for Cync lights (from cync2mqtt / cync-cli); used by the bridge, not by the probe.
OP_POWER = 0xD0  # params [1] on / [0] off
OP_BRIGHTNESS = 0xD2  # params [0-100]
OP_COLOR = 0xE2  # params [0x04, r, g, b] for RGB, [0x05, 0-100] for white temperature

# Unverified guesses for a factory-reset Telink device (Telink SDK conventions); the probe tries them.
DEFAULT_CREDENTIALS = [
    ("out_of_mesh", "123"),
    ("telink_mesh1", "123"),
    ("out_of_mesh", "123456"),
    ("telink_mesh1", "123456"),
]


def aes_reversed(key: bytes, data: bytes) -> bytes:
    """Telink's AES: AES-128-ECB with key, input and output all byte-reversed."""
    encryptor = Cipher(algorithms.AES(bytes(reversed(key))), modes.ECB()).encryptor()
    return bytes(reversed(encryptor.update(bytes(reversed(data))) + encryptor.finalize()))
    # Completes one 16-byte block.


def pad16(text: str) -> bytes:
    """Pad (or cut) a mesh name or password to exactly 16 bytes with NULs."""
    return text.encode("utf-8")[:16].ljust(16, b"\x00")
    # Completes the fixed-width credential field.


def credential_xor(name: str, password: str) -> bytes:
    """name XOR password, the 16-byte secret both login and session key are built from."""
    return bytes(a ^ b for a, b in zip(pad16(name), pad16(password)))
    # Completes the shared secret.


def login_proof(name: str, password: str, random8: bytes) -> bytes:
    """The 8-byte proof that we (or the light) know the mesh name and password."""
    return aes_reversed(random8 + bytes(8), credential_xor(name, password))[:8]
    # Completes the proof for one side's random value.


def login_packet(name: str, password: str, app_random: bytes) -> bytes:
    """17-byte login request: 0x0C, our 8 random bytes, our 8-byte proof."""
    return bytes([LOGIN_REQUEST]) + app_random + login_proof(name, password, app_random)
    # Completes the packet written to CHAR_PAIR.


def session_key(name: str, password: str, app_random: bytes, device_random: bytes) -> bytes:
    """Session key = AES(name XOR password, our random || the light's random)."""
    return aes_reversed(credential_xor(name, password), app_random[:8] + device_random[:8])
    # Completes the key used to encrypt every command in this connection.


def mac_bytes(address: str) -> bytes:
    """The light's MAC in the reversed byte order the protocol uses."""
    return bytes(reversed(bytes.fromhex(address.replace(":", ""))))
    # Completes the address field for nonces.


def encrypt_command(sk: bytes, address: str, sequence: int, destination: int, opcode: int, params: bytes) -> bytes:
    """Build and encrypt one 20-byte command packet (sequence, MIC, destination, opcode, vendor, params)."""
    addr = mac_bytes(address)
    packet = bytearray(20)
    packet[0:3] = sequence.to_bytes(3, "little")  # Must keep increasing; the light rejects replays.
    packet[5:7] = destination.to_bytes(2, "little")  # Mesh node id of the target light.
    packet[7] = opcode
    packet[8:10] = bytes(VENDOR_ID)
    packet[10:10 + len(params)] = params
    nonce = addr[:4] + b"\x01" + bytes(packet[0:3])
    authenticator = bytearray(aes_reversed(sk, nonce + b"\x0f" + bytes(7)))
    for i in range(15):
        authenticator[i] ^= packet[5 + i]
    packet[3:5] = aes_reversed(sk, bytes(authenticator))[:2]  # 2-byte message integrity code.
    keystream = aes_reversed(sk, b"\x00" + nonce + bytes(7))
    for i in range(15):
        packet[5 + i] ^= keystream[i]
    return bytes(packet)
    # Completes a packet ready to write to CHAR_COMMAND.


def self_test() -> bool:
    """Check the crypto against cync-cli's published (synthetic) test vectors."""
    sk = session_key("test_mesh", "test_key", bytes(range(8)), bytes(range(8, 16)))
    packet = login_packet("telink_mesh1", "123", bytes(range(0xA0, 0xA8)))
    checks = [
        ("session key", sk.hex(), "4dd0bcdea1361f61f048f4872578e15b"),
        ("login packet", packet.hex(), "0ca0a1a2a3a4a5a6a78db674711b855a79"),
    ]
    ok = True
    for label, got, want in checks:
        print(f"{label:<13} {'OK  ' if got == want else 'FAIL'} {got}" + ("" if got == want else f" (want {want})"))
        ok &= got == want
    return ok
    # Completes the offline crypto check.


OP_STATUS_QUERY = 0xDA  # Asks every node to report; replies arrive as OP_STATUS notifications.
OP_STATUS = 0xDC  # Status notification: two slots of [node id, online flag, brightness|0x80, colour].
BROADCAST = 0xFFFF  # Mesh destination meaning "every node".


def decrypt_notification(sk: bytes, address: str, packet: bytes) -> bytes:
    """Decrypt an inbound notification from CHAR_NOTIFY (bytes 7 onward are encrypted)."""
    keystream = aes_reversed(sk, b"\x00" + mac_bytes(address)[:3] + packet[:5] + bytes(8))
    data = bytearray(packet)
    for i in range(len(data) - 7):
        data[7 + i] ^= keystream[i]
    return bytes(data)
    # Completes one decrypted notification.


class MeshSession:
    """One logged-in connection to a light; sends encrypted commands and collects status reports."""

    def __init__(self, client, address: str, sk: bytes) -> None:
        self.client, self.address, self.sk = client, address, sk
        self.sequence = int.from_bytes(os.urandom(2), "little")  # Random start, then always increasing.
        self.statuses: list[bytes] = []  # Decrypted OP_STATUS notifications, newest last.

    async def send(self, destination: int, opcode: int, params: bytes = b"") -> None:
        packet = encrypt_command(self.sk, self.address, self.sequence, destination, opcode, params)
        self.sequence = (self.sequence + 1) & 0xFFFFFF
        await self.client.write_gatt_char(CHAR_COMMAND, packet, response=True)
    # Completes one command write.

    async def enable_status(self) -> bool:
        """Subscribe to CHAR_NOTIFY and switch reports on; returns False if the light refuses."""
        def on_notify(_char, data: bytearray) -> None:
            plain = decrypt_notification(self.sk, self.address, bytes(data))
            if len(plain) > 7 and plain[7] == OP_STATUS:
                self.statuses.append(plain)
        try:
            await self.client.start_notify(CHAR_NOTIFY, on_notify)
            await self.client.write_gatt_char(CHAR_NOTIFY, b"\x01", response=True)  # "Start reporting."
            return True
        except Exception as exc:
            print(f"  (status notifications unavailable: {type(exc).__name__}: {exc})")
            return False
    # Completes the status subscription.

    def node_ids(self) -> list[int]:
        """Node ids seen in status reports so far (slots at bytes 10 and 14; id 0 means an empty slot)."""
        ids = []
        for plain in self.statuses:
            for slot in (10, 14):
                if len(plain) > slot + 3 and plain[slot] and plain[slot] not in ids:
                    ids.append(plain[slot])
        return ids
    # Completes the list of discovered nodes.


async def open_session(address: str, name: str, password: str):
    """Connect and log in; returns (client, MeshSession). Raises if the credentials are refused."""
    from bleak import BleakClient, BleakScanner

    device = await BleakScanner.find_device_by_address(address, timeout=20)
    if device is None:
        raise ConnectionError("light not found (is it powered, and not connected to a phone app?)")
    client = BleakClient(device, timeout=20)
    await client.connect()
    app_random = os.urandom(8)
    await client.write_gatt_char(CHAR_PAIR, login_packet(name, password, app_random), response=True)
    await asyncio.sleep(0.3)
    reply = bytes(await client.read_gatt_char(CHAR_PAIR))
    if not (len(reply) >= 17 and reply[9:17] == login_proof(name, password, reply[1:9])):
        await client.disconnect()
        raise PermissionError(f"login refused (reply {reply.hex() or 'empty'})")
    return client, MeshSession(client, address, session_key(name, password, app_random, reply[1:9]))
    # Completes connect + login.


async def blue_test(address: str, name: str, password: str) -> None:
    """Visible check: find the node id, then power on, blue for 3 seconds, then off."""
    client, session = await open_session(address, name, password)
    try:
        print("  logged in")
        if await session.enable_status():
            await session.send(BROADCAST, OP_STATUS_QUERY)
            await asyncio.sleep(2)  # Gives the light time to report.
        ids = session.node_ids()
        print(f"  node ids reported: {ids or 'none'}")
        target = ids[0] if ids else BROADCAST  # Falls back to "every node" if nothing reported.
        print(f"  sending to {'node ' + str(target) if ids else 'broadcast 0xFFFF'}: on, blue, (3 s), off")
        await session.send(target, OP_POWER, b"\x01")
        await session.send(target, OP_BRIGHTNESS, bytes([100]))
        await session.send(target, OP_COLOR, bytes([0x04, 0, 0, 255]))
        await asyncio.sleep(3)
        await session.send(target, OP_POWER, b"\x00")
        print("  done")
    finally:
        await client.disconnect()
    # Completes the visible test.


PAIR_SET_NAME, PAIR_SET_PASSWORD, PAIR_SET_LTK = 0x04, 0x05, 0x06  # Telink pairing writes to CHAR_PAIR.
PAIR_ACCEPTED = 0x07  # CHAR_PAIR reads back 0x07 when the new mesh settings were taken.


def new_credentials(address: str) -> dict:
    """Random mesh password and long-term key, plus a neutral mesh name (the name is broadcast)."""
    import secrets
    import string
    alphabet = string.ascii_letters + string.digits
    return {
        "address": address,
        "mesh_name": "boxchat-32005",  # Advertised to anyone scanning, so nothing personal in it.
        "mesh_password": "".join(secrets.choice(alphabet) for _ in range(16)),
        "long_term_key": "".join(secrets.choice(alphabet) for _ in range(16)),
        "destination": BROADCAST,  # The strip doesn't report its node id, so commands go to every node.
    }
    # Completes a fresh set of credentials.


async def set_mesh(address: str, old_name: str, old_password: str, new: dict) -> bool:
    """Log in with the current credentials, then write a new mesh name, password and long-term key."""
    client, session = await open_session(address, old_name, old_password)
    try:
        for opcode, value in ((PAIR_SET_NAME, new["mesh_name"]), (PAIR_SET_PASSWORD, new["mesh_password"]),
                              (PAIR_SET_LTK, new["long_term_key"])):
            encrypted = aes_reversed(session.sk, pad16(value))  # Each field travels encrypted with the session key.
            await client.write_gatt_char(CHAR_PAIR, bytes([opcode]) + encrypted, response=True)
        await asyncio.sleep(1)  # The light stores the settings before answering.
        reply = bytes(await client.read_gatt_char(CHAR_PAIR))
        print(f"  pairing reply: {reply.hex() or 'empty'}")
        return bool(reply) and reply[0] == PAIR_ACCEPTED
    finally:
        await client.disconnect()
    # Completes the credential change; True means the light accepted it.


async def try_login(address: str, name: str, password: str) -> str:
    """Connect, attempt one login, and report whether the light proved it knows the same credentials."""
    from bleak import BleakClient, BleakScanner  # Imported here so --self-test needs no Bluetooth stack.

    device = await BleakScanner.find_device_by_address(address, timeout=20)
    if device is None:
        return "not found (is it powered, and not connected to a phone app?)"
    async with BleakClient(device, timeout=20) as client:
        app_random = os.urandom(8)
        await client.write_gatt_char(CHAR_PAIR, login_packet(name, password, app_random), response=True)
        await asyncio.sleep(0.3)  # The light needs a moment before its reply is readable.
        reply = bytes(await client.read_gatt_char(CHAR_PAIR))
    if len(reply) >= 17 and reply[9:17] == login_proof(name, password, reply[1:9]):
        return f"LOGIN OK (reply {reply[:1].hex()}...)"  # The light's proof matches: credentials are right.
    return f"rejected (reply {reply.hex() or 'empty'})"
    # Completes one read-only login attempt; the connection closes on exit.


async def probe(address: str, candidates: list[tuple[str, str]]) -> None:
    """Try each candidate mesh name/password in its own connection and print the result."""
    for name, password in candidates:
        try:
            result = await try_login(address, name, password)
        except Exception as exc:  # A refused login can also drop the link; report and move on.
            result = f"error: {type(exc).__name__}: {exc}"
        print(f"  {name!r:<16} / {password!r:<10} -> {result}", flush=True)
    # Completes the probe.


def main() -> None:
    """Run the self-test or the read-only login probe."""
    parser = argparse.ArgumentParser(description="Telink-mesh tools for Cync / C by GE lights.")
    parser.add_argument("--self-test", action="store_true", help="check the crypto against known vectors")
    parser.add_argument("--probe", metavar="ADDRESS", help="try logging in to this light (read-only)")
    parser.add_argument("--name", help="mesh name to try (default: try common factory defaults)")
    parser.add_argument("--password", default="", help="mesh password to try with --name")
    parser.add_argument("--blue-test", metavar="ADDRESS", help="log in, then on / blue 3 s / off (needs --name/--password)")
    parser.add_argument("--set-mesh", metavar="ADDRESS",
                        help="replace the light's mesh credentials (log in with --name/--password); "
                             "the new ones are saved to --secrets BEFORE anything is written")
    parser.add_argument("--secrets", default=os.path.join(os.path.dirname(__file__), "cync_secrets.json"),
                        help="where the bridge's mesh credentials live (git-ignored)")
    args = parser.parse_args()

    if args.set_mesh:
        import json
        if os.path.exists(args.secrets):
            raise SystemExit(f"{args.secrets} already exists; refusing to overwrite saved credentials.")
        new = new_credentials(args.set_mesh)
        with open(args.secrets, "w", encoding="utf-8") as handle:
            json.dump(new, handle, indent=2)  # Saved first, so the credentials can never be lost mid-change.
        print(f"Saved new credentials to {args.secrets}; writing them to {args.set_mesh}...")
        accepted = asyncio.run(set_mesh(args.set_mesh, args.name, args.password, new))
        print("  ACCEPTED" if accepted else "  NOT accepted (the saved file is kept; a factory reset restores defaults)")
        raise SystemExit(0 if accepted else 1)

    if args.self_test:
        raise SystemExit(0 if self_test() else 1)
    if args.blue_test:
        print(f"Blue test on {args.blue_test}:")
        asyncio.run(blue_test(args.blue_test, args.name, args.password))
        return
    if args.probe:
        candidates = [(args.name, args.password)] if args.name else DEFAULT_CREDENTIALS
        print(f"Probing {args.probe} (login attempts only; nothing on the light is changed):")
        asyncio.run(probe(args.probe, candidates))
        return
    parser.print_help()
    # Completes the command-line entry point.


if __name__ == "__main__":
    main()
