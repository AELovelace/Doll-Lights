"""Small dependency-free MQTT 3.1.1 client used by the light web app."""

from __future__ import annotations

import queue
import select
import socket
import struct
import threading
import time
from collections.abc import Callable, Iterable


MessageCallback = Callable[[str, bytes], None]


def encode_remaining_length(length: int) -> bytes:
    """Encode MQTT's variable-length packet-size field."""
    if length < 0 or length > 268_435_455:
        raise ValueError("MQTT remaining length is out of range")

    encoded = bytearray()
    while True:
        digit = length % 128
        length //= 128
        if length:
            digit |= 0x80
        encoded.append(digit)
        if not length:
            return bytes(encoded)


def encode_text(value: str) -> bytes:
    """Encode one MQTT UTF-8 string with its two-byte length prefix."""
    encoded = value.encode("utf-8")
    if len(encoded) > 65_535:
        raise ValueError("MQTT text field is too long")
    return struct.pack("!H", len(encoded)) + encoded


def build_connect_packet(
    client_id: str,
    keepalive: int,
    username: str = "",
    password: str = "",
    will_topic: str = "",
    will_payload: bytes = b"",
    will_retain: bool = False,
) -> bytes:
    """Build a clean-session MQTT 3.1.1 CONNECT packet, optionally with a QoS-0 last-will message."""
    flags = 0x02  # Clean session prevents old subscriptions from accumulating.
    payload = encode_text(client_id)
    if will_topic:
        flags |= 0x04 | (0x20 if will_retain else 0)  # Will flag (+ will retain); will QoS stays 0.
        payload += encode_text(will_topic) + struct.pack("!H", len(will_payload)) + will_payload
    if username:
        flags |= 0x80
        payload += encode_text(username)
    if password:
        flags |= 0x40
        payload += encode_text(password)

    variable_header = encode_text("MQTT") + bytes((4, flags)) + struct.pack("!H", keepalive)
    body = variable_header + payload
    return bytes((0x10,)) + encode_remaining_length(len(body)) + body


def build_publish_packet(topic: str, payload: bytes, retain: bool = False) -> bytes:
    """Build a QoS-zero MQTT PUBLISH packet."""
    body = encode_text(topic) + payload
    header = 0x31 if retain else 0x30
    return bytes((header,)) + encode_remaining_length(len(body)) + body


def build_subscribe_packet(packet_id: int, topics: Iterable[str]) -> bytes:
    """Build a QoS-zero MQTT SUBSCRIBE packet for one or more filters."""
    payload = b"".join(encode_text(topic) + b"\x00" for topic in topics)
    body = struct.pack("!H", packet_id) + payload
    return b"\x82" + encode_remaining_length(len(body)) + body


