"""End-to-end self-test: render a fake 'photo' response for a known synthetic
process curve and a known reference card, run the full Stage 2 math pipeline
(camera_response -> curve_fit -> lut_solve) on it with no real photo or
perspective correction involved, and check the recovered LUT reproduces
perceptually-even steps against the KNOWN curve within tolerance.

This validates the math before any real photo exists; perspective.py's
interactive clicking and photo sampling are exercised only in real usage.
"""
import math
import os
import random
import sys
from collections import defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from calib.camera_response import ReferenceCard, fit_camera_response
from calib.curve_fit import check_dmax_reached, check_monotonic, combine_repeats, fit_curve
from calib.lut_solve import density_to_Lstar, solve_16_exposures
from calib.target_layout import build_target

EMAX = 3.0
D_MIN = 0.05
D_MAX_TRUE = 1.6
K = 2.0  # saturation rate


def true_density(exposure_s: float) -> float:
    """A plausible saturating (H&D-like) characteristic curve."""
    return D_MIN + (D_MAX_TRUE - D_MIN) * (1 - math.exp(-K * exposure_s))


def camera_luma(density: float, rng: random.Random, noise_sd: float = 1.5) -> float:
    """Simulates an arbitrary, uncalibrated camera+lighting response: some
    unknown gamma and gain on reflectance, plus a little photon/JPEG noise."""
    reflectance = 10 ** (-density)
    unknown_gamma = 0.7
    unknown_gain = 240.0
    luma = unknown_gain * (reflectance ** unknown_gamma)
    return max(0.0, min(255.0, luma + rng.gauss(0, noise_sd)))


def test_synthetic_pipeline_recovers_known_curve():
    rng = random.Random(1234)

    layout = build_target(emax_s=EMAX, n_levels=24, repeats=3, rng_seed=42)

    # Simulate photographing the reference card: 6-step gray row spanning
    # roughly the same density range as the process.
    card_true_densities = [0.05, 0.35, 0.65, 0.95, 1.25, 1.55]
    card = ReferenceCard(name="synthetic", labels=[f"p{i}" for i in range(6)], densities=card_true_densities)
    card_luma = [camera_luma(d, rng) for d in card_true_densities]
    response_fn = fit_camera_response(card_luma, card)

    # Simulate photographing every target patch.
    exposure_to_densities = defaultdict(list)
    for patch in layout.patches:
        true_d = true_density(patch.seed_exposure_s)
        luma = camera_luma(true_d, rng)
        measured_d = response_fn(luma)
        exposure_to_densities[patch.seed_exposure_s].append(measured_d)

    levels = combine_repeats(exposure_to_densities)
    assert check_dmax_reached(levels) is None, "synthetic curve saturates and should not warn"
    levels, mono_warnings = check_monotonic(levels)
    assert mono_warnings == []

    curve = fit_curve(levels)
    dmin = levels[0].density
    domain = (levels[0].exposure_s, levels[-1].exposure_s)

    target_dmax = 1.5  # comfortably inside the achievable range
    exposures, warnings = solve_16_exposures(curve, domain, dmin, target_dmax, mode="Lstar")

    assert len(exposures) == 16
    assert exposures[0] == 0.0
    assert exposures == sorted(exposures)
    assert exposures[-1] <= EMAX + 1e-6

    # The chosen exposures, run back through the TRUE (noise-free) curve,
    # should land close to equal L* steps.
    achieved_L = [density_to_Lstar(true_density(e)) for e in exposures]
    steps = [achieved_L[i] - achieved_L[i + 1] for i in range(15)]  # decreasing L* as exposure rises
    mean_step = sum(steps) / len(steps)
    for s in steps:
        assert abs(s - mean_step) < 0.25 * abs(mean_step) + 3.0, (
            f"L* step {s:.2f} deviates too far from mean step {mean_step:.2f} -- "
            f"steps: {[round(x, 2) for x in steps]}"
        )
