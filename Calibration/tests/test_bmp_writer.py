import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pytest

from calib.bmp_writer import MAX_IMG_WIDTH, BmpWriteError, write_grayscale_bmp


def _read_bmp_like_firmware(path):
    """Mirrors the checks/parsing MotorController_Mega/uv_print.cpp's
    readFirst4RowsBMP performs, to catch any header drift without needing
    the actual firmware."""
    with open(path, "rb") as f:
        data = f.read()
    bf_type, bf_size, _, _, bf_off_bits = struct.unpack_from("<2sIHHI", data, 0)
    assert bf_type == b"BM"
    (bi_size, bi_width, bi_height, bi_planes, bi_bit_count, bi_compression,
     bi_size_image, _, _, bi_clr_used, _) = struct.unpack_from("<IiiHHIIiiII", data, 14)
    assert bi_compression == 0
    assert bi_bit_count in (8, 24)
    bottom_up = bi_height > 0
    h = abs(bi_height)
    w = bi_width
    row_bytes_raw = (bi_bit_count * w + 7) // 8
    row_stride = (row_bytes_raw + 3) & ~3

    def read_visual_row(visual):
        src_row = (h - 1 - visual) if bottom_up else visual
        offset = bf_off_bits + src_row * row_stride
        return data[offset:offset + w]

    return w, h, read_visual_row


def test_round_trip_pixel_values(tmp_path):
    rows = [[0, 17, 255], [128, 128, 128]]
    path = str(tmp_path / "t.bmp")
    write_grayscale_bmp(path, rows)
    w, h, read_visual_row = _read_bmp_like_firmware(path)
    assert (w, h) == (3, 2)
    assert list(read_visual_row(0)) == rows[0]
    assert list(read_visual_row(1)) == rows[1]


def test_row_padding_to_4_bytes(tmp_path):
    # width=3 -> row_bytes_raw=3 -> padded stride=4 (1 pad byte)
    rows = [[10, 20, 30]]
    path = str(tmp_path / "t.bmp")
    write_grayscale_bmp(path, rows)
    with open(path, "rb") as f:
        data = f.read()
    (_, bi_width, bi_height, _, bi_bit_count, *_rest) = struct.unpack_from("<IiiHHIIiiII", data, 14)
    row_bytes_raw = (bi_bit_count * bi_width + 7) // 8
    stride = (row_bytes_raw + 3) & ~3
    assert stride == 4
    assert row_bytes_raw == 3


def test_refuses_oversized_width(tmp_path):
    rows = [[0] * (MAX_IMG_WIDTH + 1)]
    with pytest.raises(BmpWriteError):
        write_grayscale_bmp(str(tmp_path / "t.bmp"), rows)


def test_max_width_is_allowed(tmp_path):
    rows = [[0] * MAX_IMG_WIDTH]
    write_grayscale_bmp(str(tmp_path / "t.bmp"), rows)  # should not raise


def test_rejects_ragged_rows(tmp_path):
    with pytest.raises(BmpWriteError):
        write_grayscale_bmp(str(tmp_path / "t.bmp"), [[0, 0], [0, 0, 0]])


def test_rejects_out_of_range_pixel(tmp_path):
    with pytest.raises(BmpWriteError):
        write_grayscale_bmp(str(tmp_path / "t.bmp"), [[0, 256]])


def test_opens_with_pillow(tmp_path):
    pil = pytest.importorskip("PIL.Image")
    rows = [[i * 17 for i in range(16)] for _ in range(4)]
    path = str(tmp_path / "t.bmp")
    write_grayscale_bmp(path, rows)
    im = pil.open(path)
    assert im.size == (16, 4)
    assert list(im.getdata())[:16] == rows[0]
