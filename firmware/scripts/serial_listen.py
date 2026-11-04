#!/usr/bin/env python3
"""Passive serial logger: opens the port once, then only LISTENS (never
writes, never re-opens — re-opening pulses a USB reset on this board).

Usage: python serial_listen.py [seconds] [outfile]
"""
import sys
import time

import serial

seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 180
outfile = sys.argv[2] if len(sys.argv) > 2 else "serial_log.txt"

ser = serial.Serial()
ser.port = "COM26"
ser.baudrate = 115200
ser.timeout = 0.5
ser.dtr = False
ser.rts = False
ser.open()

start = time.time()
count = 0
with open(outfile, "wb") as f:
    while time.time() - start < seconds:
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()
            count += len(data)
ser.close()
print(f"captured {count} bytes -> {outfile}")
