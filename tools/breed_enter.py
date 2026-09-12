#!/usr/bin/env python3
"""
breed_enter.py - Autonomous Hackpascal Breed Bootloader Interceptor & Web Recovery Tool.

Listens for and intercepts the Breed bootloader on MediaTek MT7628 / MT7621 / RT5350 SoCs
during power-on by transmitting BREED:ABORT UDP packets and awaiting BREED:ABORTED response.
Automatically configures host network interface IP (192.168.1.2/24), verifies HTTP recovery
console availability (http://192.168.1.1), and supports unattended sysupgrade flashing.
"""

import os
import sys
import time
import socket
import select
import urllib.request
import urllib.parse
import urllib.error
import argparse
import subprocess
import uuid

# Hackpascal Breed protocol constants
DEFAULT_BREED_IP = "192.168.1.1"
DEFAULT_HOST_IP = "192.168.1.2"
DEFAULT_SUBNET_MASK = 24
BREED_REPLY_PORT = 37540       # Host listen port (0x92a4)
BREED_TARGET_PORT = 37541      # Breed target port (0x92a5)
LEGACY_BREED_PORT = 50000      # Fallback port for alternative U-Boot builds
MAGIC_ABORT_PACKET = b"BREED:ABORT"
MAGIC_REPLY_PACKET = b"BREED:ABORTED"
BROADCAST_IP = "255.255.255.255"


def check_root():
    """Ensure script is running with root privileges for raw interface binding."""
    if os.geteuid() != 0:
        print("[-] Error: Root privileges required for SO_BINDTODEVICE and interface configuration.")
        print("    Please run with 'pkexec' or 'sudo'.")
        sys.exit(1)


def get_interface_ips(iface):
    """Return list of IPv4 addresses currently assigned to the given interface."""
    try:
        out = subprocess.check_output(
            ["ip", "-4", "-o", "addr", "show", "dev", iface],
            stderr=subprocess.DEVNULL,
            text=True
        )
        ips = []
        for line in out.splitlines():
            parts = line.strip().split()
            if len(parts) >= 4:
                ips.append(parts[3])  # IP/prefix
        return ips
    except Exception:
        return []


def configure_interface(iface, host_ip=DEFAULT_HOST_IP, mask=DEFAULT_SUBNET_MASK):
    """Ensure the network interface is UP and has an IP in the 192.168.1.0/24 subnet."""
    print(f"[*] Checking interface {iface} configuration...")
    try:
        # Bring link UP
        subprocess.run(["ip", "link", "set", "dev", iface, "up"], check=True)
    except subprocess.CalledProcessError as err:
        print(f"[-] Failed to bring up interface {iface}: {err}")
        sys.exit(1)

    existing_ips = get_interface_ips(iface)
    has_target_subnet = any(ip.startswith("192.168.1.") for ip in existing_ips)

    if not has_target_subnet:
        cidr = f"{host_ip}/{mask}"
        print(f"[*] Adding {cidr} to interface {iface}...")
        try:
            subprocess.run(["ip", "addr", "add", cidr, "dev", iface], check=True)
            print(f"[+] Successfully configured {cidr} on {iface}")
        except subprocess.CalledProcessError as err:
            print(f"[-] Failed to add IP address to {iface}: {err}")
            sys.exit(1)
    else:
        print(f"[+] Interface {iface} already has an address in 192.168.1.0/24 ({existing_ips})")


def build_multipart_form(fields, files):
    """Generate multipart/form-data payload using standard library only."""
    boundary = f"----BreedBoundary{uuid.uuid4().hex}"
    body = bytearray()

    for name, value in fields.items():
        body.extend(f"--{boundary}\r\n".encode("utf-8"))
        body.extend(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode("utf-8"))
        body.extend(str(value).encode("utf-8"))
        body.extend(b"\r\n")

    for name, (filename, content, content_type) in files.items():
        body.extend(f"--{boundary}\r\n".encode("utf-8"))
        body.extend(f'Content-Disposition: form-data; name="{name}"; filename="{filename}"\r\n'.encode("utf-8"))
        body.extend(f"Content-Type: {content_type}\r\n\r\n".encode("utf-8"))
        body.extend(content)
        body.extend(b"\r\n")

    body.extend(f"--{boundary}--\r\n".encode("utf-8"))
    content_type_header = f"multipart/form-data; boundary={boundary}"
    return bytes(body), content_type_header


