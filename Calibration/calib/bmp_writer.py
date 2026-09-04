"""Minimal uncompressed 8-bit grayscale BMP writer, firmware-compatible.

Written by hand (not via Pillow) for byte-exact control over the header fields
the firmware's BMP parser actually checks (MotorController_Mega/uv_print.cpp,
readFirst4RowsBMP / loadBMPMeta / BMPFileRowSource::open):

  - bfType must be 0x4D42 ('BM').
  - biCompression must be 0 (uncompressed).
  - biBitCount must be 8 or 24 -- this writer always emits 8 (grayscale), which
    also sidesteps the firmware's 24-bit -> gray luma conversion entirely, so
    the pixel byte the firmware reads is exactly the byte we wrote.
  - Row stride is padded to a 4-byte boundary, per the BMP spec.
  - biHeight > 0 means bottom-up storage; the firmware normalizes this so that
    "visual row 0" (the row passed to readRow()/img.row[0]) is always the top
    row as viewed in an ordinary image editor, regardless of storage order.
    This writer stores bottom-up (positive biHeight) so the file also opens
    correctly, top-to-bottom, in any standard image viewer.
  - Only width <= MAX_IMG_WIDTH is usable by the firmware (columns beyond that
    are silently ignored by the device). This writer refuses to write an
    oversized image rather than silently producing a file the firmware would
    truncate.
"""
from __future__ import annotations

import struct

MAX_IMG_WIDTH = 150  # MotorController_Mega/uv_types.h: MAX_IMG_WIDTH

FILE_HEADER_SIZE = 14
INFO_HEADER_SIZE = 40
PALETTE_SIZE = 256 * 4


class BmpWriteError(ValueError):
    pass


def _row_stride(width: int) -> int:
    return ((width + 3) // 4) * 4


def write_grayscale_bmp(path: str, rows: list[list[int]]) -> None:
    """rows[0] is the top of the image as viewed in an editor; rows[-1] is the
    bottom. Each row must be a sequence of ints in 0..255, all rows the same
    length. Writes a bottom-up (biHeight > 0), uncompressed, 8-bit grayscale
    BMP with an identity palette."""
    height = len(rows)
    if height == 0:
        raise BmpWriteError("image has zero rows")
    width = len(rows[0])
    if width == 0:
        raise BmpWriteError("image has zero width")
    if width > MAX_IMG_WIDTH:
        raise BmpWriteError(
            f"width {width}px exceeds firmware's MAX_IMG_WIDTH={MAX_IMG_WIDTH}px "
            f"(300mm at 2mm/pixel) -- reduce --patches or --patch-mm"
        )
    for r in rows:
        if len(r) != width:
            raise BmpWriteError("all rows must have the same width")
        for v in r:
            if not (0 <= v <= 255):
                raise BmpWriteError(f"pixel value {v} out of range 0..255")

    stride = _row_stride(width)
    pixel_data_size = stride * height
    off_bits = FILE_HEADER_SIZE + INFO_HEADER_SIZE + PALETTE_SIZE
    file_size = off_bits + pixel_data_size

    file_header = struct.pack(
        "<2sIHHI",
        b"BM",
        file_size,
        0,
        0,
        off_bits,
    )
    info_header = struct.pack(
        "<IiiHHIIiiII",
        INFO_HEADER_SIZE,
        width,
        height,  # positive -> bottom-up
        1,       # biPlanes
        8,       # biBitCount
        0,       # biCompression (BI_RGB, uncompressed)
        pixel_data_size,
        0,       # biXPelsPerMeter
        0,       # biYPelsPerMeter
        256,     # biClrUsed
        0,       # biClrImportant
    )
    palette = bytearray()
    for i in range(256):
        palette += bytes((i, i, i, 0))  # B, G, R, reserved -- identity gray

    with open(path, "wb") as f:
        f.write(file_header)
        f.write(info_header)
        f.write(palette)
        # Bottom-up storage: first row written is the image's bottom row.
        for file_row in range(height):
            src_row = rows[height - 1 - file_row]
            f.write(bytes(src_row))
            pad = stride - width
            if pad:
                f.write(b"\x00" * pad)
