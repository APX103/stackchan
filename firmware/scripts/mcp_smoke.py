#!/usr/bin/env python3
"""Dependency-free smoke test for the StackChan MCP Agent (LAN MCP server).

Usage:
    python mcp_smoke.py <device-ip> [port] [--token TOKEN]

Covers: initialize handshake, tools/list pagination, and a representative
tools/call for every tool group (device / display / robot / led / reminder /
camera / audio-stream discovery). Exits non-zero on the first failure.
"""
import json
import sys
import urllib.request

BASE = None
TOKEN = None


def rpc(method, params=None, id=1):
    body = {"jsonrpc": "2.0", "id": id, "method": method}
    if params is not None:
        body["params"] = params
    return raw_call(body)


def raw_call(body):
    req = urllib.request.Request(
        BASE + "/mcp",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
    )
    if TOKEN:
        req.add_header("Authorization", "Bearer " + TOKEN)
    with urllib.request.urlopen(req, timeout=40) as resp:
        data = resp.read().decode()
    return json.loads(data) if data else None


def call_tool(name, args=None):
    return rpc("tools/call", {"name": name, "arguments": args or {}})


def check(label, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {label} {detail}")
    if not cond:
        sys.exit(1)


def main():
    global BASE, TOKEN
    ip = sys.argv[1]
    port = 8080
    if len(sys.argv) > 2 and sys.argv[2].isdigit():
        port = int(sys.argv[2])
    if "--token" in sys.argv:
        TOKEN = sys.argv[sys.argv.index("--token") + 1]
    BASE = f"http://{ip}:{port}"

    # -- server status endpoint --
    with urllib.request.urlopen(BASE + "/", timeout=5) as resp:
        status = json.loads(resp.read().decode())
    check("GET / status", status.get("name") == "stackchan-mcp", str(status.get("version")))

    # -- MCP handshake --
    r = rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                           "clientInfo": {"name": "smoke", "version": "0"}})
    check("initialize", r["result"]["serverInfo"]["name"] == "stackchan-mcp")
    # Notifications return 202 with an empty body
    raw_call({"jsonrpc": "2.0", "method": "notifications/initialized"})

    # -- tools/list with pagination --
    names, cursor = [], None
    while True:
        params = {"cursor": cursor} if cursor else {}
        r = rpc("tools/list", params)
        page = r["result"]["tools"]
        names += [t["name"] for t in page]
        cursor = r["result"].get("nextCursor")
        if not cursor:
            break
    expected = [
        "self.get_device_status", "self.get_system_info", "self.audio_speaker.set_volume",
        "self.screen.get_info", "self.screen.set_brightness",
        "self.display.set_emotion", "self.display.show_message", "self.display.clear_message",
        "self.robot.get_head_angles", "self.robot.set_head_angles", "self.robot.look_at",
        "self.robot.go_home", "self.robot.set_led_color",
        "self.robot.create_reminder", "self.robot.get_reminders", "self.robot.stop_reminder",
        "self.camera.take_photo", "self.microphone.stream", "self.speaker.stream",
    ]
    missing = [n for n in expected if n not in names]
    check(f"tools/list complete ({len(names)} tools)", not missing, f"missing: {missing}")

    # -- device group --
    r = call_tool("self.get_device_status")
    check("device_status", not r["result"].get("isError"), r["result"]["content"][0]["text"][:80])
    r = call_tool("self.get_system_info")
    check("system_info", not r["result"].get("isError"), r["result"]["content"][0]["text"][:80])
    r = call_tool("self.screen.get_info")
    check("screen_info", '"width":320' in r["result"]["content"][0]["text"])

    check("set_volume 25", not call_tool("self.audio_speaker.set_volume", {"volume": 25})["result"].get("isError"))
    check("set_volume 75", not call_tool("self.audio_speaker.set_volume", {"volume": 75})["result"].get("isError"))
    check("set_brightness 20", not call_tool("self.screen.set_brightness", {"brightness": 20})["result"].get("isError"))
    check("set_brightness 75", not call_tool("self.screen.set_brightness", {"brightness": 75})["result"].get("isError"))

    # -- display group --
    check("set_emotion happy", not call_tool("self.display.set_emotion", {"emotion": "happy"})["result"].get("isError"))
    check("show_message", not call_tool("self.display.show_message", {"text": "MCP smoke test", "duration_ms": 3000})["result"].get("isError"))
    check("clear_message", not call_tool("self.display.clear_message")["result"].get("isError"))

    # -- robot group --
    check("get_head_angles", not call_tool("self.robot.get_head_angles")["result"].get("isError"))
    check("set_head_angles", not call_tool("self.robot.set_head_angles", {"yaw": 20, "pitch": 40})["result"].get("isError"))
    check("look_at", not call_tool("self.robot.look_at", {"x": 0.5, "y": 0.0})["result"].get("isError"))
    check("go_home", not call_tool("self.robot.go_home")["result"].get("isError"))
    check("set_led_color", not call_tool("self.robot.set_led_color", {"red": 0, "green": 100, "blue": 0})["result"].get("isError"))

    # -- reminders --
    r = call_tool("self.robot.create_reminder", {"duration_seconds": 60, "message": "smoke"})
    rid = r["result"]["content"][0]["text"]
    check("create_reminder", rid.strip().lstrip("-").isdigit(), f"id={rid}")
    check("get_reminders", not call_tool("self.robot.get_reminders")["result"].get("isError"))
    check("stop_reminder", not call_tool("self.robot.stop_reminder", {"id": int(rid)})["result"].get("isError"))

    # -- camera (takes ~1 s) --
    r = call_tool("self.camera.take_photo", {"quality": 20})
    text = r["result"]["content"][0]["text"]
    check("take_photo", "/photo.jpg" in text, text[:100])
    photo_url = json.loads(text)["url"].replace("https://", "http://")
    photo_req = urllib.request.Request(photo_url)
    if TOKEN:
        photo_req.add_header("Authorization", "Bearer " + TOKEN)
    with urllib.request.urlopen(photo_req, timeout=10) as resp:
        data = resp.read()
    check("GET /photo.jpg", resp_ok(data) and data[:2] == b"\xff\xd8", f"{len(data)} bytes")

    # -- inline image response --
    r = call_tool("self.camera.take_photo", {"quality": 20, "embed": True})
    check("take_photo embed", any(part.get("type") == "image" and len(part.get("data", "")) > 100
                                 for part in r["result"]["content"]))

    # -- audio stream discovery --
    r = call_tool("self.microphone.stream")
    check("microphone.stream", "ws://" in r["result"]["content"][0]["text"])
    r = call_tool("self.speaker.stream")
    check("speaker.stream", "ws://" in r["result"]["content"][0]["text"])

    # -- error paths --
    r = call_tool("self.robot.set_head_angles", {"yaw": 9999})
    check("invalid arg -> isError", r["result"].get("isError") is True)
    r = call_tool("self.not_a_tool")
    check("unknown tool -> -32602", r.get("error", {}).get("code") == -32602)

    print("\nAll smoke tests passed.")


def resp_ok(data):
    return len(data) > 1000


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    main()