def intercept_breed(iface, target_ip, timeout=60, verbose=True):
    """
    Transmit BREED:ABORT packets to UDP port 37541 and listen for BREED:ABORTED on port 37540.
    Returns True when bootloader acknowledges abort or recovery HTTP service comes online.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    # Bind to physical interface to prevent leaking packets to default gateway
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode("utf-8"))
    except OSError as err:
        print(f"[!] Warning: SO_BINDTODEVICE on {iface} failed ({err}). Proceeding with standard routing.")

    try:
        sock.bind(("0.0.0.0", BREED_REPLY_PORT))
    except OSError as err:
        print(f"[-] Could not bind to port {BREED_REPLY_PORT}: {err}")
        print("    Ensure no other instance of breed_enter is running.")
        sys.exit(1)

    sock.setblocking(False)

    print("=" * 68)
    print("  Hackpascal Breed Bootloader Interceptor (MT7628 / MT7621 / RT5350)")
    print("=" * 68)
    print(f"[*] Network Interface   : {iface}")
    print(f"[*] Breed Target Address: {target_ip}:{BREED_TARGET_PORT}")
    print(f"[*] Reply Listener Port : 0.0.0.0:{BREED_REPLY_PORT}")
    print(f"[*] Abort Magic String  : {MAGIC_ABORT_PACKET.decode('ascii')}")
    print(f"[*] Waiting Timeout     : {timeout} seconds")
    print("[*] Power-cycle or reboot the device NOW.")
    print("=" * 68 + "\n")

    start_time = time.time()
    packet_count = 0
    last_print = 0.0
    aborted = False

    while time.time() - start_time < timeout:
        current_time = time.time()

        # Send BREED:ABORT to broadcast and unicast target
        try:
            sock.sendto(MAGIC_ABORT_PACKET, (BROADCAST_IP, BREED_TARGET_PORT))
            sock.sendto(MAGIC_ABORT_PACKET, (target_ip, BREED_TARGET_PORT))
            # Auxiliary burst to port 50000 for non-standard forks
            sock.sendto(b"\x00" * 32, (BROADCAST_IP, LEGACY_BREED_PORT))
            packet_count += 3
        except OSError:
            pass

        # Check for incoming reply
        readable, _, _ = select.select([sock], [], [], 0.005)
        if sock in readable:
            try:
                data, addr = sock.recvfrom(1024)
                if MAGIC_REPLY_PACKET in data:
                    print(f"\n[+] SUCCESS: Received '{data.decode('latin-1').strip()}' from {addr[0]}:{addr[1]}!")
                    print("[+] Breed autoboot sequence successfully halted!")
                    aborted = True
                    break
            except Exception:
                pass

        if current_time - last_print > 1.0:
            elapsed = int(current_time - start_time)
            print(f"\r[*] Transmitting abort packets... {elapsed}s elapsed ({packet_count} packets sent)", end="", flush=True)
            last_print = current_time

    sock.close()
    print()

    if not aborted:
        print("[*] Intercept window expired without explicit UDP ACK.")
        print("[*] Verifying whether Breed Web Recovery server is nevertheless active...")

    return aborted


def wait_for_breed_web(target_ip, timeout=15):
    """Poll Breed HTTP web recovery server until HTTP 200 OK is returned."""
    url = f"http://{target_ip}/"
    print(f"[*] Polling Breed Web Recovery Console at {url}...")
    start_time = time.time()

    headers = {"User-Agent": "BreedEnter/1.0"}
    req = urllib.request.Request(url, headers=headers)

    while time.time() - start_time < timeout:
        try:
            with urllib.request.urlopen(req, timeout=1.5) as response:
                if response.status == 200:
                    html = response.read().decode("utf-8", errors="replace")
                    title = "Breed Web Recovery"
                    if "<title>" in html:
                        title = html.split("<title>")[1].split("</title>")[0].strip()
                    print(f"[+] Breed Web Recovery is ONLINE! HTTP 200 OK ({title})")
                    print(f"[+] Web interface accessible at: http://{target_ip}/")
                    return True
        except (urllib.error.URLError, OSError, TimeoutError):
            pass
        time.sleep(0.5)

    print(f"[-] Breed Web Console did not respond within {timeout}s.")
    return False


def set_breed_bootdelay(target_ip, delay=2):
    """Set bootloader autoboot delay in Breed Web Console via /env.html."""
    url = f"http://{target_ip}/env.html"
    print(f"[*] Configuring Breed bootloader autoboot delay to {delay}s...")
    for var in ["bootdelay", "autoboot_delay"]:
        try:
            data = urllib.parse.urlencode({"name": var, "value": str(delay)}).encode("utf-8")
            req = urllib.request.Request(url, data=data)
            with urllib.request.urlopen(req, timeout=3) as resp:
                if resp.status == 200:
                    print(f"[+] Successfully configured Breed variable '{var}={delay}'")
        except Exception as err:
            print(f"[!] Note: Could not set '{var}' via /env.html: {err}")


def flash_firmware(target_ip, fw_path, layout="reference", autoreboot=True):
    """
    Perform automated sysupgrade via Breed's /upload.html and /upgrading.html endpoints.
    Tracks flash erase & write progress via /upgrade_query.html.
    """
    if not os.path.isfile(fw_path):
        print(f"[-] Firmware file not found: {fw_path}")
        return False

    fw_size = os.path.getsize(fw_path)
    fw_name = os.path.basename(fw_path)
    print(f"\n[*] Preparing to flash firmware: {fw_name} ({fw_size} bytes)")
    print(f"[*] Target Flash Layout: {layout}")
    print(f"[*] Auto-reboot: {'Yes' if autoreboot else 'No'}")

    with open(fw_path, "rb") as f:
        fw_bytes = f.read()

    upload_url = f"http://{target_ip}/upload.html"
    fields = {
        "fw_check": "1",
        "flash_layout": layout,
        "fw_type": "generic",
        "autoreboot": "1" if autoreboot else "0",
        "submit": "Upload"
    }
    files = {
        "fw_file": (fw_name, fw_bytes, "application/octet-stream")
    }

    print("[*] Uploading firmware image to Breed...")
    payload, ctype = build_multipart_form(fields, files)
    req = urllib.request.Request(upload_url, data=payload, headers={"Content-Type": ctype})

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            resp_html = resp.read().decode("utf-8", errors="replace")
            if "upgrading.html" not in resp_html and resp.status != 200:
                print(f"[-] Unexpected response from upload endpoint: HTTP {resp.status}")
                return False
    except Exception as err:
        print(f"[-] Failed during firmware upload: {err}")
        return False

    print("[+] Firmware successfully staged into Breed RAM buffer.")

    # Trigger flash write sequence
    start_url = f"http://{target_ip}/upgrading.html"
    print("[*] Triggering flash write process via /upgrading.html...")
    try:
        urllib.request.urlopen(start_url, timeout=5)
    except Exception:
        pass  # Breed may immediately start blocking or redirecting

    # Monitor progress via /upgrade_query.html
    query_url = f"http://{target_ip}/upgrade_query.html"
    print("[*] Monitoring flash write progress...")

    last_prog = -1
    consecutive_failures = 0
    t0 = time.time()

    while time.time() - t0 < 180:
        time.sleep(1.0)
        try:
            with urllib.request.urlopen(query_url, timeout=2.0) as qresp:
                raw_status = qresp.read().decode("ascii", errors="replace").strip()
                consecutive_failures = 0

                if raw_status.isdigit():
                    prog = int(raw_status)
                    if prog != last_prog:
                        bar = "=" * (prog // 5) + " " * (20 - (prog // 5))
                        print(f"\r[*] Flashing progress: [{bar}] {prog}%", end="", flush=True)
                        last_prog = prog

                    if prog >= 100:
                        print(f"\n[+] Flashing complete! (100%)")
                        if autoreboot:
                            print("[+] Device is rebooting into the new firmware...")
                        return True
                elif raw_status == "SE":
                    print("\n[-] Breed reported internal flash write error (SE)!")
                    return False
                elif raw_status == "NP":
                    if last_prog >= 90:
                        print("\n[+] Flashing operation concluded. Device rebooting...")
                        return True
        except (urllib.error.URLError, OSError):
            consecutive_failures += 1
            # If we reached high progress and connection dropped, the device has rebooted
            if last_prog >= 80 and consecutive_failures >= 3:
                print(f"\n[+] Connection dropped at {last_prog}%: Device successfully rebooted into new firmware!")
                return True
            if consecutive_failures > 20:
                print("\n[-] Lost connection to Breed before flash completion.")
                return False

    print("\n[-] Flash monitoring timed out after 180 seconds.")
    return False


def main():
    parser = argparse.ArgumentParser(
        description="Autonomous Breed Bootloader Interceptor & Flasher for Linux (MT7628 / Audio Pro C3)"
    )
    parser.add_argument(
        "-i", "--interface",
        default="eth0",
        help="Network interface connected to device (default: eth0)"
    )
    parser.add_argument(
        "-t", "--target-ip",
        default=DEFAULT_BREED_IP,
        help=f"Breed bootloader recovery IP (default: {DEFAULT_BREED_IP})"
    )
    parser.add_argument(
        "-H", "--host-ip",
        default=DEFAULT_HOST_IP,
        help=f"Host IP to assign if interface is not configured (default: {DEFAULT_HOST_IP})"
    )
    parser.add_argument(
        "-T", "--timeout",
        type=int,
        default=60,
        help="Interception listen window in seconds (default: 60)"
    )
    parser.add_argument(
        "-f", "--flash",
        help="Path to sysupgrade.bin / firmware image to automatically flash"
    )
    parser.add_argument(
        "--layout",
        default="reference",
        choices=["reference", "compact", "phicomm", "big", "wndr3700v5"],
        help="Flash partition layout for Breed (default: reference / 0x50000 OpenWrt)"
    )
    parser.add_argument(
        "--no-reboot",
        action="store_true",
        help="Do not reboot automatically after flashing"
    )
    parser.add_argument(
        "--bootdelay",
        type=int,
        default=2,
        help="Configure Breed bootloader autoboot delay in seconds (default: 2)"
    )

    args = parser.parse_args()

    check_root()
    configure_interface(args.interface, host_ip=args.host_ip)

    intercepted = intercept_breed(
        iface=args.interface,
        target_ip=args.target_ip,
        timeout=args.timeout
    )

    web_alive = wait_for_breed_web(args.target_ip, timeout=20)
    if not web_alive:
        print("[-] Breed web interface is not responding. Check physical cable link and power.")
        sys.exit(1)

    print("\n[+] Breed recovery mode is active and ready.")

    if args.bootdelay is not None:
        set_breed_bootdelay(args.target_ip, args.bootdelay)

    if args.flash:
        success = flash_firmware(
            target_ip=args.target_ip,
            fw_path=args.flash,
            layout=args.layout,
            autoreboot=not args.no_reboot
        )
        if not success:
            sys.exit(1)

    print("[+] Operation completed successfully.")


if __name__ == "__main__":
    main()
