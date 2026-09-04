#!/usr/bin/env python3
"""Stage 2: analyze a photo of a printed, developed calibration target
(alongside a reference card in the same frame) and produce the final LUT.

Interactive: you will be asked to click, in order:
  1. The target's 4 fiducial corners (TL, TR, BL, BR).
  2. The reference card's patches, in the order given by --reference-card
     (white -> black; the tool prints the expected label order before asking).

Example:
    python analyze_target.py --photo IMG_1234.jpg \\
        --sidecar vandyke_2026-09-04_sidecar.json \\
        --reference-card q14 --target-dmax 1.8 --mode Lstar \\
        --out-lut vandyke_2026-09-04_final.txt \\
        --diagnostic-plot vandyke_2026-09-04_diag.png
"""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict

import numpy as np
from PIL import Image

from calib.camera_response import CameraResponseError, fit_camera_response, load_reference_card
from calib.curve_fit import check_dmax_reached, check_monotonic, combine_repeats, fit_curve
from calib.lut_format import write_lut
from calib.lut_solve import LutSolveError, solve_16_exposures
from calib.perspective import (
    PerspectiveError,
    compute_homography,
    pick_points,
    sample_patch_mean,
    sample_point_mean,
    to_grayscale,
)
from calib.sidecar import load_sidecar

FIDUCIAL_ORDER = ("TL", "TR", "BL", "BR")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--photo", required=True, help="JPEG/PNG/TIFF photo of the developed target + reference card")
    ap.add_argument("--sidecar", required=True, help="sidecar JSON written by gen_target.py")
    ap.add_argument("--reference-card", required=True,
                     choices=["q13", "q14", "colorchecker_classic", "colorchecker_mini", "custom-csv"])
    ap.add_argument("--reference-card-csv", help="path to label,density (or label,reflectance_pct) CSV, for --reference-card custom-csv")
    ap.add_argument("--target-dmax", type=float, required=True, help="desired Dmax for the process (e.g. 1.8)")
    ap.add_argument("--mode", choices=["Lstar", "density"], default="Lstar",
                     help="equal-step domain for the 16 output exposures (default Lstar, perceptually uniform)")
    ap.add_argument("--out-lut", required=True, help="output LUT .txt path")
    ap.add_argument("--diagnostic-plot", help="optional diagnostic PNG path")
    args = ap.parse_args(argv)

    sidecar = load_sidecar(args.sidecar)
    photo = np.asarray(Image.open(args.photo).convert("RGB"), dtype=np.float64)
    gray = to_grayscale(photo)

    try:
        card = load_reference_card(args.reference_card, args.reference_card_csv)
    except CameraResponseError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print("Click the 4 target fiducials in order: " + ", ".join(FIDUCIAL_ORDER))
    fiducial_photo_px = pick_points(photo / 255.0, 4, "Click fiducials: TL, TR, BL, BR")
    fiducial_mm = []
    by_name = {f["name"]: f for f in sidecar["fiducials"]}
    for name in FIDUCIAL_ORDER:
        f = by_name[name]
        fiducial_mm.append((f["x_mm"] + f["size_mm"] / 2, f["y_mm"] + f["size_mm"] / 2))

    try:
        homography = compute_homography(fiducial_mm, fiducial_photo_px)
    except PerspectiveError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(f"Click the {len(card.labels)} reference-card patches in order: " + ", ".join(card.labels))
    card_points = pick_points(photo / 255.0, len(card.labels), f"Click reference card patches: {', '.join(card.labels)}")
    card_luma = [sample_point_mean(gray, p) for p in card_points]

    try:
        response_fn = fit_camera_response(card_luma, card)
    except CameraResponseError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    exposure_to_densities: dict[float, list[float]] = defaultdict(list)
    for patch in sidecar["patches"]:
        try:
            luma = sample_patch_mean(gray, homography, patch["x_mm"], patch["y_mm"], patch["w_mm"], patch["h_mm"])
        except PerspectiveError as e:
            print(f"warning: skipping patch {patch['id']}: {e}", file=sys.stderr)
            continue
        density = response_fn(luma)
        exposure_to_densities[patch["seed_exposure_s"]].append(density)

    levels = combine_repeats(exposure_to_densities)
    for lv in levels:
        if lv.low_confidence:
            print(f"warning: exposure {lv.exposure_s:.3f}s has low confidence "
                  f"({lv.n_kept}/{lv.n_total} repeats kept)", file=sys.stderr)

    dmax_warning = check_dmax_reached(levels)
    if dmax_warning:
        print(f"warning: {dmax_warning}", file=sys.stderr)

    levels, mono_warnings = check_monotonic(levels)
    for w in mono_warnings:
        print(f"warning: {w}", file=sys.stderr)

    try:
        curve = fit_curve(levels)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    dmin = levels[0].density
    exposure_domain = (levels[0].exposure_s, levels[-1].exposure_s)
    try:
        exposures, solve_warnings = solve_16_exposures(curve, exposure_domain, dmin, args.target_dmax, args.mode)
    except LutSolveError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for w in solve_warnings:
        print(f"warning: {w}", file=sys.stderr)

    write_lut(args.out_lut, exposures)
    print(f"wrote {args.out_lut}")

    if args.diagnostic_plot:
        _write_diagnostic_plot(args.diagnostic_plot, levels, curve, exposures, dmax_warning, mono_warnings)
        print(f"wrote {args.diagnostic_plot}")

    return 0


def _write_diagnostic_plot(path, levels, curve, exposures, dmax_warning, mono_warnings) -> None:
    import matplotlib.pyplot as plt

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
    if dmax_warning or mono_warnings:
        title += "  [see warnings]"
    ax.set_title(title)
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


if __name__ == "__main__":
    raise SystemExit(main())
