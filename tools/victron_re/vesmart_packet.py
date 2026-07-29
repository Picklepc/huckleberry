#!/usr/bin/env python3

import argparse
import struct

from cryptography.hazmat.primitives.ciphers.aead import AESCCM


COMPANY_FIELD = bytes.fromhex("ffe102")
MESSAGE_SEQUENCE = 1
MESSAGE_COMPACT = 2
SMART_BATTERY_SENSE_PRODUCT = 0x0000A3A5
SMART_BATTERY_SENSE_FIRMWARE = 0x000115FF
VREG_PRODUCT_ID = 0x0100
VREG_FIRMWARE_VERSION = 0x0102
VREG_BATTERY_VOLTAGE = 0xED8D
VREG_BATTERY_TEMPERATURE = 0xEDEC
VREG_BATTERY_CURRENT = 0xED8C
SENSOR_PRIORITY = 8
CURRENT_PRIORITY = 12


def parse_integer(value: str) -> int:
    return int(value, 0)


def pack_u48(value: int) -> bytes:
    if not 0 <= value < 1 << 48:
        raise ValueError("sequence must fit in 48 bits")
    return value.to_bytes(6, "little")


def build_nonce(
    message_type: int,
    sequence: int,
    source_address: int,
    network_address: int,
) -> bytes:
    if not 0 <= source_address <= 0xFFFFFFFF:
        raise ValueError("source address must fit in 32 bits")
    if not 0 <= network_address <= 0xFFFF:
        raise ValueError("network address must fit in 16 bits")
    return (
        bytes((message_type,))
        + pack_u48(sequence)
        + struct.pack("<IH", source_address, network_address)
    )


def pack_vreg(vreg: int, value: bytes) -> bytes:
    if not 0 < len(value) <= 0xFF:
        raise ValueError("VREG values must contain 1 to 255 bytes")
    return struct.pack("<HB", vreg, len(value)) + value


def wrap_advertisement(clear_header: bytes, encrypted_payload: bytes) -> bytes:
    body = COMPANY_FIELD + clear_header + encrypted_payload
    if len(body) > 0xFF:
        raise ValueError("advertisement data element is too large")
    return bytes((len(body),)) + body


def build_compact_advertisement(
    key: bytes,
    network_address: int,
    source_address: int,
    sequence: int,
    values: list[tuple[int, bytes]],
    compact_priority: int = SENSOR_PRIORITY,
) -> bytes:
    payload = bytes((compact_priority & 0x0F,)) + b"".join(
        pack_vreg(vreg, value) for vreg, value in values
    )
    if len(payload) > 13:
        raise ValueError("compact plaintext exceeds Victron's 13-byte limit")
    nonce = build_nonce(
        MESSAGE_COMPACT, sequence, source_address, network_address
    )
    encrypted_payload = AESCCM(key, tag_length=4).encrypt(nonce, payload, None)
    clear_header = (
        bytes((MESSAGE_COMPACT, network_address & 0xFF))
        + struct.pack("<I", source_address)
        + pack_u48(sequence)[:4]
    )
    return wrap_advertisement(clear_header, encrypted_payload)


def build_sequence_advertisement(
    key: bytes,
    network_address: int,
    source_address: int,
    sequence: int,
) -> bytes:
    nonce = build_nonce(
        MESSAGE_SEQUENCE, sequence, source_address, network_address
    )
    tag = AESCCM(key, tag_length=4).encrypt(nonce, b"", None)
    clear_header = (
        bytes((MESSAGE_SEQUENCE, network_address & 0xFF))
        + struct.pack("<I", source_address)
        + pack_u48(sequence)
    )
    return wrap_advertisement(clear_header, tag)


def voltage_value(volts: float) -> bytes:
    raw = round(volts * 100)
    if not -0x8000 <= raw < 0x7FFF:
        raise ValueError("battery voltage is outside the signed 0.01 V range")
    return struct.pack("<h", raw)


def temperature_value(celsius: float) -> bytes:
    raw = round(celsius * 100 + 27315)
    if not 0 <= raw < 0xFFFF:
        raise ValueError("battery temperature is outside the unsigned 0.01 K range")
    return struct.pack("<H", raw)


def current_value(amps: float) -> bytes:
    raw = round(amps * 1000)
    if not -0x80000000 <= raw < 0x7FFFFFFF:
        raise ValueError("battery current is outside the signed 0.001 A range")
    return struct.pack("<i", raw)


def build_sensor_cycle(
    key: bytes,
    network_address: int,
    source_address: int,
    sequence: int,
    voltage: float,
    temperature_c: float,
    current_a: float | None = None,
) -> dict[str, bytes]:
    identity = build_compact_advertisement(
        key,
        network_address,
        source_address,
        sequence,
        [
            (VREG_PRODUCT_ID, struct.pack("<I", SMART_BATTERY_SENSE_PRODUCT)),
            (VREG_BATTERY_VOLTAGE, voltage_value(voltage)),
        ],
    )
    status = build_compact_advertisement(
        key,
        network_address,
        source_address,
        sequence + 1,
        [
            (VREG_FIRMWARE_VERSION, struct.pack("<I", SMART_BATTERY_SENSE_FIRMWARE)),
            (VREG_BATTERY_TEMPERATURE, temperature_value(temperature_c)),
        ],
    )
    sequence_sync = build_sequence_advertisement(
        key, network_address, source_address, sequence + 2
    )
    packets = {"identity": identity, "status": status, "sequence": sequence_sync}
    if current_a is not None:
        packets["current"] = build_compact_advertisement(
            key,
            network_address,
            source_address,
            sequence + 3,
            [(VREG_BATTERY_CURRENT, current_value(current_a))],
            compact_priority=CURRENT_PRIORITY,
        )
    return packets


