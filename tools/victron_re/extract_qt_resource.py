#!/usr/bin/env python3
import argparse
import pathlib
import struct
import zlib


DIRECTORY = 0x02
COMPRESSED = 0x01


def read_name(names: bytes, offset: int) -> str:
    length = struct.unpack_from(">H", names, offset)[0]
    start = offset + 6
    return names[start : start + length * 2].decode("utf-16-be")


def read_nodes(tree: bytes, names: bytes):
    nodes = []
    for offset in range(0, len(tree), 22):
        name_offset, flags = struct.unpack_from(">IH", tree, offset)
        node = {"name": read_name(names, name_offset), "flags": flags}
        if flags & DIRECTORY:
            node["count"], node["child"] = struct.unpack_from(">II", tree, offset + 6)
        else:
            node["country"], node["language"], node["data"] = struct.unpack_from(">HHI", tree, offset + 6)
        nodes.append(node)
    return nodes


def walk(nodes, index=0, parent=""):
    node = nodes[index]
    path = f"{parent}/{node['name']}" if node["name"] else parent
    if node["flags"] & DIRECTORY:
        for child in range(node["child"], node["child"] + node["count"]):
            yield from walk(nodes, child, path)
    else:
        yield path, node


def extract_data(data: bytes, node) -> bytes:
    offset = node["data"]
    size = struct.unpack_from(">I", data, offset)[0]
    payload = data[offset + 4 : offset + 4 + size]
    if node["flags"] & COMPRESSED:
        expected = struct.unpack_from(">I", payload, 0)[0]
        payload = zlib.decompress(payload[4:])
        if len(payload) != expected:
            raise ValueError(f"decompressed {len(payload)} bytes, expected {expected}")
    return payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tree", type=pathlib.Path)
    parser.add_argument("names", type=pathlib.Path)
    parser.add_argument("data", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    tree = args.tree.read_bytes()
    names = args.names.read_bytes()
    data = args.data.read_bytes()
    nodes = read_nodes(tree, names)
    for path, node in walk(nodes):
        destination = args.output / path.lstrip("/")
        destination.parent.mkdir(parents=True, exist_ok=True)
        payload = extract_data(data, node)
        destination.write_bytes(payload)
        print(f"{path}: {len(payload)} bytes")


if __name__ == "__main__":
    main()
