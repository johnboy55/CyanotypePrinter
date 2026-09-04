"""Convert a fitted exposure->density curve into the final 16-entry LUT, with
steps equally spaced in a perceptual (CIE L*) or density domain.

Reflection density here is absolute (D = -log10(reflectance)), because the
reference card used to build the camera-response correction (camera_response.py)
supplies absolute reflectance values -- so it's valid to use the standard
absolute CIE L* formula rather than something normalized to the paper-white
patch.
"""
from __future__ import annotations

import math

from scipy.interpolate import PchipInterpolator
from scipy.optimize import brentq


class LutSolveError(ValueError):
    pass


def density_to_Lstar(d: float) -> float:
    y = 10 ** (-d)
    if y > 0.008856:
        return 116 * y ** (1 / 3) - 16
    return 903.3 * y


def Lstar_to_density(l_star: float) -> float:
    if l_star > 8.0:
        y = ((l_star + 16) / 116) ** 3
    else:
        y = l_star / 903.3
    y = max(y, 1e-6)
    return -math.log10(y)


def solve_16_exposures(
    curve: PchipInterpolator,
    exposure_domain: tuple[float, float],
    dmin: float,
    dmax_target: float,
    mode: str = "Lstar",
) -> tuple[list[float], list[str]]:
    """Returns (16 exposures in seconds, warnings). exposure_domain is the
    [min, max] exposure actually sampled by the fitted curve -- roots are
    only searched inside it, never extrapolated past real data."""
    warnings: list[str] = []
    e_lo, e_hi = exposure_domain
    if e_hi <= e_lo:
        raise LutSolveError("degenerate exposure domain")

    dmax_measured = float(curve(e_hi))
    dmax_used = min(dmax_target, dmax_measured)
    if dmax_used < dmax_target - 1e-6:
        warnings.append(
            f"requested target Dmax {dmax_target:.2f} exceeds the measured achievable "
            f"Dmax {dmax_measured:.2f} at the sampled max exposure -- clamped to {dmax_used:.2f}. "
            f"Increase --emax and reprint the target to reach a higher Dmax."
        )

    if mode == "Lstar":
        l_top, l_bot = density_to_Lstar(dmin), density_to_Lstar(dmax_used)
        targets_L = [l_top + (l_bot - l_top) * i / 15 for i in range(16)]
        target_densities = [Lstar_to_density(l) for l in targets_L]
    elif mode == "density":
        target_densities = [dmin + (dmax_used - dmin) * i / 15 for i in range(16)]
    else:
        raise LutSolveError(f"unknown mode '{mode}' -- expected 'Lstar' or 'density'")

    def solve_one(target_d: float, is_first: bool) -> float:
        if is_first:
            return 0.0
        f = lambda e: float(curve(e)) - target_d
        f_lo, f_hi = f(e_lo), f(e_hi)
        if f_lo * f_hi > 0:
            # Target density unreachable within the sampled domain -- clamp to
            # whichever end is closer rather than failing the whole run.
            clamped = e_lo if abs(f_lo) < abs(f_hi) else e_hi
            warnings.append(
                f"target density {target_d:.3f} falls outside the fitted curve's range "
                f"[{curve(e_lo):.3f}, {curve(e_hi):.3f}] -- clamped exposure to {clamped:.3f}s"
            )
            return clamped
        return brentq(f, e_lo, e_hi)

    exposures = [solve_one(d, i == 0) for i, d in enumerate(target_densities)]
    # Enforce non-decreasing exposures -- guards against fit noise producing a
    # tiny inversion between adjacent solved points.
    for i in range(1, len(exposures)):
        if exposures[i] < exposures[i - 1]:
            exposures[i] = exposures[i - 1]
    return exposures, warnings
