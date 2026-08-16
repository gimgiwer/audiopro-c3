#!/usr/bin/env python3
import os
import sys
import time
import termios
import select
import argparse

parser = argparse.ArgumentParser(description="Audio Pro Addon C3 (Linkplay A28) - U-Boot SDRAM RAM Boot Tool")
parser.add_argument("-p", "--port", default=os.getenv("UART_PORT", "/dev/ttyUSB0"), help="Serial port (default: /dev/ttyUSB0)")
parser.add_argument("-b", "--baud", type=int, default=57600, help="Baud rate (default: 57600)")
parser.add_argument("-d", "--device-ip", default="192.168.1.122", help="Device IP for TFTP (default: 192.168.1.122)")
parser.add_argument("-s", "--server-ip", default="192.168.1.202", help="TFTP server IP (default: 192.168.1.202)")
parser.add_argument("-f", "--filename", default="openwrt.bin", help="Initramfs filename on TFTP server (default: openwrt.bin)")
args = parser.parse_args()

if not os.path.exists(args.port):
    print(f"[-] Error: {args.port} not found! Check UART adapter connection.")
    sys.exit(1)

fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = termios.B57600
attrs[5] = termios.B57600
attrs[0] = attrs[1] = attrs[3] = 0
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
termios.tcsetattr(fd, termios.TCSANOW, attrs)
termios.tcflush(fd, termios.TCIOFLUSH)

print("=" * 65)
print("  Audio Pro Addon C3 (Linkplay A28) - U-Boot SDRAM RAM Boot")
print("=" * 65)
print(f"[*] Port: {args.port} @ {args.baud} baud")
print(f"[*] Network: Device {args.device_ip} -> Server {args.server_ip} (Image: {args.filename})")
print("[*] Connect UART probes and power cycle the device.")
print("=" * 65 + "\n")

state = "WAIT_MENU"
t0 = time.time()
buf_accum = ""

while time.time() - t0 < 300:
    r, _, _ = select.select([fd], [], [], 0.02)
    if fd in r:
        try:
            raw = os.read(fd, 2048)
            if raw:
                chunk = raw.decode("utf-8", errors="replace")
                print(chunk, end="", flush=True)
                buf_accum += chunk

                if state == "WAIT_MENU":
                    if any(kw in buf_accum for kw in ["Please choose", "check rootfs finished", "Load system code to SDRAM", "choose the operation"]):
                        print("\n[+] U-Boot menu detected! Selecting option 5 (SDRAM TFTP)...")
                        for _ in range(3):
                            os.write(fd, b"5\n")
                            time.sleep(0.05)
                        buf_accum = ""
                        state = "WAIT_DEV_IP"

                elif state in ("WAIT_MENU", "WAIT_DEV_IP"):
                    if any(kw in buf_accum for kw in ["device IP", "Input device IP", "ipaddr"]):
                        print(f"\n[+] Setting Device IP: {args.device_ip}...")
                        dev_cmd = f"{args.device_ip}\n".encode()
                        os.write(fd, dev_cmd)
                        time.sleep(0.05)
                        os.write(fd, dev_cmd)
                        buf_accum = ""
                        state = "WAIT_SRV_IP"

                elif state in ("WAIT_DEV_IP", "WAIT_SRV_IP"):
                    if any(kw in buf_accum for kw in ["server IP", "Input server IP", "serverip"]):
                        print(f"\n[+] Setting Server IP: {args.server_ip}...")
                        srv_cmd = f"{args.server_ip}\n".encode()
                        os.write(fd, srv_cmd)
                        time.sleep(0.05)
                        os.write(fd, srv_cmd)
                        buf_accum = ""
                        state = "WAIT_FILENAME"

                elif state in ("WAIT_SRV_IP", "WAIT_FILENAME"):
                    if any(kw in buf_accum for kw in ["filename", "Kernel filename"]):
                        print(f"\n[+] Setting filename: {args.filename}...")
                        file_cmd = f"{args.filename}\n".encode()
                        os.write(fd, file_cmd)
                        print("\n[*] Loading initramfs image into SDRAM via TFTP...")
                        buf_accum = ""
                        state = "DOWNLOADING"

                elif state == "DOWNLOADING":
                    if any(kw in buf_accum for kw in ["Starting kernel", "Linux version", "OpenWrt", "procd:", "snd-soc-dummy"]):
                        print("\n\n[+] OpenWrt successfully booted into RAM!")
                        print("[+] SSH is ready on 192.168.1.1.")
                        state = "BOOTED"
                        break
        except Exception:
            pass

os.close(fd)
if state == "BOOTED":
    print("\n[*] RAM boot sequence completed successfully.")
else:
    print("\n[-] Timeout waiting for U-Boot.")
    sys.exit(1)
