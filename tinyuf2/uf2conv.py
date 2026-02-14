#!/usr/bin/env python3
import argparse
import math
import os
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000

PAYLOAD_SIZE = 256
BLOCK_SIZE = 512


def parse_args():
    p = argparse.ArgumentParser(
        description="Convert .bin to .uf2 (minimal uf2conv replacement)")
    p.add_argument("-c", action="store_true", help="convert to UF2")
    p.add_argument("-b", "--base", default="0x0", help="base address")
    p.add_argument("-f", "--family", default=None, help="UF2 family ID (hex)")
    p.add_argument("-o", "--output", default=None, help="output UF2 file")
    p.add_argument("input", nargs=1, help="input .bin file")
    return p.parse_args()


def to_int(v):
    if v is None:
        return None
    return int(v, 0)


def build_uf2(bin_data, base_addr, family_id):
    num_blocks = int(math.ceil(len(bin_data) / float(PAYLOAD_SIZE)))
    out = bytearray()

    flags = 0
    if family_id is not None:
        flags |= UF2_FLAG_FAMILY_ID

    for block_no in range(num_blocks):
        chunk = bin_data[block_no * PAYLOAD_SIZE:(block_no + 1) * PAYLOAD_SIZE]
        if len(chunk) < PAYLOAD_SIZE:
            chunk += b"\x00" * (PAYLOAD_SIZE - len(chunk))

        target_addr = base_addr + block_no * PAYLOAD_SIZE
        header_word7 = family_id if family_id is not None else len(bin_data)

        block = bytearray(BLOCK_SIZE)
        struct.pack_into(
            "<IIIIIIII",
            block,
            0,
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            flags,
            target_addr,
            PAYLOAD_SIZE,
            block_no,
            num_blocks,
            header_word7,
        )
        block[32:32 + PAYLOAD_SIZE] = chunk
        struct.pack_into("<I", block, BLOCK_SIZE - 4, UF2_MAGIC_END)
        out += block

    return out


def main():
    args = parse_args()
    in_path = args.input[0]
    out_path = args.output or os.path.splitext(in_path)[0] + ".uf2"

    base_addr = to_int(args.base)
    family_id = to_int(args.family)

    if base_addr is None:
        base_addr = 0

    with open(in_path, "rb") as f:
        bin_data = f.read()

    uf2 = build_uf2(bin_data, base_addr, family_id)
    with open(out_path, "wb") as f:
        f.write(uf2)

    print("Wrote:", out_path)


if __name__ == "__main__":
    main()

