#!/usr/bin/env python3
"""WebSocket audio test helpers for the StackChan MCP Agent.

Requires: pip install websocket-client

Record 10 s from the robot's microphone:
    python ws_audio_test.py mic <device-ip> [out.opus] [--token TOKEN]

Play an Opus file to the robot's speaker (16 kHz mono 60 ms self-contained
frames, as produced by `ffmpeg -i in.wav -ac 1 -ar 16000 -c:a libopus
-application voip -frame_duration 60 -f data out.opus`):
    python ws_audio_test.py speaker <device-ip> play.opus [--token TOKEN]
"""
import json
import sys
import time

import websocket  # websocket-client


def record(ip, port, out, token):
    url = f"ws://{ip}:{port}/mic"
    if token:
        url += "?token=" + token
    ws = websocket.create_connection(url, timeout=5)
    ws.send('{"type":"start"}')
    hello = json.loads(ws.recv())
    assert hello.get("type") == "hello", hello
    print("hello:", hello)
    end = time.time() + 10
    frames = 0
    with open(out, "wb") as f:
        while time.time() < end:
            ws.settimeout(max(0.5, end - time.time()))
            try:
                data = ws.recv()
            except websocket.WebSocketTimeoutException:
                break
            if isinstance(data, bytes):
                # length-prefixed packet so the file is splittable
                f.write(len(data).to_bytes(2, "little") + data)
                frames += 1
    ws.send('{"type":"stop"}')
    ws.close()
    print(f"captured {frames} opus packets (~{frames * 0.06:.1f}s) -> {out}")
    print("decode with: python ws_audio_test.py unwrap <in.opus> <out.opus> && "
          "ffmpeg -c:a libopus -ar 16000 -ac 1 -i out.opus out.wav")


def unwrap(inp, outp):
    with open(inp, "rb") as f, open(outp, "wb") as o:
        while True:
            hdr = f.read(2)
            if len(hdr) < 2:
                break
            n = int.from_bytes(hdr, "little")
            o.write(f.read(n))
    print(f"unwrapped -> {outp}")


def play(ip, port, path, token):
    url = f"ws://{ip}:{port}/speaker"
    if token:
        url += "?token=" + token
    ws = websocket.create_connection(url, timeout=5)
    ws.send('{"type":"start"}')
    hello = json.loads(ws.recv())
    assert hello.get("type") == "hello", hello
    print("hello:", hello)
    sent = 0
    with open(path, "rb") as f:
        while True:
            hdr = f.read(2)
            if len(hdr) < 2:
                break
            pkt = f.read(int.from_bytes(hdr, "little"))
            if len(pkt) == 0:
                break
            ws.send_binary(pkt)
            sent += 1
            time.sleep(0.06)  # pace at the frame duration
    ws.send('{"type":"stop"}')
    ws.close()
    print(f"sent {sent} packets (~{sent * 0.06:.1f}s)")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    mode = sys.argv[1]
    token = sys.argv[sys.argv.index("--token") + 1] if "--token" in sys.argv else None
    if mode == "unwrap":
        unwrap(sys.argv[2], sys.argv[3])
        return
    ip = sys.argv[2]
    port = 8080
    if len(sys.argv) > 3 and sys.argv[3].isdigit():
        port = int(sys.argv[3])
    if mode == "mic":
        record(ip, port, sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].isdigit() else "mic.opus", token)
    elif mode == "speaker":
        play(ip, port, sys.argv[3], token)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
