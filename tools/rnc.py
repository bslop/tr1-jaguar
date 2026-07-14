#!/usr/bin/env python3
"""RNC ProPack decompressor (method 2), used by the TR1 PSX disc for the
DELDATA/*.RAW title/loading images.  Method-2 algorithm ported from ScummVM's
unpackM2 (common/compression/rnc_deco.cpp) and verified byte-exact against the
disc's decompressed images.  Pure Python, no dependencies -> deterministic and
reproducible inside the pinned build container.

    from rnc import rnc_unpack
    raw = open("AMERTIT.RAW","rb").read()
    bgr555 = rnc_unpack(raw)          # -> bytes (unpacked image)

CLI:  python3 rnc.py IN.RAW OUT.bin
"""
import struct, sys


def rnc2_unpack(data):
    """Decompress an RNC method-2 (b"RNC\\x02") blob. Returns the raw bytes."""
    if data[0:3] != b"RNC" or data[3] != 2:
        raise ValueError("not an RNC method-2 stream")
    unpack_len = struct.unpack_from(">I", data, 4)[0]
    p = 18                       # packed data follows the 18-byte header
    buf = 0
    cnt = 0
    out = bytearray()

    def bit():
        nonlocal buf, cnt, p
        if cnt == 0:
            buf = data[p]; p += 1; cnt = 8
        t = (buf >> 7) & 1
        buf = (buf << 1) & 0xFF
        cnt -= 1
        return t

    def rbyte():
        nonlocal p
        v = data[p]; p += 1
        return v

    bit(); bit()                 # two initial, unused bits

    while True:
        load_val = False
        while bit() == 0:        # literal run
            out.append(rbyte())

        length = 2
        ofs_hi = 0
        if bit() == 0:
            length = (length << 1) | bit()          # 4 or 5
            if bit() == 1:
                length -= 1                          # 3 or 4
                length = (length << 1) | bit()       # 6,7,8,9
                if length == 9:                      # long literal run
                    length = 4
                    while length:
                        ofs_hi = (ofs_hi << 1) | bit(); length -= 1
                    n = (ofs_hi + 3) * 4
                    while n:
                        out.append(rbyte()); n -= 1
                    continue
            load_val = True
        else:
            if bit() == 1:
                length += 1                          # 3
                if bit() == 1:
                    length = rbyte()
                    if length == 0:
                        if bit() == 1:
                            continue                 # end of chunk, more follows
                        break                        # end of stream
                    length += 8
                load_val = True

        if load_val and bit() == 1:
            ofs_hi = (ofs_hi << 1) | bit()
            if bit() == 1:
                ofs_hi = ((ofs_hi << 1) | bit()) | 4
                if bit() == 0:
                    ofs_hi = (ofs_hi << 1) | bit()
            elif ofs_hi == 0:
                ofs_hi = 2 | bit()

        ofs = (ofs_hi << 8) | rbyte()
        src = len(out) - ofs - 1
        for _ in range(length):
            out.append(out[src]); src += 1

    if len(out) != unpack_len:
        raise ValueError("RNC unpack length mismatch: got %d, header says %d"
                         % (len(out), unpack_len))
    return bytes(out)


def rnc_unpack(data):
    """Decompress RNC (any method); pass through data that is not RNC."""
    if data[0:3] == b"RNC" and len(data) > 3 and data[3] == 2:
        return rnc2_unpack(data)
    if data[0:3] == b"RNC" and len(data) > 3 and data[3] == 1:
        raise NotImplementedError("RNC method 1 not needed for TR1 title art")
    return data                  # already raw


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: rnc.py IN.RAW OUT.bin")
    blob = open(sys.argv[1], "rb").read()
    open(sys.argv[2], "wb").write(rnc_unpack(blob))
