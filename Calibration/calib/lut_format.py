"""Read/write the printer's 16-entry exposure LUT file.

Format contract, matching MotorController_Mega/uv_sd.cpp:loadCalibrationFile()
exactly: a single line of 16 comma-separated float seconds. The firmware parser
just walks tokens split on ',' and calls toFloat() on each (stopping at 16), so
extra whitespace or a trailing newline are harmless, but the file must contain
exactly 16 numeric tokens or the firmware will silently reject the load
(loadCalibrationFile returns false unless it collects all 16).
"""
from __future__ import annotations

LUT_LEN = 16


class LutFormatError(ValueError):
    pass


def format_lut(values: list[float]) -> str:
    if len(values) != LUT_LEN:
        raise LutFormatError(f"LUT must have exactly {LUT_LEN} values, got {len(values)}")
    if abs(values[0]) > 1e-9:
        raise LutFormatError(f"LUT entry 0 must be 0.00 (paper white / no exposure), got {values[0]}")
    return ",".join(f"{v:.2f}" for v in values)


def write_lut(path: str, values: list[float]) -> None:
    text = format_lut(values)
    with open(path, "w", newline="\n") as f:
        f.write(text + "\n")


def parse_lut(text: str) -> list[float]:
    tokens = [t.strip() for t in text.strip().split(",")]
    tokens = [t for t in tokens if t != ""]
    if len(tokens) != LUT_LEN:
        raise LutFormatError(f"expected {LUT_LEN} comma-separated values, found {len(tokens)}")
    try:
        values = [float(t) for t in tokens]
    except ValueError as e:
        raise LutFormatError(f"non-numeric LUT token: {e}") from e
    return values


def read_lut(path: str) -> list[float]:
    with open(path, "r") as f:
        return parse_lut(f.read())
