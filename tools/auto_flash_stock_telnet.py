#!/usr/bin/env python3
import os
import sys
import time
import termios
import select
import subprocess
import socket
import hashlib
import argparse

parser = argparse.ArgumentParser(description="Audio Pro Addon C3 - Automated Firmware Flash & Root Installer")
parser.add_argument("-i", "--image", default="firmware_telnet_valid_crc.bin", help="Target firmware image to flash")
parser.add_argument("-p", "--port", default=os.getenv("UART_PORT", "/dev/ttyUSB0"), help="Serial port (default: /dev/ttyUSB0)")
parser.add_argument("-t", "--target-ip", default="192.168.1.1", help="OpenWrt target IP (default: 192.168.1.1)")
parser.add_argument("-s", "--http-server", default="192.168.1.202:8888", help="Host HTTP server serving firmware image (default: 192.168.1.202:8888)")
parser.add_argument("-d", "--device-ip", default="192.168.1.122", help="Device IP for TFTP (default: 192.168.1.122)")
parser.add_argument("-T", "--tftp-server", default="192.168.1.202", help="TFTP server IP (default: 192.168.1.202)")
parser.add_argument("--timeout", type=int, default=300, help="U-Boot wait timeout in seconds (default: 300)")
args = parser.parse_args()

IMAGE_PATH = args.image
if not os.path.exists(IMAGE_PATH):
    alt = os.path.join(os.path.dirname(__file__), "..", "..", IMAGE_PATH)
    if os.path.exists(alt):
        IMAGE_PATH = alt
    else:
        print(f"[-] Error: firmware image '{IMAGE_PATH}' not found!")
        sys.exit(1)

