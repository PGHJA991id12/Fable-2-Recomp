#!/usr/bin/env python3
"""Fable 2 .bnk (script bank) reader / single-entry rewriter.

Format (big-endian), discovered from the file layout + the modding
community's CompileCompressReplace.py:

  [0x00:0x04]  baseOffset      (0x00800000; file data region starts here)
  [0x04:0x08]  00 00 00 03     (version / tag)
  [0x08]       01              (flag)
  [0x09:0x0D]  tocZSize        (compressed TOC length, zlib, NO adler32 tail)
  [0x0D:0x11]  tocSize         (uncompressed TOC length)
  [0x11 .. ]   zlib(TOC)       (stream may lack the trailing adler32)

TOC (after decompression):
  [0:4]        entryCount
  per entry:
    [0:4]      nameLen          (includes the trailing NUL)
    [..]       name (NUL-terminated, backslash paths)
    [4]        fileOffset       (relative to baseOffset)
    [8]        uncompressedSize
    [12]       compressedSize
    [16]       flag             (0)
    [17:19]    pad              (0 0)
    [19]       f2               (1 or 2)
    [20:24]    tail1            (== uncompressedSize when f2==1)
    [24:28]    tail2            (present only when f2==2; tail1+tail2==uncomp)

File payloads are zlib-compressed (standard, WITH adler32 is fine on
write). Compressed payloads are placed at baseOffset + fileOffset.

Usage:
  bnk.lua list <bnk>
  bnk.lua extract <bnk> <name-substr> <outdir>
  bnk.lua replace <bnk> <name-substr> <payload-file>
      - payload is written COMPRESSED at the entry's existing offset; it
        MUST be <= the entry's current compressed size (padding zeros).
"""
import struct
import sys
import zlib


def _decompress_lenient(raw: bytes) -> bytes:
    """zlib that tolerates a missing trailing adler32."""
    d = zlib.decompressobj()
    return d.decompress(raw)


def read(bnk: bytes):
    base = struct.unpack(">I", bnk[0:4])[0]
    toc_z = struct.unpack(">I", bnk[0x09:0x0D])[0]
    toc_u = struct.unpack(">I", bnk[0x0D:0x11])[0]
    toc = _decompress_lenient(bnk[0x11:0x11 + toc_z])
    count = struct.unpack(">I", toc[0:4])[0]
    entries = []
    off = 4
    for _ in range(count):
        nlen = struct.unpack(">I", toc[off:off + 4])[0]
        name = toc[off + 4:off + 4 + nlen].split(b"\x00")[0].decode()
        meta = off + 4 + nlen
        o, u, c = struct.unpack(">III", toc[meta:meta + 12])
        f2 = toc[meta + 15]
        entries.append(
            {
                "name": name,
                "offset": o,
                "uncomp": u,
                "comp": c,
                "f2": f2,
                "toc_off": off,  # index of this entry's nameLen in the TOC
            }
        )
        off += 4 + nlen + (20 if f2 == 1 else 24)
    return base, toc_z, toc_u, toc, entries


def payload(bnk: bytes, base: int, e) -> bytes:
    raw = bnk[base + e["offset"]: base + e["offset"] + e["comp"]]
    return _decompress_lenient(raw)


def list_entries(bnk: bytes):
    base, toc_z, toc_u, toc, entries = read(bnk)
    print(
        f"base={base:#x} tocZ={toc_z:#x} tocU={toc_u:#x} entries={len(entries)}"
    )
    for e in entries:
        print(
            f"{e['name']}  off={e['offset']:#08x} u={e['uncomp']:#07x} "
            f"c={e['comp']:#07x} f2={e['f2']}"
        )


def extract(bnk: bytes, needle: str, outdir: str):
    import os

    base, *_r, entries = read(bnk)
    os.makedirs(outdir, exist_ok=True)
    for e in entries:
        if needle in e["name"]:
            data = payload(bnk, base, e)
            fn = e["name"].replace("\\", "_")
            path = os.path.join(outdir, fn)
            with open(path, "wb") as f:
                f.write(data)
            print(f"wrote {path} ({len(data)} bytes) <- {e['name']}")


def replace(bnk: bytes, needle: str, payload_file: str) -> bytes:
    base, toc_z, toc_u, toc, entries = read(bnk)
    matches = [e for e in entries if needle in e["name"]]
    if len(matches) != 1:
        raise SystemExit(
            f"expected exactly 1 match for {needle!r}, got {len(matches)}: "
            + ", ".join(m["name"] for m in matches)
        )
    e = matches[0]
    new_raw = open(payload_file, "rb").read()
    if len(new_raw) > e["comp"]:
        raise SystemExit(
            f"new payload {len(new_raw)} > entry compressed size {e['comp']}; "
            "shorten the payload"
        )
    comp = zlib.compress(new_raw, 9)
    if len(comp) > e["comp"]:
        raise SystemExit(
            f"compressed payload {len(comp)} > entry size {e['comp']}; "
            "shorten the payload"
        )

    out = bytearray(bnk)
    # 1) replace the file payload at its existing offset, zero the tail
    start = base + e["offset"]
    out[start:start + len(comp)] = comp
    out[start + len(comp): start + e["comp"]] = b"\x00" * (e["comp"] - len(comp))

    # 2) patch the TOC entry sizes (uncomp + comp [+ tail])
    meta = e["toc_off"] + 4
    nlen = struct.unpack(">I", toc[meta - 4: meta])[0]
    m2 = meta + nlen
    new_u = len(new_raw)
    new_c = len(comp)
    toc = bytearray(toc)
    struct.pack_into(">I", toc, m2 + 4, new_u)   # uncompressedSize
    struct.pack_into(">I", toc, m2 + 8, new_c)   # compressedSize
    struct.pack_into(">I", toc, m2 + 20, new_u)  # tail1
    if e["f2"] == 2:
        # keep tail1+tail2 == uncompressedSize
        old_tail1 = struct.unpack(">I", toc[m2 + 20: m2 + 24])[0]
        old_tail2 = struct.unpack(">I", toc[m2 + 24: m2 + 28])[0]
        # distribute the delta to tail1 (clamped), tail2 keeps the rest
        delta = new_u - (old_tail1 + old_tail2)
        t1 = max(0, min(old_tail1 + delta, new_u))
        t2 = new_u - t1
        struct.pack_into(">I", toc, m2 + 20, t1)
        struct.pack_into(">I", toc, m2 + 24, t2)

    # 3) recompress the TOC and rewrite the header
    toc_new = zlib.compress(bytes(toc), 9)
    struct.pack_into(">I", out, 0x09, len(toc_new))
    struct.pack_into(">I", out, 0x0D, len(toc))
    # clear the old TOC region then write the new one
    out[0x11: 0x11 + toc_z] = b"\x00" * toc_z
    out[0x11: 0x11 + len(toc_new)] = toc_new
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd = sys.argv[1]
    bnk = open(sys.argv[2], "rb").read()
    if cmd == "list":
        list_entries(bnk)
    elif cmd == "extract":
        extract(bnk, sys.argv[3], sys.argv[4])
    elif cmd == "replace":
        data = replace(bnk, sys.argv[3], sys.argv[4])
        with open(sys.argv[2], "wb") as f:
            f.write(data)
        print(f"wrote {sys.argv[2]} ({len(data)} bytes)")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
