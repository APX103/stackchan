#!/usr/bin/env python3
"""Device WebSocket lifecycle tests for the standalone MCP Agent.

Requires: pip install websocket-client
Usage: python mcp_stream_test.py <device-ip> [port] [--token TOKEN] [--out FILE]

The output is an Ogg/Opus file that FFmpeg can decode directly. Network
playback is checked here; audible speaker quality still requires an on-site
listener.
"""
import json
import struct
import sys
import time
import urllib.parse
import urllib.request

import websocket


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_ogg(path, packets):
    serial = 0x5354434D

    def page(payload, sequence, granule, flags=0):
        segments = []
        remaining = len(payload)
        while remaining >= 255:
            segments.append(255)
            remaining -= 255
        segments.append(remaining)
        header = struct.pack("<4sBBQIIIB", b"OggS", 0, flags, granule,
                             serial, sequence, 0, len(segments))
        data = bytearray(header + bytes(segments) + payload)
        crc = 0
        for byte in data:
            crc ^= byte << 24
            for _ in range(8):
                crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
        data[22:26] = struct.pack("<I", crc)
        return data

    head = b"OpusHead" + bytes([1, 1]) + struct.pack("<HIHB", 0, 16000, 0, 0)
    tags = b"OpusTags" + struct.pack("<II", 0, 0)
    with open(path, "wb") as output:
        output.write(page(head, 0, 0, 2))
        output.write(page(tags, 1, 0))
        for index, packet in enumerate(packets):
            output.write(page(packet, index + 2, (index + 1) * 2880))


class StreamTest:
    def __init__(self, ip, port=8080, token=None, out="mcp_capture.opus"):
        self.base = f"http://{ip}:{port}"
        self.ip = ip
        self.port = port
        self.token = token
        self.out = out

    def status(self):
        with urllib.request.urlopen(self.base + "/", timeout=5) as response:
            return json.loads(response.read().decode())

    def wait_inactive(self, key, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not self.status()[key]:
                return
            time.sleep(0.1)
        raise RuntimeError(f"{key} remained active after disconnect")

    def open(self, path):
        url = f"ws://{self.ip}:{self.port}/{path}"
        if self.token:
            url += "?token=" + urllib.parse.quote(self.token, safe="")
        last = None
        for _ in range(50):
            ws = websocket.create_connection(url, timeout=5)
            ws.send('{"type":"start"}')
            response = json.loads(ws.recv())
            if response.get("type") == "hello":
                check(response.get("sample_rate") == 16000, response)
                check(response.get("channels") == 1, response)
                check(response.get("frame_ms") == 60, response)
                check(response.get("codec") == "opus", response)
                return ws
            ws.close()
            last = response
            if "restarting" not in response.get("message", ""):
                break
            time.sleep(0.1)
        details = ""
        try:
            status = self.status()
            details = f"; free_sram={status.get('free_sram')}, largest={status.get('largest_sram_block')}"
        except Exception:
            pass
        raise RuntimeError(f"could not start /{path}: {last}{details}")

    def next_binary(self, ws, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ws.settimeout(max(0.1, deadline - time.monotonic()))
            data = ws.recv()
            if isinstance(data, bytes):
                check(len(data) > 0, "empty Opus packet")
                return data
        raise RuntimeError("no binary audio packet before deadline")

    @staticmethod
    def abort(ws):
        ws.abort()

    def run(self):
        packets = []
        mic = self.open("mic")
        url = f"ws://{self.ip}:{self.port}/mic"
        if self.token:
            url += "?token=" + urllib.parse.quote(self.token, safe="")
        second = websocket.create_connection(url, timeout=5)
        second.send('{"type":"start"}')
        busy = json.loads(second.recv())
        second.close()
        check(busy.get("message") == "busy", f"second mic was not rejected: {busy}")
        print("[PASS] second mic client -> busy")

        while len(packets) < 40:
            packet = self.next_binary(mic)
            packets.append(packet)
        write_ogg(self.out, packets)
        mic_stack = self.status()["mic_stack_free"]
        check(mic_stack >= 1024, f"mic stack margin is too small: {mic_stack}")
        print(f"[PASS] mic stack high-water margin: {mic_stack} bytes")
        mic.send('{"type":"stop"}')
        mic.close()
        self.wait_inactive("mic_active")
        print(f"[PASS] captured {len(packets)} non-empty Opus packets -> {self.out}")

        for attempt in range(1, 4):
            mic = self.open("mic")
            self.next_binary(mic)
            self.abort(mic)
            self.wait_inactive("mic_active")
            print(f"[PASS] mic abrupt disconnect/reconnect {attempt}/3")

        speaker = self.open("speaker")
        for packet in packets[:20]:
            speaker.send_binary(packet)
            time.sleep(0.06)
        speaker_stack = self.status()["spk_stack_free"]
        check(speaker_stack >= 1024, f"speaker stack margin is too small: {speaker_stack}")
        print(f"[PASS] speaker stack high-water margin: {speaker_stack} bytes")
        speaker.send('{"type":"stop"}')
        speaker.close()
        self.wait_inactive("spk_active")
        print("[PASS] paced speaker packets and clean stop (network path)")

        speaker = self.open("speaker")
        for index in range(100):
            speaker.send_binary(packets[index % len(packets)])
        speaker.settimeout(0.25)
        backlog = False
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                data = speaker.recv()
            except websocket.WebSocketTimeoutException:
                continue
            if isinstance(data, str) and "backlog" in data:
                backlog = True
                break
        check(backlog, "speaker overload did not report bounded backlog")
        self.abort(speaker)
        self.wait_inactive("spk_active")
        print("[PASS] speaker overload -> explicit backlog, abrupt disconnect released")

        mic = self.open("mic")
        speaker = self.open("speaker")
        for packet in packets[:8]:
            speaker.send_binary(packet)
            self.next_binary(mic)
            time.sleep(0.06)
        self.abort(mic)
        self.abort(speaker)
        self.wait_inactive("mic_active")
        self.wait_inactive("spk_active")
        print("[PASS] simultaneous mic/speaker teardown")

        mic = self.open("mic")
        self.next_binary(mic)
        mic.send('{"type":"stop"}')
        mic.close()
        self.wait_inactive("mic_active")
        speaker = self.open("speaker")
        speaker.send_binary(packets[0])
        speaker.send('{"type":"stop"}')
        speaker.close()
        self.wait_inactive("spk_active")
        print("[PASS] both streams restart after full-duplex disconnect")
        print("\nAll WebSocket lifecycle tests passed; audible quality is not judged.")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    ip = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 8080
    token = sys.argv[sys.argv.index("--token") + 1] if "--token" in sys.argv else None
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else "mcp_capture.opus"
    StreamTest(ip, port, token, out).run()


if __name__ == "__main__":
    main()