def get_file_info(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return os.path.getsize(path), h.hexdigest()

EXPECTED_SIZE, EXPECTED_SHA256 = get_file_info(IMAGE_PATH)

if not os.path.exists(args.port):
    print(f"[-] Error: {args.port} not found! Please connect UART adapter.")
    sys.exit(1)

fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = termios.B57600
attrs[5] = termios.B57600
attrs[0] = attrs[1] = attrs[3] = 0
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
termios.tcsetattr(fd, termios.TCSANOW, attrs)
termios.tcflush(fd, termios.TCIOFLUSH)

print("=" * 70)
print("  Audio Pro Addon C3 - Automated Firmware Flash & Root Installer")
print("=" * 70)
print(f"[*] Target image: {os.path.basename(IMAGE_PATH)}")
print(f"[*] Size: {EXPECTED_SIZE} bytes, SHA-256: {EXPECTED_MD5[:16]}...")
print(f"[*] Port: {args.port} | Target: {args.target_ip} | HTTP Host: {args.http_server}")
print("[*] 1. Intercepts U-Boot and loads OpenWrt into SDRAM (RAM boot)")
print("[*] 2. Downloads and flashes image to SPI Flash via mtd write")
print("[*] 3. Performs byte-level SHA-256 readback verification")
print("[*] 4. Reboots into factory firmware with root telnet server enabled")
print("=" * 70)
print("[*] Connect UART probes and power cycle the speaker...")
print("=" * 70 + "\n")

state = "WAIT_MENU"
t0 = time.time()
buf_accum = ""

while time.time() - t0 < args.timeout:
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
                        print("\n[+] U-Boot menu detected! Sending option 5 (SDRAM TFTP)...")
                        time.sleep(0.08)
                        os.write(fd, b"5\n")
                        time.sleep(0.08)
                        os.write(fd, b"5\n")
                        buf_accum = ""
                        state = "WAIT_DEV_IP"

                elif state in ("WAIT_MENU", "WAIT_DEV_IP"):
                    if any(kw in buf_accum for kw in ["device IP", "Input device IP", "ipaddr"]):
                        print(f"\n[+] Setting Device IP: {args.device_ip}...")
                        dev_cmd = f"{args.device_ip}\n".encode()
                        time.sleep(0.08)
                        os.write(fd, dev_cmd)
                        time.sleep(0.05)
                        os.write(fd, dev_cmd)
                        buf_accum = ""
                        state = "WAIT_SRV_IP"

                elif state in ("WAIT_DEV_IP", "WAIT_SRV_IP"):
                    if any(kw in buf_accum for kw in ["server IP", "Input server IP", "serverip"]):
                        print(f"\n[+] Setting Server IP: {args.tftp_server}...")
                        srv_cmd = f"{args.tftp_server}\n".encode()
                        time.sleep(0.08)
                        os.write(fd, srv_cmd)
                        time.sleep(0.05)
                        os.write(fd, srv_cmd)
                        buf_accum = ""
                        state = "WAIT_FILENAME"

                elif state in ("WAIT_SRV_IP", "WAIT_FILENAME"):
                    if any(kw in buf_accum for kw in ["filename", "Kernel filename"]):
                        print("\n[+] Setting filename: openwrt.bin...")
                        time.sleep(0.08)
                        os.write(fd, b"openwrt.bin\n")
                        print("\n[*] Loading initramfs image into SDRAM via TFTP...")
                        buf_accum = ""
                        state = "DOWNLOADING"

                elif state == "DOWNLOADING":
                    if any(kw in buf_accum for kw in ["Starting kernel", "Linux version", "OpenWrt", "procd:", "snd-soc-dummy"]):
                        print("\n\n[+] OpenWrt booted into RAM successfully!")
                        state = "BOOTED"
                        break
        except Exception:
            pass

os.close(fd)

if state != "BOOTED":
    print("\n[-] Timeout waiting for U-Boot.")
    sys.exit(1)

print(f"\n[*] Waiting for OpenWrt SSH server at {args.target_ip}:22...")
ssh_ready = False
for attempt in range(45):
    try:
        s = socket.create_connection((args.target_ip, 22), timeout=1.5)
        s.close()
        ssh_ready = True
        print(f"\n[+] SSH server ready (attempt {attempt+1})")
        break
    except Exception:
        print(".", end="", flush=True)
        time.sleep(1)

if not ssh_ready:
    print(f"\n[-] Error: OpenWrt SSH did not become ready at {args.target_ip}!")
    sys.exit(1)

time.sleep(2)

print("\n" + "=" * 70)
print("  Downloading, Flashing and Verifying SPI Flash via OpenWrt")
print("=" * 70)

image_name = os.path.basename(IMAGE_PATH)
cmd = f"""
set -e
echo '[1/5] Downloading {image_name} from host...'
wget -q -O /tmp/fw.bin http://{args.http_server}/{image_name}

echo '[2/5] Verifying image size and SHA-256 before write...'
SIZE=$(wc -c < /tmp/fw.bin)
if [ "$SIZE" != "{EXPECTED_SIZE}" ]; then
    echo "SIZE MISMATCH: $SIZE != {EXPECTED_SIZE}"
    exit 1
fi

SHA256=$(sha256sum /tmp/fw.bin | awk '{{print $1}}')
if [ "$SHA256" != "{EXPECTED_SHA256}" ]; then
    echo "SHA-256 MISMATCH: $SHA256 != {EXPECTED_SHA256}"
    exit 1
fi
echo "Size: $SIZE bytes OK, SHA-256: $SHA256 OK"

echo '[3/5] Writing to firmware partition via mtd write...'
mtd write /tmp/fw.bin firmware

echo '[4/5] Dynamically resolving target MTD partition for readback...'
MTD_DEV=$(grep '"firmware"' /proc/mtd | cut -d: -f1)
if [ -z "$MTD_DEV" ]; then
    MTD_DEV="mtd4"
fi
echo "Reading $SIZE bytes from /dev/$MTD_DEV..."

FLASH_SHA256=$(head -c "$SIZE" "/dev/$MTD_DEV" | sha256sum | awk '{{print $1}}')
if [ "$FLASH_SHA256" != "$SHA256" ]; then
    echo "CRITICAL: Flash readback verification failed ($FLASH_SHA256 != $SHA256)!"
    exit 1
fi
echo "Flash readback verified successfully (SHA-256: $FLASH_SHA256)"

echo '[5/5] Syncing and rebooting...'
sync
rm -f /tmp/fw.bin
echo 'FLASH_COMPLETE_REBOOT'
reboot
"""

proc = subprocess.run([
    "ssh", "-o", "ConnectTimeout=5",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    f"root@{args.target_ip}",
    cmd
], capture_output=True, text=True)

print(proc.stdout)
if proc.stderr:
    print("Stderr:", proc.stderr)

if proc.returncode == 0 or "FLASH_COMPLETE_REBOOT" in proc.stdout:
    print("\n" + "=" * 70)
    print("  [SUCCESS] Rooted stock firmware verified & flashed successfully!")
    print("  - Telnet server is available on port 23 with full PTY support")
    print("  - UART console (ttyS0 / ttyS1) provides a direct root shell")
    print("=" * 70 + "\n")
else:
    print("[-] Flashing failed on device.")
    sys.exit(1)
