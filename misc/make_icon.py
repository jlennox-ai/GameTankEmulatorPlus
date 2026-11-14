# Draws the GameTank emulator's icon as 16x16 pixel art (a tank on a transparent background) and
# writes it as an .ico (PNG images at 16, 32, 48 and 256, scaled without smoothing) plus a
# 256-pixel preview PNG.
#
#   python misc/make_icon.py res/gametank_emulator.ico <preview.png>
import struct, sys, zlib

ART = [
    "................",
    "................",
    "................",
    "................",
    ".....LLLL.......",
    "....LTTTTLLLLLL.",
    "....LTTTTL......",
    "...LLLLLLLLL....",
    "..LGGGGGGGGGGL..",
    "..LDDDDDDDDDDL..",
    ".KKKKKKKKKKKKKK.",
    ".KKWKKWKKWKKWKK.",
    "..KKKKKKKKKKKK..",
    "................",
    "................",
    "................",
]
COLORS = {
    ".": (0, 0, 0, 0),
    "L": (0, 135, 81, 255),     # outline green
    "T": (0, 228, 54, 255),     # turret
    "G": (0, 200, 60, 255),     # hull
    "D": (0, 110, 50, 255),     # hull shadow
    "K": (60, 60, 70, 255),     # tread
    "W": (194, 195, 199, 255),  # wheel
}

def png(size):
    scale = size // 16
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        for x in range(size):
            raw.extend(COLORS[ART[y // scale][x // scale]])
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))

assert all(len(row) == 16 for row in ART) and len(ART) == 16
ico_path, preview_path = sys.argv[1], sys.argv[2]
sizes = [16, 32, 48, 256]
images = [png(size) for size in sizes]
header = struct.pack("<HHH", 0, 1, len(images))
offset = 6 + 16 * len(images)
entries = b""
for size, image in zip(sizes, images):
    entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
    offset += len(image)
open(ico_path, "wb").write(header + entries + b"".join(images))
open(preview_path, "wb").write(images[-1])
