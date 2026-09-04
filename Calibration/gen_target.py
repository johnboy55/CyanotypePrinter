#!/usr/bin/env python3
"""Stage 1: generate a photometric step-wedge calibration target.

Produces three files (<out-prefix>_target.bmp, _seed_lut.txt, _sidecar.json):
print the BMP after loading the seed LUT on the printer (Calib > Load LUT),
develop it, then hand the sidecar + a photo of the result to analyze_target.py.

Example:
    python gen_target.py --emax 3.0 --mm-per-pixel 2.0 --patches 24 --repeats 3 \\
        --patch-mm 12 --invert --check-against-cfg ../SDCard/config/uv.cfg \\
        --out-prefix vandyke_2026-09-04
"""
from __future__ import annotations

import argparse
import datetime
import sys

from calib.bmp_writer import write_grayscale_bmp
from calib.lut_format import write_lut
from calib.sidecar import EarlyDmaxSwatch, Fiducial, ImageInfo, Patch, Sidecar, save_sidecar
from calib.target_layout import TargetLayoutError, build_target


def parse_cfg(path: str) -> dict[str, str]:
    cfg: dict[str, str] = {}
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            cfg[key.strip()] = value.strip()
    return cfg


def check_against_cfg(cfg_path: str, mm_per_pixel: float, invert: bool) -> None:
    try:
        cfg = parse_cfg(cfg_path)
    except OSError as e:
        print(f"warning: could not read --check-against-cfg {cfg_path}: {e}", file=sys.stderr)
        return
    cfg_mm = cfg.get("mm_per_pixel")
    if cfg_mm is not None and abs(float(cfg_mm) - mm_per_pixel) > 1e-6:
        print(
            f"warning: --mm-per-pixel {mm_per_pixel} does not match {cfg_path}'s "
            f"mm_per_pixel={cfg_mm} -- the printed target's physical pixel pitch "
            f"will not match this tool's layout math",
            file=sys.stderr,
        )
    cfg_invert = cfg.get("invert_image")
    if cfg_invert is not None:
        cfg_invert_bool = cfg_invert.strip() == "1"
        if cfg_invert_bool != invert:
            print(
                f"warning: --invert={invert} does not match {cfg_path}'s "
                f"invert_image={cfg_invert} -- patch exposures will not match "
                f"what this tool computed",
                file=sys.stderr,
            )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emax", type=float, default=3.0, help="max exposure in seconds (default 3.0, matches SDCard/calibrations/3sec.txt)")
    ap.add_argument("--mm-per-pixel", type=float, default=2.0, help="physical pixel pitch in mm (default 2.0, matches uv.cfg)")
    ap.add_argument("--patches", type=int, default=24, dest="n_levels", help="number of distinct exposure levels to sample (default 24)")
    ap.add_argument("--repeats", type=int, default=3, help="repeats per exposure level, spread across the sheet (default 3)")
    ap.add_argument("--patch-mm", type=float, default=12.0, help="side length of each patch in mm (default 12.0)")
    ap.add_argument("--fiducial-mm", type=float, default=16.0, help="side length of each corner fiducial in mm (default 16.0)")
    ap.add_argument("--dmax-swatch-mm", type=float, default=30.0, help="height of the early-DMAX strip at the top of the target (default 30.0)")
    ap.add_argument("--invert", dest="invert", action="store_true", default=True, help="assume CFG.invertImage=1 (default, matches uv.cfg)")
    ap.add_argument("--no-invert", dest="invert", action="store_false", help="assume CFG.invertImage=0")
    ap.add_argument("--check-against-cfg", metavar="PATH", help="warn if --mm-per-pixel/--invert mismatch this uv.cfg-style file")
    ap.add_argument("--rng-seed", type=int, default=None, help="fix the patch-shuffle RNG seed (default: random, recorded in the sidecar either way)")
    ap.add_argument("--out-prefix", required=True, help="output filename prefix")
    args = ap.parse_args(argv)

    if args.check_against_cfg:
        check_against_cfg(args.check_against_cfg, args.mm_per_pixel, args.invert)

    try:
        layout = build_target(
            emax_s=args.emax,
            mm_per_pixel=args.mm_per_pixel,
            n_levels=args.n_levels,
            repeats=args.repeats,
            patch_mm=args.patch_mm,
            fiducial_mm=args.fiducial_mm,
            dmax_swatch_mm=args.dmax_swatch_mm,
            invert=args.invert,
            rng_seed=args.rng_seed,
        )
    except TargetLayoutError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    bmp_path = f"{args.out_prefix}_target.bmp"
    lut_path = f"{args.out_prefix}_seed_lut.txt"
    sidecar_path = f"{args.out_prefix}_sidecar.json"

    write_grayscale_bmp(bmp_path, layout.rows)
    write_lut(lut_path, layout.seed_lut_values)

    sidecar = Sidecar(
        created=datetime.datetime.now().isoformat(timespec="seconds"),
        mm_per_pixel=args.mm_per_pixel,
        emax_s=args.emax,
        seed_lut_file=lut_path,
        seed_lut_values=layout.seed_lut_values,
        invert_image=args.invert,
        image=layout.image_info,
        early_dmax_swatch=layout.early_dmax_swatch,
        fiducials=layout.fiducials,
        patches=layout.patches,
        rng_seed=layout.rng_seed,
    )
    save_sidecar(sidecar_path, sidecar)

    print(f"wrote {bmp_path} ({layout.width_px}x{layout.height_px}px, "
          f"{layout.width_px * args.mm_per_pixel:.0f}x{layout.height_px * args.mm_per_pixel:.0f}mm)")
    print(f"wrote {lut_path} -- load this on the printer via Calib > Load LUT BEFORE printing the target")
    print(f"wrote {sidecar_path} -- keep this next to the photo for analyze_target.py")
    print(f"{len(layout.patches)} patches across {args.n_levels} exposure levels x {args.repeats} repeats")
    print("early-DMAX strip is the top "
          f"{layout.early_dmax_swatch.row_end_px}px ({layout.early_dmax_swatch.row_end_px * args.mm_per_pixel:.0f}mm) "
          "of the target -- printed first, check it before the rest finishes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
