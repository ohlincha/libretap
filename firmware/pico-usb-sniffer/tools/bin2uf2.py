#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Written for LibreTap (not part of usb-sniffer-lite). Turns the linked RP2040 binary into a UF2 file for the Pico's
# BOOTSEL drive, like ataradov's bin2uf2 tool (github.com/ataradov/tools):
#  - the first 256 bytes are the second-stage boot loader; the boot ROM only runs it when its last 4 bytes hold the
#    CRC-32 of the first 252 (polynomial 0x04c11db7, initial value 0xffffffff, not reflected, no final XOR), so that
#    CRC is filled in here (the linker script leaves 0xcccccccc there);
#  - the image is cut into 256-byte UF2 blocks for flash address 0x10000000, family ID RP2040.
#
# usage: bin2uf2.py -i firmware.bin -o firmware.uf2
import argparse
import struct
import sys

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
RP2040_FAMILY = 0xE48BFF56
FLASH_BASE = 0x10000000


def crc32_boot2(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc


def convert(image: bytes) -> bytes:
    if len(image) < 256:
        sys.exit("bin2uf2: the image is shorter than the 256-byte boot sector")
    image = bytearray(image)
    image[252:256] = struct.pack("<I", crc32_boot2(bytes(image[:252])))
    if len(image) % 256:
        image += bytes(256 - len(image) % 256)
    n = len(image) // 256
    out = bytearray()
    for i in range(n):
        payload = bytes(image[i * 256:(i + 1) * 256]) + bytes(476 - 256)
        out += struct.pack("<IIIIIIII", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, FLASH_BASE + i * 256, 256, i, n,
                           RP2040_FAMILY)
        out += payload
        out += struct.pack("<I", UF2_MAGIC_END)
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser(description="RP2040 binary to UF2 (with the boot2 CRC)")
    ap.add_argument("-i", required=True, help="input .bin")
    ap.add_argument("-o", required=True, help="output .uf2")
    a = ap.parse_args()
    with open(a.i, "rb") as f:
        data = f.read()
    with open(a.o, "wb") as f:
        f.write(convert(data))


if __name__ == "__main__":
    main()
