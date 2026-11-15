"""Stage 2 core: turn a photo of a developed target (plus the reference-card
and fiducial points picked on it) into the final 16-entry LUT.

Shared by analyze_target.py (desktop, matplotlib clicking) and the web app
(web/app.py, points tapped in a browser) so both run exactly the same math.
"""
from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field

import numpy as np

from .camera_response import CameraResponseError, ReferenceCard, fit_camera_response
from .curve_fit import LevelMeasurement, check_dmax_reached, check_monotonic, combine_repeats, fit_curve
from .lut_solve import LutSolveError, solve_16_exposures
from .perspective import PerspectiveError, compute_homography, sample_patch_mean, sample_point_mean, to_grayscale

FIDUCIAL_ORDER = ("TL", "TR", "BL", "BR")


class AnalysisError(ValueError):
    """A problem that stops the LUT from being produced at all."""


@dataclass
class AnalysisResult:
    exposures: list[float]
    levels: list[LevelMeasurement]
    curve: object  # callable exposure_s -> density
    warnings: list[str] = field(default_factory=list)
    dmax_warning: str | None = None
    mono_warnings: list[str] = field(default_factory=list)


def fiducial_centers_mm(sidecar: dict) -> list[tuple[float, float]]:
    by_name = {f["name"]: f for f in sidecar["fiducials"]}
    out = []
    for name in FIDUCIAL_ORDER:
        f = by_name[name]
        out.append((f["x_mm"] + f["size_mm"] / 2, f["y_mm"] + f["size_mm"] / 2))
    return out


def analyze(
    photo_rgb: np.ndarray,
    sidecar: dict,
    card: ReferenceCard,
    fiducial_photo_px: list[tuple[float, float]],
    card_points_px: list[tuple[float, float]],
    target_dmax: float,
    mode: str = "Lstar",
) -> AnalysisResult:
    """photo_rgb is an HxWx3 array (0..255). fiducial_photo_px are the clicked
    fiducial centers in FIDUCIAL_ORDER; card_points_px one point per card
    patch, in card.labels order. Raises AnalysisError on anything fatal;
    non-fatal problems come back in AnalysisResult.warnings."""
    if len(fiducial_photo_px) != 4:
        raise AnalysisError(f"need 4 fiducial points (TL, TR, BL, BR), got {len(fiducial_photo_px)}")
    if len(card_points_px) != len(card.labels):
        raise AnalysisError(f"need {len(card.labels)} reference-card points, got {len(card_points_px)}")

    gray = to_grayscale(np.asarray(photo_rgb, dtype=np.float64))
    warnings: list[str] = []

    try:
        homography = compute_homography(fiducial_centers_mm(sidecar), fiducial_photo_px)
        card_luma = [sample_point_mean(gray, p) for p in card_points_px]
        response_fn = fit_camera_response(card_luma, card)
    except (PerspectiveError, CameraResponseError) as e:
        raise AnalysisError(str(e)) from e

    exposure_to_densities: dict[float, list[float]] = defaultdict(list)
    for patch in sidecar["patches"]:
        try:
            luma = sample_patch_mean(gray, homography, patch["x_mm"], patch["y_mm"], patch["w_mm"], patch["h_mm"])
        except PerspectiveError as e:
            warnings.append(f"skipping patch {patch['id']}: {e}")
            continue
        exposure_to_densities[patch["seed_exposure_s"]].append(response_fn(luma))

    levels = combine_repeats(exposure_to_densities)
    for lv in levels:
        if lv.low_confidence:
            warnings.append(f"exposure {lv.exposure_s:.3f}s has low confidence "
                            f"({lv.n_kept}/{lv.n_total} repeats kept)")

    dmax_warning = check_dmax_reached(levels)
    if dmax_warning:
        warnings.append(dmax_warning)

    levels, mono_warnings = check_monotonic(levels)
    warnings.extend(mono_warnings)

    try:
        curve = fit_curve(levels)
    except ValueError as e:
        raise AnalysisError(str(e)) from e

    dmin = levels[0].density
    exposure_domain = (levels[0].exposure_s, levels[-1].exposure_s)
    try:
        exposures, solve_warnings = solve_16_exposures(curve, exposure_domain, dmin, target_dmax, mode)
    except LutSolveError as e:
        raise AnalysisError(str(e)) from e
    warnings.extend(solve_warnings)

    return AnalysisResult(
        exposures=exposures, levels=levels, curve=curve, warnings=warnings,
        dmax_warning=dmax_warning, mono_warnings=mono_warnings,
    )


def write_diagnostic_plot(dest, result: AnalysisResult) -> None:
    """dest is a path or a binary file-like object; written as PNG."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    levels, curve, exposures = result.levels, result.curve, result.exposures
    xs = [lv.exposure_s for lv in levels]
    ys = [lv.density for lv in levels]
    fine_x = np.linspace(xs[0], xs[-1], 200)
    fine_y = [float(curve(x)) for x in fine_x]

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(xs, ys, "o", label="measured (per exposure level)")
    ax.plot(fine_x, fine_y, "-", label="fitted curve")
    ax.plot(exposures, [float(curve(min(max(e, xs[0]), xs[-1]))) for e in exposures], "x", markersize=10, label="chosen 16 LUT points")
    ax.set_xlabel("exposure (s)")
    ax.set_ylabel("reflection density")
    title = "Calibration curve"
    if result.dmax_warning or result.mono_warnings:
        title += "  [see warnings]"
    ax.set_title(title)
    ax.legend()
    fig.tight_layout()
    fig.savefig(dest, dpi=150, format="png")
    plt.close(fig)
