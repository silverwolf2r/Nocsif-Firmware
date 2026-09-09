r"""
Builds the app icon files (nocsif.ico + nocsif_icon.png): a NocSif four-point star engraved on a
near-black badge, drawn procedurally with only the standard library so no binary art asset needs to
live in the repo.

    python gen_icon.py            -> nocsif.ico (16/32/48/256, PNG-compressed entries) + nocsif_icon.png
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
VOID = (0x07, 0x07, 0x08)
BONE = (0x8C, 0x8C, 0x92)
WHITE = (0xA6, 0xA6, 0xAC)
ACCENT = (0x65, 0x55, 0x78)


def star_alpha(x, y, cx, cy, r):
    """Alpha coverage (0..1) at one pixel for a thin four-point star: two crossed tapered spikes plus a soft center glow."""
    dx, dy = x - cx, y - cy
    d = math.hypot(dx, dy)
    if d > r:
        return 0.0
    a = 0.0
    for ux, uy in ((1, 0), (0, 1)):                     # the two spike directions
        along = abs(dx * ux + dy * uy)
        across = abs(dx * uy - dy * ux)
        width = max(0.6, (1.0 - along / r) * r * 0.10)  # spike narrows toward its tip
        if across < width:
            a = max(a, min(1.0, (width - across) / 0.9) * (1.0 - 0.35 * along / r))
    core = max(0.0, 1.0 - d / (r * 0.12))
    return min(1.0, a + core * core)


def render(size):
    cx = cy = (size - 1) / 2.0
    r = size * 0.42
    px = bytearray()
    for y in range(size):
        for x in range(size):
            # everything outside the rounded-square badge shape stays fully transparent
            nx, ny = (x - cx) / (size / 2.0), (y - cy) / (size / 2.0)
            inside = (abs(nx) ** 4 + abs(ny) ** 4) <= 0.92
            if not inside:
                px += bytes((0, 0, 0, 0))
                continue
            a = star_alpha(x + 0.5, y + 0.5, cx, cy, r)
            # a subtle accent-tinted ring (the "orrery") sits behind the star
            d = math.hypot(x - cx, y - cy)
            ring = max(0.0, 1.0 - abs(d - r * 0.78) / (size * 0.012))
            base = tuple(int(VOID[i] * (1 - ring * 0.6) + ACCENT[i] * ring * 0.6) for i in range(3))
            col = tuple(int(base[i] * (1 - a) + (WHITE[i] if a > 0.7 else BONE[i]) * a) for i in range(3))
            px += bytes(col + (255,))
    return bytes(px)


def png(size, rgba):
    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def ico(pngs):
    """Packs a list of (size, png_bytes) into a Windows .ico container using PNG-compressed frames (Vista+ only)."""
    head = struct.pack("<HHH", 0, 1, len(pngs))
    entries, data = b"", b""
    off = 6 + 16 * len(pngs)
    for size, blob in pngs:
        entries += struct.pack("<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0, 0, 0, 1, 32, len(blob), off)
        data += blob
        off += len(blob)
    return head + entries + data


if __name__ == "__main__":
    pngs = [(s, png(s, render(s))) for s in (16, 32, 48, 256)]
    with open(os.path.join(HERE, "nocsif.ico"), "wb") as fh:
        fh.write(ico(pngs))
    with open(os.path.join(HERE, "nocsif_icon.png"), "wb") as fh:
        fh.write(pngs[-1][1])
    print("wrote nocsif.ico + nocsif_icon.png")
