#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
D12 WiFi OTA Server
Listen on TCP 9000, push firmware to device using 4-step protocol: HELLO/INFO/DATA(N)/END.
Usage: python ota_server.py <firmware.bin>
"""
import socket
import base64
import zlib
import json
import sys
import time

HOST = '0.0.0.0'
PORT = 9000                # must match boot_ota.h OTA_SERVER_PORT
BLOCK_SIZE = 1024          # must match boot_ota.h OTA_BLOCK_SIZE (before base64)
FRAME_GAP_MS = 50          # gap between frames to help ESP8266 +IPD fragmentation

def recv_line(conn, timeout=3):
    """Read one line ending with \r\n. Return string. Return None on disconnect.
       On timeout: return whatever was received (if any), so a HELLO without
       trailing \r\n can still be parsed."""
    conn.settimeout(timeout)
    buf = b''
    while True:
        try:
            b = conn.recv(1)
            if not b:
                return None
            buf += b
            if buf.endswith(b'\r\n'):
                return buf[:-2].decode('utf-8', errors='replace')
        except socket.timeout:
            return buf.decode('utf-8', errors='replace') if buf else None

def send_json(conn, obj):
    """Send JSON + \r\n. Compact format (no spaces) so boot_ota.c strstr can match."""
    s = json.dumps(obj, separators=(',', ':')) + '\r\n'
    conn.sendall(s.encode('utf-8'))

def handle_device(conn, fw_data, fw_size, fw_crc, packets):
    # 1. Wait HELLO
    line = recv_line(conn, 15)
    if line is None:
        print("[OTA] !!! No HELLO, timeout")
        return
    print(f"[OTA] RX HELLO: {line}")
    try:
        hello = json.loads(line)
    except Exception as e:
        print(f"[OTA] !!! HELLO parse fail: {e}")
        return
    if hello.get('type') != 'HELLO':
        print("[OTA] !!! Not HELLO")
        return
    dev = hello.get('dev', '?')
    exp_crc = hello.get('expect_crc', 0)
    exp_size = hello.get('expect_size', 0)
    print(f"[OTA]   dev={dev} expect_crc=0x{exp_crc:08X} expect_size={exp_size}")
    if exp_crc != 0xFFFFFFFF and exp_crc != fw_crc:
        print(f"[OTA] !!! device expect_crc=0x{exp_crc:08X} != server fw_crc=0x{fw_crc:08X}, but send anyway")

    # 2. Send INFO
    info = {
        "type": "INFO",
        "size": fw_size,
        "crc": fw_crc,            # decimal, boot_ota.c parses with strtoul
        "packets": packets,
        "pktsize": BLOCK_SIZE
    }
    send_json(conn, info)
    print(f"[OTA] TX INFO: size={fw_size} crc=0x{fw_crc:08X} packets={packets} pktsize={BLOCK_SIZE}")


    #Wait for STM32 to complete Flash erase and assert READY before sending DATA.
    print("[OTA] Waiting for READY (device erased flash)...")
    ready_line = recv_line(conn, 30)   # 30 s，erase 464KB 
    if ready_line is None:
        print("[OTA] !!! No READY, device timeout")
        return
    try:
        ready = json.loads(ready_line)
    except Exception:
        print(f"[OTA] !!! Invalid READY: {ready_line}")
        return
    if ready.get('type') != 'READY':
        print(f"[OTA] !!! Unexpected response before DATA: {ready_line}")
        return
    print(f"[OTA] RX READY: {ready_line}")

    # 3. Send DATA x N
    for seq in range(packets):
        offset = seq * BLOCK_SIZE
        chunk = fw_data[offset:offset + BLOCK_SIZE]
        b64 = base64.b64encode(chunk).decode('ascii')
        frame = {"type": "DATA", "seq": seq, "data": b64}
        ack_ok = False
        for attempt in range(3):
            send_json(conn, frame)
            if seq % 20 == 0 or seq == packets - 1:
                print(f"[OTA] TX DATA seq={seq+1}/{packets} ({len(chunk)} bytes), try={attempt+1}")
            ack = recv_line(conn, 5)   # 5 秒超时
            if ack is None:
                continue
            try:
                ack_obj = json.loads(ack)
            except Exception:
                print(f"[OTA] !!! Invalid ACK for seq={seq}: {ack}")
                continue
            if ack_obj.get('type') == 'ACK' and ack_obj.get('seq') == seq:
                ack_ok = True
                break
            print(f"[OTA] !!! Unexpected ACK for seq={seq}: {ack}")
        if not ack_ok:
            print(f"[OTA] !!! No valid ACK for seq={seq}, aborting")
            return
        time.sleep(FRAME_GAP_MS / 1000.0)

    # 4. Send END
    end = {"type": "END", "total_bytes": fw_size}
    send_json(conn, end)
    print(f"[OTA] TX END: total_bytes={fw_size}")
    print("[OTA] Done. Device should verify CRC and JumpToApp.")

    # Drain any trailing bytes from device
    try:
        conn.settimeout(3)
        while True:
            b = conn.recv(64)
            if not b:
                break
    except Exception:
        pass

def main():
    if len(sys.argv) < 2:
        print("Usage: python ota_server.py <firmware.bin>")
        sys.exit(1)
    fw_path = sys.argv[1]
    with open(fw_path, 'rb') as f:
        fw_data = f.read()
    fw_size = len(fw_data)
    fw_crc = zlib.crc32(fw_data) & 0xFFFFFFFF   # same as STM32 CRC32_Calc (IEEE 802.3)
    if fw_size == 0 or fw_size > 0x74000:
        print(f"Invalid firmware size {fw_size}; expected 1..0x74000 bytes")
        sys.exit(1)
    packets = (fw_size + BLOCK_SIZE - 1) // BLOCK_SIZE
    print(f"[OTA] FW: {fw_path}")
    print(f"[OTA]   size={fw_size} crc=0x{fw_crc:08X} packets={packets} block={BLOCK_SIZE}")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(1)
    print(f"[OTA] Listening on {HOST}:{PORT}, waiting device...")
    print("[OTA] (auto-push firmware when device connects, Ctrl+C to exit)")

    try:
        while True:
            conn, addr = srv.accept()
            print(f"\n[OTA] === Device connected from {addr[0]}:{addr[1]} ===")
            try:
                handle_device(conn, fw_data, fw_size, fw_crc, packets)
            except Exception as e:
                print(f"[OTA] !!! handle error: {e}")
            conn.close()
            print("[OTA] Connection closed, waiting next...")
    except KeyboardInterrupt:
        print("\n[OTA] Bye")
    finally:
        srv.close()

if __name__ == '__main__':
    main()
