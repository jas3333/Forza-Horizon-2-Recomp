#!/usr/bin/env python3
"""Extract every Xbox 360 shader container from FH2's files.

Usage:
  extract_shaders.py OUT_DIR INPUT... [--runtime DUMP_DIR]

INPUTs are files or directories (scanned recursively): the game's .fxobj
shader packages and a dump of the decrypted executable image. Each container
(magic 0x102A1100 vertex / 0x102A1101 pixel) is parsed and its microcode
(container + virtualSize + shader.physicalOffset, shader.size bytes) written
to OUT_DIR/ucode/<hash>.<vs|ps>.bin, big-endian as stored on the console.

<hash> is XXH3-64 of the big-endian microcode -- the same key the runtime
computes when the game loads a shader with IM_LOAD, so offline and runtime
shaders line up. OUT_DIR/index.json maps each hash to its type, size and
sources. With --runtime, coverage is checked against a --dump_shaders dump.
"""
import ctypes
import json
import os
import re
import struct
import sys

_xxh = ctypes.CDLL("libxxhash.so")
_xxh.XXH3_64bits.restype = ctypes.c_uint64
_xxh.XXH3_64bits.argtypes = [ctypes.c_char_p, ctypes.c_size_t]


def xxh3(data: bytes) -> int:
    return _xxh.XXH3_64bits(data, len(data))


MAGICS = {b"\x10\x2a\x11\x00": "vs", b"\x10\x2a\x11\x01": "ps"}


def parse_container(data: bytes, off: int):
    """Returns (kind, microcode) or None if this isn't a valid container."""
    if off + 36 > len(data):
        return None
    flags, virtual_size, physical_size, _, _, _, shader_offset = struct.unpack_from(">7I", data, off)
    kind = MAGICS.get(data[off:off + 4])
    if kind is None:
        return None
    if not (0 < virtual_size < 1 << 20 and 0 < physical_size < 1 << 20):
        return None
    if not (0 < shader_offset < virtual_size) or off + virtual_size + physical_size > len(data):
        return None
    sh = off + shader_offset
    if sh + 8 > len(data):
        return None
    physical_offset, size = struct.unpack_from(">2I", data, sh)
    if size == 0 or size % 4 or physical_offset + size > physical_size:
        return None
    start = off + virtual_size + physical_offset
    return kind, data[start:start + size]


def scan(path, found):
    data = open(path, "rb").read()
    for magic in MAGICS:
        i = data.find(magic)
        while i != -1:
            parsed = parse_container(data, i)
            if parsed:
                kind, ucode = parsed
                h = f"{xxh3(ucode):016X}"
                entry = found.setdefault(h, {"type": kind, "size": len(ucode), "sources": []})
                if len(entry["sources"]) < 8:
                    entry["sources"].append(f"{os.path.basename(path)}@0x{i:X}")
                entry["_ucode"] = ucode
            i = data.find(magic, i + 4)


def main():
    args = sys.argv[1:]
    runtime = None
    if "--runtime" in args:
        k = args.index("--runtime")
        runtime = args[k + 1]
        del args[k:k + 2]
    out, inputs = args[0], args[1:]
    found = {}
    for item in inputs:
        if os.path.isdir(item):
            for dp, _, files in os.walk(item):
                for f in files:
                    scan(os.path.join(dp, f), found)
        else:
            scan(item, found)
    os.makedirs(os.path.join(out, "ucode"), exist_ok=True)
    for h, e in found.items():
        with open(os.path.join(out, "ucode", f"{h}.{e['type']}.bin"), "wb") as f:
            f.write(e.pop("_ucode"))
    with open(os.path.join(out, "index.json"), "w") as f:
        json.dump(found, f, indent=1, sort_keys=True)
    vs = sum(1 for e in found.values() if e["type"] == "vs")
    print(f"{len(found)} unique shaders ({vs} vertex, {len(found) - vs} pixel) -> {out}")

    if runtime:
        rt = {}
        for name in os.listdir(runtime):
            m = re.match(r"shader_([0-9A-F]{16})\.ucode\.bin\.(vert|frag)$", name)
            if m:
                rt[m.group(1)] = "vs" if m.group(2) == "vert" else "ps"
        hit = [h for h in rt if h in found]
        miss = sorted(h for h in rt if h not in found)
        print(f"runtime shaders: {len(rt)}, found offline: {len(hit)}, missing: {len(miss)}")
        for h in miss[:10]:
            print("  missing", h, rt[h])


if __name__ == "__main__":
    main()
