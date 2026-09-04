"""Combine repeated patch measurements into one density per exposure level,
flag Dmax-not-reached / fogging-reversal problems, and fit a monotonic
exposure -> density curve.
"""
from __future__ import annotations

import statistics
from dataclasses import dataclass

from scipy.interpolate import PchipInterpolator


@dataclass
class LevelMeasurement:
    exposure_s: float
    density: float
    n_kept: int
    n_total: int
    low_confidence: bool


def combine_repeats(exposure_to_densities: dict[float, list[float]]) -> list[LevelMeasurement]:
    """Median + MAD-based outlier rejection across the repeats of each
    exposure level, so a single fogged/glared/misregistered patch doesn't
    skew that level's result."""
    out = []
    for exposure_s, values in sorted(exposure_to_densities.items()):
        n_total = len(values)
        med = statistics.median(values)
        mad = statistics.median(abs(v - med) for v in values)
        threshold = 3 * 1.4826 * mad if mad > 0 else float("inf")
        kept = [v for v in values if abs(v - med) <= threshold]
        if not kept:
            kept = values
        final = statistics.median(kept)
        out.append(LevelMeasurement(
            exposure_s=exposure_s, density=final,
            n_kept=len(kept), n_total=n_total,
            low_confidence=len(kept) < 2,
        ))
    return out


def check_dmax_reached(levels: list[LevelMeasurement], tail_frac: float = 0.2, warn_frac: float = 0.25) -> str | None:
    """Warn if the curve is still rising steeply at the top of the exposure
    range -- i.e. no shoulder/plateau visible, meaning Emax was probably too
    low to reach a true Dmax for this paper/process/chemistry."""
    if len(levels) < 3:
        return None
    xs = [lv.exposure_s for lv in levels]
    ys = [lv.density for lv in levels]
    slopes = [(ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]) for i in range(len(xs) - 1) if xs[i + 1] > xs[i]]
    if not slopes:
        return None
    overall_max_slope = max(slopes)
    if overall_max_slope <= 0:
        return None
    x_range = xs[-1] - xs[0]
    tail_start = xs[-1] - tail_frac * x_range
    tail_slopes = [
        (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i])
        for i in range(len(xs) - 1)
        if xs[i] >= tail_start and xs[i + 1] > xs[i]
    ]
    if not tail_slopes:
        return None
    avg_tail_slope = sum(tail_slopes) / len(tail_slopes)
    if avg_tail_slope > warn_frac * overall_max_slope:
        return (
            f"Dmax may not have been reached -- density is still rising at "
            f"{avg_tail_slope:.2f} D/s near the top of the exposure range "
            f"(vs. a steepest observed slope of {overall_max_slope:.2f} D/s). "
            f"Consider increasing --emax and reprinting the target."
        )
    return None


def check_monotonic(levels: list[LevelMeasurement], tol: float = 0.03) -> tuple[list[LevelMeasurement], list[str]]:
    """Drop (and warn about) any point where density decreases with
    increasing exposure beyond noise tolerance -- a sign of fogging/
    solarization reversal that would corrupt the curve fit if left in."""
    warnings: list[str] = []
    kept: list[LevelMeasurement] = []
    running_max = float("-inf")
    for lv in levels:
        if lv.density < running_max - tol:
            warnings.append(
                f"non-monotonic response at {lv.exposure_s:.3f}s (density {lv.density:.3f} "
                f"dropped below the running max {running_max:.3f}) -- excluded from the fit; "
                f"possible fogging/solarization reversal"
            )
            continue
        running_max = max(running_max, lv.density)
        kept.append(lv)
    return kept, warnings


def fit_curve(levels: list[LevelMeasurement]) -> PchipInterpolator:
    if len(levels) < 3:
        raise ValueError("need at least 3 exposure levels to fit a curve")
    xs = [lv.exposure_s for lv in levels]
    ys = [lv.density for lv in levels]
    return PchipInterpolator(xs, ys, extrapolate=False)
