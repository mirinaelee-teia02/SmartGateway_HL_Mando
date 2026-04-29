#!/usr/bin/env python3
"""
SmartGateway UDP 수신 테스트 스크립트
MessagePack 형식의 ADC 데이터를 수신하여 출력합니다.

사용법:
    pip install msgpack
    python udp_receiver.py [port]

기본 포트: 8888
"""

import socket
import sys

try:
    import msgpack
except ImportError:
    print("msgpack 필요: pip install msgpack")
    sys.exit(1)

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8888

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", PORT))
print(f"[UDP] Listening on 0.0.0.0:{PORT}")
print("[UDP] Waiting for SmartGateway... (Ctrl+C to stop)")
print("-" * 60)

try:
    while True:
        data, addr = sock.recvfrom(256)
        try:
            d = msgpack.unpackb(bytes(data), strict_map_key=False)
            line = d.get("line", 0)
            dt = d.get("datetime", [0, 0, 0, 0, 0, 0])
            raw = d.get("raw", [])
            mn = d.get("min", [])
            mx = d.get("max", [])
            ts = f"{dt[0]:04d}-{dt[1]:02d}-{dt[2]:02d} {dt[3]:02d}:{dt[4]:02d}:{dt[5]:02d}"
            print(f"[{ts}] Line {line} | raw={raw} | min={mn} | max={mx}")
        except Exception as e:
            print(f"[ERR] {e} | raw={data[:32].hex()}...")
finally:
    sock.close()
