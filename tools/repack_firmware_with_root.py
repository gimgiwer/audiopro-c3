#!/usr/bin/env python3
"""
Repacks Linkplay A28 / Audio Pro C3 stock firmware with Root (telnetd)
and dynamically generates a valid dual-uImage header for U-Boot CRC validation.
"""
import os
import sys
import time
import struct
import zlib
import subprocess

def find_kernel_boundary(data):
    """
    Dynamically locates the boundary between kernel and rootfs uImage.
    1. Looks for secondary uImage header with Filesystem/Rootfs type.
    2. Fallback: Looks for SquashFS signature ('hsqs').
    3. Fallback: Checks standard Linkplay flash offsets.
    """
    # 1. Search for secondary uImage header
    pos = 64
    while True:
        idx = data.find(b"\x27\x05\x19\x56", pos)
        if idx == -1:
            break
        hdr = data[idx:idx+64]
        if len(hdr) == 64:
            magic, _, _, size, _, _, _, _, _, typ, _ = struct.unpack(">IIIIIIIBBBB", hdr[:32])
            name = hdr[32:64].rstrip(b"\x00")
            if typ == 7 or b"Rootfs" in name or b"rootfs" in name:
                print(f"[+] Found secondary RootFS uImage header at offset 0x{idx:x}")
                return idx
        pos = idx + 1

    # 2. Fallback: Search for SquashFS magic
    sq_idx = data.find(b"hsqs")
    if sq_idx != -1:
        print(f"[+] Found SquashFS magic 'hsqs' at offset 0x{sq_idx:x}")
        return sq_idx

    # 3. Fallback: Standard Linkplay MT7628 kernel size heuristic (~1.7 MB)
    kernel_guess = 0x1B72C2
    if kernel_guess < len(data):
        print(f"[!] Using standard fallback kernel boundary: 0x{kernel_guess:x}")
        return kernel_guess

    raise ValueError("Could not find rootfs boundary in source firmware binary")

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <stock_uimage.bin> <squashfs_root_dir> <output_uimage.bin>")
        sys.exit(1)

    stock_bin = sys.argv[1]
    sq_dir = sys.argv[2]
    out_bin = sys.argv[3]

    if not os.path.exists(stock_bin):
        print(f"[-] Error: input firmware '{stock_bin}' not found")
        sys.exit(1)

    if not os.path.isdir(sq_dir):
        print(f"[-] Error: squashfs directory '{sq_dir}' not found")
        sys.exit(1)

    print("[*] Reading source firmware and locating kernel boundary...")
    with open(stock_bin, "rb") as f:
        orig = f.read()

    kernel_boundary = find_kernel_boundary(orig)
    print(f"[+] Kernel partition size: {kernel_boundary} bytes (0x{kernel_boundary:x})")
    kernel_part = orig[:kernel_boundary]

    print("[*] Repacking SquashFS with XZ compression (512KB block size)...")
    sq_tmp = "/tmp/new_rootfs.sqsh"
    if os.path.exists(sq_tmp):
        os.remove(sq_tmp)

    subprocess.run([
        "mksquashfs", sq_dir, sq_tmp,
        "-comp", "xz",
        "-b", "524288",
        "-noappend",
        "-nopad",
        "-no-progress"
    ], check=True)

    with open(sq_tmp, "rb") as f:
        sq_data = f.read()

    sq_size = len(sq_data)
    sq_dcrc = zlib.crc32(sq_data) & 0xffffffff
    sq_time = int(time.time())

    IH_MAGIC = 0x27051956
    IH_NAME  = b"Wiimu Rootfs".ljust(32, b"\x00")

    # Generate uImage filesystem header
    hdr_raw = struct.pack(">IIIIIIIBBBB32s", IH_MAGIC, 0, sq_time, sq_size, 0, 0, sq_dcrc, 6, 5, 7, 1, IH_NAME)
    sq_hcrc = zlib.crc32(hdr_raw) & 0xffffffff
    sq_hdr  = struct.pack(">IIIIIIIBBBB32s", IH_MAGIC, sq_hcrc, sq_time, sq_size, 0, 0, sq_dcrc, 6, 5, 7, 1, IH_NAME)

    output = kernel_part + sq_hdr + sq_data
    with open(out_bin, "wb") as f:
        f.write(output)

    print(f"[+] Generated rooted uImage firmware: {out_bin} ({len(output)} bytes)")
    print(f"    - RootFS Size: {sq_size} bytes")
    print(f"    - RootFS CRC32: 0x{sq_dcrc:08x}")
    print(f"    - Header CRC32: 0x{sq_hcrc:08x}")

if __name__ == "__main__":
    main()
