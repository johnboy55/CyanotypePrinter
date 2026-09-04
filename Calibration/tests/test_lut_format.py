import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pytest

from calib.lut_format import LutFormatError, format_lut, parse_lut, read_lut, write_lut

REPO_ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
SAMPLE_LUT = os.path.join(REPO_ROOT, "SDCard", "calibrations", "3sec.txt")


def test_parse_real_calibration_file():
    with open(SAMPLE_LUT) as f:
        text = f.read()
    values = parse_lut(text)
    assert len(values) == 16
    assert values[0] == 0.0
    assert values[-1] == 3.0
    assert values == sorted(values)  # 3sec.txt is a monotonic ramp


def test_format_round_trips_real_file_values():
    with open(SAMPLE_LUT) as f:
        original_text = f.read().strip()
    values = parse_lut(original_text)
    assert format_lut(values) == original_text


def test_format_rejects_wrong_length():
    with pytest.raises(LutFormatError):
        format_lut([0.0] * 15)


def test_format_rejects_nonzero_first_entry():
    with pytest.raises(LutFormatError):
        format_lut([0.1] + [1.0] * 15)


def test_parse_rejects_wrong_token_count():
    with pytest.raises(LutFormatError):
        parse_lut("0.0,1.0,2.0")


def test_parse_rejects_non_numeric():
    with pytest.raises(LutFormatError):
        parse_lut(",".join(["x"] * 16))


def test_write_and_read_round_trip(tmp_path):
    values = [round(3.0 * n / 15, 2) for n in range(16)]
    path = str(tmp_path / "test_lut.txt")
    write_lut(path, values)
    assert read_lut(path) == values
