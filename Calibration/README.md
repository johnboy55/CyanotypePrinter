# Photo-Based LUT Calibration

Derives a measured, perceptually-even 16-entry exposure LUT for the printer
(cyanotype, Van Dyke, or any other process) from a photograph of a printed
step-wedge target, without needing a working densitometer. Useful whenever
you switch process/paper/sensitizer batch, or suspect fog or a tired
developer is throwing off your exposure.

Output is a plain-text LUT file in the exact format the firmware already
loads (`MotorController_Mega/uv_sd.cpp:loadCalibrationFile`) -- no firmware
changes are involved anywhere in this tool.

## Setup

```
pip install -r requirements.txt
```

## Reference card

You need a physical reference card with **published, known reflection
density or reflectance values** photographed in the same frame as the
printed target. Recommended: a **Kodak/Tiffen Q-13 (8") or Q-14 (14") Gray
Scale** -- a 20-step reflection gray scale (~$25-45, Freestyle Photo, B&H,
Amazon) purpose-built for exactly this. Twenty known points gives a much
better curve fit than a plain white/grey/black card. An X-Rite/Calibrite
ColorChecker Classic or Mini also works (`--reference-card
colorchecker_classic` / `colorchecker_mini`) if you already have one, though
it only has 6 gray steps.

The density values baked into this tool for these presets are **nominal**,
not measured off your specific card -- real units vary batch to batch. If
your card came with (or you can find) its actual per-step values, put them
in a CSV (`label,density` or `label,reflectance_pct` columns) and use
`--reference-card custom-csv --reference-card-csv your_values.csv` for
better accuracy.

Photo capture tips:
- Manual/fixed exposure and white balance if your camera allows it (auto
  modes actively fight the correction step).
- Even, diffuse lighting -- no glare or hotspots on the print or the card.
- Camera as close to perpendicular to the page as practical.
- Print and card coplanar, both fully in frame.

## Workflow

**1. Generate a target:**

```
python gen_target.py --emax 3.0 --mm-per-pixel 2.0 --patches 24 --repeats 2 \
    --check-against-cfg ../SDCard/config/uv.cfg \
    --out-prefix vandyke_2026-09-04
```

Produces `vandyke_2026-09-04_target.bmp`, `_seed_lut.txt`, `_sidecar.json`.

**2. On the printer:** load `_seed_lut.txt` via `Calib > Load LUT`, confirm
with `Calib > View LUT`, then print `_target.bmp` and develop it as normal.

The topmost strip of the target (reported by `gen_target.py` on the console,
and recorded as `early_dmax_swatch` in the sidecar) is a solid full-exposure
swatch and is printed **first** -- check it as soon as it's exposed, before
the rest of the target finishes. If it doesn't look like it's heading toward
your process's real Dmax, stop: increase `--emax` and reprint rather than
spending time developing and photographing the whole target.

**3. Photograph** the developed target and the reference card together (see
tips above).

**4. Analyze:**

```
python analyze_target.py --photo IMG_1234.jpg \
    --sidecar vandyke_2026-09-04_sidecar.json \
    --reference-card q14 --target-dmax 1.8 --mode Lstar \
    --out-lut vandyke_2026-09-04_final.txt \
    --diagnostic-plot vandyke_2026-09-04_diag.png
```

You'll be asked to click the target's 4 fiducial corners, then each
reference-card patch, in the order printed to the console. Check console
warnings (Dmax not reached, non-monotonic/fogged response, low-confidence
exposure levels) and the diagnostic plot before trusting the output.

**5.** Copy `_final.txt` onto the SD card's `/calibrations/` folder and load
it via `Calib > Load LUT`, same as any other calibration file.

## Design notes

- Patch exposures are intentionally NOT a plain left-to-right ramp: each of
  the ~24 exposure levels is repeated 2x at shuffled, non-adjacent positions
  on the sheet, so a fogging gradient or uneven tray development doesn't bias
  any one level. `analyze_target.py` combines repeats with outlier rejection.
- Sampling is denser near the toe (low exposure) and shoulder (high exposure)
  than a plain linear or log ramp would give, since that's where fog and Dmax
  nonlinearity actually show up.
- The final LUT only has 16 entries, but the target samples more points than
  that (default 24) so the fitted curve -- and therefore the 16 chosen
  exposures -- is better characterized than a bare 16-point measurement would
  allow.

## Status / limitations

The math pipeline (`calib/camera_response.py`, `calib/curve_fit.py`,
`calib/lut_solve.py`) is validated end-to-end against a synthetic, known
curve (`tests/test_synthetic_pipeline.py`) -- run `pytest tests/` to check.
The photo-analysis path (`calib/perspective.py`: fiducial/card clicking,
perspective warp, patch sampling) has NOT yet been run against a real photo,
since it depends on the reference card being in hand. Expect to tune glare
handling and click-tolerance once you have real photos to test against.