class MQTTClient:
    """Maintain one reconnecting MQTT connection for web requests and state."""

    def __init__(
        self,
        host: str,
        port: int,
        client_id: str,
        subscriptions: Iterable[str],
        on_message: MessageCallback,
        username: str = "",
        password: str = "",
        keepalive: int = 30,
        will_topic: str = "",
        will_payload: str = "",
        will_retain: bool = False,
        on_connect: Callable[[], None] | None = None,
    ) -> None:
        self.will = (will_topic, will_payload.encode("utf-8"), will_retain)  # Broker publishes this if we vanish.
        self.on_connect = on_connect  # Called after every (re)connect, e.g. to announce "online".
        self.host = host
        self.port = port
        self.client_id = client_id
        self.username = username
        self.password = password
        self.keepalive = max(10, keepalive)
        self.subscriptions = tuple(subscriptions)
        self.on_message = on_message
        self._outgoing: queue.Queue[bytes] = queue.Queue(maxsize=256)
        self._connected = threading.Event()
        self._publish_lock = threading.Lock()  # Coordinate enqueue with disconnect cleanup.
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._status_lock = threading.Lock()
        self._last_error = "not started"

    @property
    def connected(self) -> bool:
        """Return whether the MQTT worker currently has a broker connection."""
        return self._connected.is_set()

    @property
    def last_error(self) -> str:
        """Return the most recent connection or protocol error."""
        with self._status_lock:
            return self._last_error

    def start(self) -> None:
        """Start the reconnecting MQTT worker once."""
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="mqtt-client", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """Request worker shutdown and wait briefly for it to finish."""
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)

    def publish(self, topic: str, payload: str, retain: bool = False) -> bool:
        """Queue a QoS-zero publish only while connected, avoiding stale commands."""
        with self._publish_lock:
            if not self.connected:
                return False
            try:
                packet = build_publish_packet(topic, payload.encode("utf-8"), retain)
                self._outgoing.put_nowait(packet)
                return True
            except queue.Full:
                self._set_error("outgoing MQTT queue is full")
                return False

    def _discard_pending(self) -> None:
        """Drop unsent work on disconnect so reconnection cannot replay old light changes."""
        with self._publish_lock:
            self._connected.clear()
            while True:
                try:
                    self._outgoing.get_nowait()
                except queue.Empty:
                    return

    def _set_error(self, message: str) -> None:
        """Store a concise status message for the HTTP status endpoint."""
        with self._status_lock:
            self._last_error = message

    def _run(self) -> None:
        """Reconnect forever until stopped, servicing publish and receive traffic."""
        while not self._stop.is_set():
            sock: socket.socket | None = None
            try:
                sock = socket.create_connection((self.host, self.port), timeout=5)
                sock.settimeout(5)
                sock.sendall(
                    build_connect_packet(
                        self.client_id,
                        self.keepalive,
                        self.username,
                        self.password,
                        *self.will,
                    )
                )
                packet_type, payload = self._read_packet(sock)
                if packet_type != 2 or len(payload) != 2 or payload[1] != 0:
                    code = payload[1] if len(payload) > 1 else "missing"
                    raise ConnectionError(f"broker rejected CONNECT with code {code}")

                sock.sendall(build_subscribe_packet(1, self.subscriptions))
                self._connected.set()
                self._set_error("")
                if self.on_connect:
                    try:
                        self.on_connect()  # Runs after _connected is set, so it may publish.
                    except Exception as exc:  # A callback bug must not kill the reconnect loop.
                        self._set_error(f"on_connect callback failed: {exc}")
                self._service_connection(sock)
            except (ConnectionError, OSError, ValueError) as exc:
                self._set_error(str(exc))
            finally:
                self._discard_pending()
                if sock:
                    try:
                        sock.close()
                    except OSError:
                        pass

            self._stop.wait(2)  # Keeps broker outages from creating a tight retry loop.

    def _service_connection(self, sock: socket.socket) -> None:
        """Exchange queued publishes, inbound state, and keepalive packets."""
        last_traffic = time.monotonic()
        sock.settimeout(5)

        while not self._stop.is_set():
            sent_packet = False
            for _ in range(32):
                try:
                    packet = self._outgoing.get_nowait()
                except queue.Empty:
                    break
                sock.sendall(packet)
                sent_packet = True
            if sent_packet:
                last_traffic = time.monotonic()

            readable, _, _ = select.select((sock,), (), (), 0.2)
            if readable:
                packet_type, payload = self._read_packet(sock)
                last_traffic = time.monotonic()
                if packet_type == 3:
                    self._handle_publish(payload)

            if time.monotonic() - last_traffic >= self.keepalive / 2:
                sock.sendall(b"\xC0\x00")  # PINGREQ keeps quiet local connections alive.
                last_traffic = time.monotonic()

    def _handle_publish(self, payload: bytes) -> None:
        """Decode a broker PUBLISH payload and forward its topic and data."""
        if len(payload) < 2:
            raise ConnectionError("received malformed MQTT PUBLISH")
        topic_length = struct.unpack("!H", payload[:2])[0]
        if len(payload) < 2 + topic_length:
            raise ConnectionError("received truncated MQTT PUBLISH")
        topic = payload[2 : 2 + topic_length].decode("utf-8")
        self.on_message(topic, payload[2 + topic_length :])

    @staticmethod
    def _read_packet(sock: socket.socket) -> tuple[int, bytes]:
        """Read one complete MQTT packet from a connected socket."""
        first = MQTTClient._recv_exact(sock, 1)[0]
        multiplier = 1
        remaining = 0
        for _ in range(4):
            digit = MQTTClient._recv_exact(sock, 1)[0]
            remaining += (digit & 0x7F) * multiplier
            if not digit & 0x80:
                return first >> 4, MQTTClient._recv_exact(sock, remaining)
            multiplier *= 128
        raise ConnectionError("invalid MQTT remaining-length field")

    @staticmethod
    def _recv_exact(sock: socket.socket, length: int) -> bytes:
        """Receive exactly the requested number of bytes or raise on disconnect."""
        chunks = bytearray()
        while len(chunks) < length:
            chunk = sock.recv(length - len(chunks))
            if not chunk:
                raise ConnectionError("MQTT broker closed the connection")
            chunks.extend(chunk)
        return bytes(chunks)
