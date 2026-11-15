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

import numpy as np
from PIL import Image, ImageOps

from calib.analysis import FIDUCIAL_ORDER, AnalysisError, analyze, write_diagnostic_plot
from calib.camera_response import CameraResponseError, load_reference_card
from calib.lut_format import write_lut
from calib.perspective import pick_points
from calib.sidecar import load_sidecar


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
    # exif_transpose: phone JPEGs are often stored sideways with an EXIF rotation tag.
    photo = np.asarray(ImageOps.exif_transpose(Image.open(args.photo)).convert("RGB"), dtype=np.float64)

    try:
        card = load_reference_card(args.reference_card, args.reference_card_csv)
    except CameraResponseError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print("Click the 4 target fiducials in order: " + ", ".join(FIDUCIAL_ORDER))
    fiducial_photo_px = pick_points(photo / 255.0, 4, "Click fiducials: TL, TR, BL, BR")
    print(f"Click the {len(card.labels)} reference-card patches in order: " + ", ".join(card.labels))
    card_points = pick_points(photo / 255.0, len(card.labels), f"Click reference card patches: {', '.join(card.labels)}")

    try:
        result = analyze(photo, sidecar, card, fiducial_photo_px, card_points, args.target_dmax, args.mode)
    except AnalysisError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for w in result.warnings:
        print(f"warning: {w}", file=sys.stderr)

    write_lut(args.out_lut, result.exposures)
    print(f"wrote {args.out_lut}")

    if args.diagnostic_plot:
        write_diagnostic_plot(args.diagnostic_plot, result)
        print(f"wrote {args.diagnostic_plot}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
