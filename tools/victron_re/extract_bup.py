#!/usr/bin/env python3

import argparse
import base64
import pathlib
import struct
import xml.etree.ElementTree as ET


BLE_BUP_XOR_KEY = bytes.fromhex(
    "f6ecd9b367cf9e3d7af4e8d1a3478e1d"
    "3b76eddbb66ddab56ad5ab57af5ebd7b"
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract firmware payloads from a Victron BUP file")
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument(
        "--decode-ble",
        action="store_true",
        help="remove the repeating XOR mask used by Victron BLE firmware packages",
    )
    args = parser.parse_args()

    root = ET.parse(args.input).getroot()
    chunks = [node.text or "" for node in root.findall("./block/firmware")]
    if not chunks:
        raise SystemExit("no firmware blocks found")

    payload = b"".join(base64.b64decode(chunk, validate=True) for chunk in chunks)
    if args.decode_ble:
        payload = bytes(
            value ^ BLE_BUP_XOR_KEY[index % len(BLE_BUP_XOR_KEY)]
            for index, value in enumerate(payload)
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    summary = (
        f"{args.input.name}: product={root.findtext('productId')} "
        f"version={root.findtext('appVersion')} chunks={len(chunks)} bytes={len(payload)} "
        f"format={'decoded-ble' if args.decode_ble else 'transport'}"
    )
    if args.decode_ble and len(payload) >= 8:
        stack_pointer, reset_handler = struct.unpack_from("<II", payload)
        summary += f" stack=0x{stack_pointer:08x} reset=0x{reset_handler:08x}"
    print(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