def parse_compact_payload(payload: bytes) -> tuple[int, list[tuple[int, bytes]]]:
    if not payload:
        raise ValueError("compact payload is empty")
    compact_priority = payload[0] & 0x0F
    values: list[tuple[int, bytes]] = []
    offset = 1
    while offset < len(payload):
        if offset + 3 > len(payload):
            raise ValueError("truncated VREG header")
        vreg, length = struct.unpack_from("<HB", payload, offset)
        offset += 3
        if offset + length > len(payload):
            raise ValueError("truncated VREG value")
        values.append((vreg, payload[offset : offset + length]))
        offset += length
    return compact_priority, values


def decrypt_advertisement(
    packet: bytes,
    key: bytes,
    network_address: int,
    expected_sequence: int | None = None,
) -> dict[str, object]:
    if len(packet) < 8 or packet[0] != len(packet) - 1:
        raise ValueError("invalid advertisement data-element length")
    if packet[1:4] != COMPANY_FIELD:
        raise ValueError("not a Victron 0x02E1 manufacturer element")
    message_type = packet[4]
    if packet[5] != network_address & 0xFF:
        raise ValueError("network-address low byte does not match")
    source_address = struct.unpack_from("<I", packet, 6)[0]
    if message_type == MESSAGE_SEQUENCE:
        sequence = int.from_bytes(packet[10:16], "little")
        encrypted_payload = packet[16:]
    elif message_type == MESSAGE_COMPACT:
        if expected_sequence is None:
            raise ValueError("compact messages require the full expected sequence")
        sequence = expected_sequence
        if packet[10:14] != pack_u48(sequence)[:4]:
            raise ValueError("sequence low bytes do not match")
        encrypted_payload = packet[14:]
    else:
        raise ValueError(f"unsupported message type {message_type}")
    nonce = build_nonce(message_type, sequence, source_address, network_address)
    payload = AESCCM(key, tag_length=4).decrypt(nonce, encrypted_payload, None)
    result: dict[str, object] = {
        "message_type": message_type,
        "sequence": sequence,
        "source_address": source_address,
        "payload": payload,
    }
    if message_type == MESSAGE_COMPACT:
        compact_priority, values = parse_compact_payload(payload)
        result["compact_priority"] = compact_priority
        result["values"] = values
    return result


def self_test() -> None:
    key = bytes(range(16))
    network_address = 0x1234
    source_address = 0x89ABCDEF
    sequence = 0x010203040506
    packets = build_sensor_cycle(
        key, network_address, source_address, sequence, 13.27, 24.5, -4.321
    )
    identity = decrypt_advertisement(
        packets["identity"], key, network_address, sequence
    )
    status = decrypt_advertisement(
        packets["status"], key, network_address, sequence + 1
    )
    sequence_sync = decrypt_advertisement(
        packets["sequence"], key, network_address
    )
    current = decrypt_advertisement(
        packets["current"], key, network_address, sequence + 3
    )
    assert identity["values"] == [
        (VREG_PRODUCT_ID, struct.pack("<I", SMART_BATTERY_SENSE_PRODUCT)),
        (VREG_BATTERY_VOLTAGE, voltage_value(13.27)),
    ]
    assert status["values"] == [
        (VREG_FIRMWARE_VERSION, struct.pack("<I", SMART_BATTERY_SENSE_FIRMWARE)),
        (VREG_BATTERY_TEMPERATURE, temperature_value(24.5)),
    ]
    assert sequence_sync["sequence"] == sequence + 2
    assert current["compact_priority"] == CURRENT_PRIORITY
    assert current["values"] == [
        (VREG_BATTERY_CURRENT, current_value(-4.321)),
    ]
    assert all(len(packet) <= 31 for packet in packets.values())
    for name, packet in packets.items():
        print(f"{name}: {packet.hex()}")
    print("self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build offline VE.Smart Smart Battery Sense test packets"
    )
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--key", help="16-byte network key as 32 hexadecimal digits")
    parser.add_argument("--network-address", type=parse_integer)
    parser.add_argument("--source-address", type=parse_integer)
    parser.add_argument("--sequence", type=parse_integer, default=0)
    parser.add_argument("--voltage", type=float)
    parser.add_argument("--temperature-c", type=float)
    parser.add_argument("--current-a", type=float)
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    required = {
        "--key": args.key,
        "--network-address": args.network_address,
        "--source-address": args.source_address,
        "--voltage": args.voltage,
        "--temperature-c": args.temperature_c,
    }
    missing = [name for name, value in required.items() if value is None]
    if missing:
        parser.error(f"missing required arguments: {', '.join(missing)}")
    try:
        key = bytes.fromhex(args.key)
    except ValueError as error:
        parser.error(str(error))
    if len(key) != 16:
        parser.error("--key must contain exactly 16 bytes")
    packets = build_sensor_cycle(
        key,
        args.network_address,
        args.source_address,
        args.sequence,
        args.voltage,
        args.temperature_c,
        args.current_a,
    )
    for name, packet in packets.items():
        print(f"{name}: {packet.hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
