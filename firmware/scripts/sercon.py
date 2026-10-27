#!/usr/bin/env python3
"""USB console driver for the StackChan serial console (hal/serial_console.h).

Opens the port WITHOUT asserting DTR/RTS (asserting them resets an ESP32-S3
USB-Serial-JTAG into download mode!) and drives the launcher head-lessly.

Usage:
    python sercon.py page
    python sercon.py next | prev | go | reboot
    python sercon.py open MCP.AGENT        # navigate the launcher + open

Needs pyserial:  pip install pyserial
"""
import sys
import time

import serial


def open_port(port="COM26", baud=115200):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False  # applied at open: no download-mode reset
    ser.rts = False
    ser.open()
    return ser


def drain(ser, seconds=0.5):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        out += ser.read(4096)
    return out.decode("utf-8", errors="replace")


def ask_page(ser):
    ser.reset_input_buffer()
    ser.write(b"page\n")
    for _ in range(10):
        time.sleep(0.15)
        for line in drain(ser, 0.1).splitlines():
            if line.startswith("PAGE:"):
                parts = line.split(maxsplit=2)
                return parts[1], parts[2] if len(parts) > 2 else "?"
    return None, None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd = sys.argv[1].lower()
    port = "COM26"
    for i, a in enumerate(sys.argv):
        if a == "--port" and i + 1 < len(sys.argv):
            port = sys.argv[i + 1]

    ser = open_port(port)

    if cmd in ("page", "next", "prev", "go", "reboot"):
        if cmd == "page":
            app_id, name = ask_page(ser)
            print(f"PAGE: {app_id} {name}" if app_id else "no answer (launcher busy/booting?)")
        else:
            ser.write((cmd + "\n").encode())
            print(drain(ser, 0.8) or "(no output)")
    elif cmd == "open":
        target = sys.argv[2].upper()
        last_seen = None
        stable = 0
        for _ in range(30):
            app_id, name = ask_page(ser)
            if app_id is None:
                print("waiting for launcher...")
                time.sleep(1)
                continue
            if name.upper() == target:
                # Require a second identical report so the scroll animation
                # has settled before "tapping" the icon.
                stable += 1
                if stable >= 2:
                    ser.write(b"go\n")
                    print(f"opening {name}")
                    ser.close()
                    return
                time.sleep(0.4)
                continue
            stable = 0
            ser.write(b"next\n")
            time.sleep(0.7)  # let the scroll animation settle
        print(f"app {target} not found")
    else:
        print(__doc__)
        sys.exit(2)
    ser.close()


if __name__ == "__main__":
    main()
