"""Reference-card presets and the luma -> reflection-density correction fit
that absorbs the camera/lighting's arbitrary exposure and tone curve.

The presets below are NOMINAL values for the named product lines, not values
measured off any specific physical card. Real Q-13/Q-14 and ColorChecker
units vary batch to batch and often ship with (or can be found with
published) per-unit or per-lot density/reflectance tables -- for real
calibration work, prefer `--reference-card custom-csv` with your card's
actual documented values over these defaults.
"""
from __future__ import annotations

import csv
import math
from dataclasses import dataclass

from scipy.interpolate import PchipInterpolator


class CameraResponseError(ValueError):
    pass


@dataclass
class ReferenceCard:
    name: str
    labels: list[str]
    densities: list[float]  # -log10(reflectance), ascending white -> black


def _linspace(a: float, b: float, n: int) -> list[float]:
    if n == 1:
        return [a]
    step = (b - a) / (n - 1)
    return [a + step * i for i in range(n)]


def _reflectance_pct_to_density(pct: float) -> float:
    return -math.log10(pct / 100.0)


def _q_gray_scale(name: str) -> ReferenceCard:
    # Nominal 20-step reflection gray scale, ~0.05 to ~1.90 density.
    densities = _linspace(0.05, 1.90, 20)
    labels = [f"step{i}" for i in range(20)]
    return ReferenceCard(name=name, labels=labels, densities=densities)


def _colorchecker_gray_row(name: str) -> ReferenceCard:
    # Nominal published reflectance %, X-Rite ColorChecker Classic row 4 (white->black).
    pct = [90.0, 59.1, 36.2, 19.8, 9.3, 3.1]
    labels = ["white", "neutral8", "neutral6.5", "neutral5", "neutral3.5", "black"]
    return ReferenceCard(name=name, labels=labels, densities=[_reflectance_pct_to_density(p) for p in pct])


def load_custom_csv(path: str) -> ReferenceCard:
    """CSV with a header row and columns `label,density` (preferred) or
    `label,reflectance_pct` -- exactly one of the two value columns."""
    labels: list[str] = []
    densities: list[float] = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise CameraResponseError(f"{path}: empty or missing header row")
        has_density = "density" in reader.fieldnames
        has_reflectance = "reflectance_pct" in reader.fieldnames
        if has_density == has_reflectance:
            raise CameraResponseError(
                f"{path}: expected exactly one of columns 'density' or 'reflectance_pct'"
            )
        for row in reader:
            labels.append(row.get("label", f"patch{len(labels)}"))
            if has_density:
                densities.append(float(row["density"]))
            else:
                densities.append(_reflectance_pct_to_density(float(row["reflectance_pct"])))
    if len(labels) < 3:
        raise CameraResponseError(f"{path}: need at least 3 patches to fit a camera-response curve")
    return ReferenceCard(name=f"custom:{path}", labels=labels, densities=densities)


_BUILTIN_PRESETS = {
    "q13": lambda: _q_gray_scale("q13"),
    "q14": lambda: _q_gray_scale("q14"),
    "colorchecker_classic": lambda: _colorchecker_gray_row("colorchecker_classic"),
    "colorchecker_mini": lambda: _colorchecker_gray_row("colorchecker_mini"),
}


def load_reference_card(name: str, custom_csv: str | None = None) -> ReferenceCard:
    if name == "custom-csv":
        if not custom_csv:
            raise CameraResponseError("--reference-card custom-csv requires --reference-card-csv PATH")
        return load_custom_csv(custom_csv)
    if name not in _BUILTIN_PRESETS:
        raise CameraResponseError(
            f"unknown reference card '{name}' -- choices: {sorted(_BUILTIN_PRESETS)} or custom-csv"
        )
    return _BUILTIN_PRESETS[name]()


def rgb_to_luma(r: float, g: float, b: float) -> float:
    # Same weights the firmware uses for its own 24-bit BMP -> gray conversion
    # (uv_print.cpp), kept here only for consistency, not for firmware compatibility.
    return 0.299 * r + 0.587 * g + 0.114 * b


def fit_camera_response(measured_luma: list[float], card: ReferenceCard):
    """Returns a callable luma -> density, monotonic and clamped to the
    calibration range. Requires len(measured_luma) == len(card.densities),
    one measured luma per card patch in the same order as card.labels."""
    if len(measured_luma) != len(card.densities):
        raise CameraResponseError(
            f"got {len(measured_luma)} measured patches, card '{card.name}' has {len(card.densities)}"
        )
    pairs = sorted(zip(measured_luma, card.densities), key=lambda p: p[0])
    lumas = [p[0] for p in pairs]
    densities = [p[1] for p in pairs]
    for i in range(1, len(lumas)):
        if lumas[i] <= lumas[i - 1]:
            raise CameraResponseError(
                "reference card patches did not read out in monotonic luma order -- "
                "check for glare/misclicks or a card too small/blurry in the photo"
            )
    interp = PchipInterpolator(lumas, densities, extrapolate=False)
    lo, hi = lumas[0], lumas[-1]

    def response(luma: float) -> float:
        clamped = min(max(luma, lo), hi)
        return float(interp(clamped))

    return response
