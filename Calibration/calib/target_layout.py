"""Stage 1 core: build the step-wedge patch grid, fiducials, and early-DMAX
swatch as a pixel array, plus the metadata needed to fill in a Sidecar.

Byte encoding: the printer applies CFG.invertImage (uv_print.cpp:1069) before
looking up LUT16 (uv_sd.cpp:162-186 loads LUT16 as a plain linear ramp here,
LUT16[n] = emax * n/15). Because that ramp is linear, lutExposureFromByte's
piecewise interpolation collapses to a single linear map, exposure = emax *
v_eff/255, for ANY byte 0..255 -- not just multiples of 17 -- which is what
lets this tool encode more than 16 distinct exposures in one target. See
exposure_to_bmp_byte() below for the exact (and invert-aware) inversion.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass

from .bmp_writer import MAX_IMG_WIDTH
from .sidecar import EarlyDmaxSwatch, Fiducial, ImageInfo, Patch

# Background and fiducial byte values are NOT fixed literals -- they must go
# through exposure_to_bmp_byte() like every patch, because CFG.invertImage
# flips which raw byte means "no exposure" vs. "max exposure". Computed per
# call in build_target() from the actual `invert` argument.


class TargetLayoutError(ValueError):
    pass


def seed_lut_ramp(emax_s: float) -> list[float]:
    """The 16-entry linear seed LUT loaded on the printer before printing the
    target. Must be linear -- exposure_to_bmp_byte()'s inversion assumes it."""
    return [emax_s * n / 15 for n in range(16)]


def compute_seed_exposures(n_levels: int, emax_s: float, emin_frac: float = 1.0 / 64) -> list[float]:
    """n_levels sorted seed exposures in seconds; the first is exactly 0.0, the
    rest are clustered more densely near both the toe (low exposure, near
    emin) and the shoulder (high exposure, near emax) than plain log spacing
    would give, using Chebyshev-style node spacing on the log axis -- the
    toe and shoulder are where fog and Dmax nonlinearity actually live, and
    the well-behaved middle of the curve needs fewer points to characterize."""
    if n_levels < 8:
        raise TargetLayoutError(f"n_levels must be >= 8 for a usable curve fit, got {n_levels}")
    if emax_s <= 0:
        raise TargetLayoutError(f"emax_s must be > 0, got {emax_s}")
    emin_s = emax_s * emin_frac
    n_core = n_levels - 1  # minus the explicit 0.0 patch
    log_min, log_max = math.log(emin_s), math.log(emax_s)
    core = []
    for i in range(n_core):
        # Clustered near i=0 and i=n_core-1 (endpoints), sparser in the middle.
        u = -math.cos(math.pi * i / (n_core - 1)) if n_core > 1 else 0.0
        t = (u + 1) / 2  # 0..1
        core.append(math.exp(log_min + t * (log_max - log_min)))
    return [0.0] + core


def exposure_to_bmp_byte(exposure_s: float, emax_s: float, invert: bool) -> int:
    """Inverse of the firmware's byte -> exposure mapping for a linear seed LUT
    (see module docstring). Clamped to [0, emax_s] before conversion."""
    frac = max(0.0, min(1.0, exposure_s / emax_s))
    v_eff = round(255 * frac)
    return (255 - v_eff) if invert else v_eff


def _shuffle_no_adjacent_repeats(items: list, rng: random.Random, max_attempts: int = 200) -> list:
    """Shuffle so identical seed_exposure_s values (repeats of the same level)
    don't land next to each other in print order, where practical."""
    best = list(items)
    rng.shuffle(best)
    for _ in range(max_attempts):
        if all(best[i].seed_exposure_s != best[i + 1].seed_exposure_s for i in range(len(best) - 1)):
            return best
        rng.shuffle(best)
    return best  # give up gracefully -- a rare adjacency doesn't break the tool


@dataclass
class _PatchSpec:
    seed_exposure_s: float
    replicate: int


@dataclass
class TargetLayout:
    rows: list[list[int]]           # pixel array, rows[0] = top of image
    width_px: int
    height_px: int
    patches: list[Patch]
    fiducials: list[Fiducial]
    early_dmax_swatch: EarlyDmaxSwatch
    image_info: ImageInfo
    seed_lut_values: list[float]
    rng_seed: int


def build_target(
    *,
    emax_s: float = 3.0,
    mm_per_pixel: float = 2.0,
    n_levels: int = 24,
    repeats: int = 2,
    # 2x2 pixels at the default 2 mm/pixel: the smallest patch whose sampled center
    # (shrink=0.6 in perspective.sample_patch_mean) stays clear of UV spill from its
    # neighbours. Print time scales with width x height, so every mm here costs.
    patch_mm: float = 4.0,
    fiducial_mm: float = 4.0,
    dmax_swatch_mm: float = 4.0,
    gap_mm: float = 2.0,
    cols: int | None = None,
    invert: bool = True,
    rng_seed: int | None = None,
) -> TargetLayout:
    if rng_seed is None:
        rng_seed = random.randrange(2**31)
    rng = random.Random(rng_seed)

    seed_exposures = compute_seed_exposures(n_levels, emax_s)
    specs = [_PatchSpec(seed_exposure_s=e, replicate=r) for e in seed_exposures for r in range(repeats)]
    specs = _shuffle_no_adjacent_repeats(specs, rng)

    patch_px = max(1, round(patch_mm / mm_per_pixel))
    fiducial_px = max(1, round(fiducial_mm / mm_per_pixel))
    gap_px = max(0, round(gap_mm / mm_per_pixel))
    dmax_px = max(1, round(dmax_swatch_mm / mm_per_pixel))

    margin_px = fiducial_px + gap_px
    available_width_px = MAX_IMG_WIDTH - 2 * margin_px
    if available_width_px < patch_px:
        raise TargetLayoutError(
            f"patch_mm={patch_mm} leaves no room for the grid under MAX_IMG_WIDTH="
            f"{MAX_IMG_WIDTH}px ({MAX_IMG_WIDTH * mm_per_pixel:.0f}mm) once fiducial "
            f"margins are reserved -- reduce patch_mm or fiducial_mm"
        )
    max_cols = max(1, available_width_px // patch_px)
    total_patches = len(specs)
    if cols is None:
        # Roughly square grid: the sheet only needs to be as wide as the grid,
        # not the firmware's full MAX_IMG_WIDTH, which wastes media and time.
        cols = math.ceil(math.sqrt(total_patches))
    cols = max(1, min(cols, max_cols, total_patches))
    rows_n = math.ceil(total_patches / cols)

    grid_width_px = cols * patch_px
    width_px = grid_width_px + 2 * margin_px
    if width_px > MAX_IMG_WIDTH:
        raise TargetLayoutError(
            f"computed width {width_px}px exceeds MAX_IMG_WIDTH={MAX_IMG_WIDTH}px -- "
            f"reduce n_levels/repeats/patch_mm"
        )

    grid_height_px = rows_n * patch_px
    top_band_px = dmax_px + gap_px
    fid_band_px = fiducial_px + gap_px
    height_px = top_band_px + fid_band_px + grid_height_px + fid_band_px

    # Background = "no exposure" and fiducials = "max exposure", each resolved
    # through exposure_to_bmp_byte() so they're correct under either polarity
    # of CFG.invertImage, not just the (default) inverted one.
    background_byte = exposure_to_bmp_byte(0.0, emax_s, invert)
    dmax_byte = exposure_to_bmp_byte(emax_s, emax_s, invert)
    fiducial_byte = dmax_byte

    pixels = [[background_byte] * width_px for _ in range(height_px)]

    def fill_rect(x0: int, y0: int, w: int, h: int, value: int) -> None:
        for y in range(y0, min(y0 + h, height_px)):
            for x in range(x0, min(x0 + w, width_px)):
                pixels[y][x] = value

    # Early-DMAX swatch: topmost rows, full width -- printed first (see gen_target.py / README).
    fill_rect(0, 0, width_px, dmax_px, dmax_byte)

    grid_y0 = top_band_px + fid_band_px
    grid_x0 = margin_px

    # Fiducials: solid black squares at the four corners of the fiducial+grid block.
    fid_y_top = top_band_px
    fid_y_bottom = grid_y0 + grid_height_px + gap_px
    fiducials = [
        Fiducial(name="TL", x_mm=0.0, y_mm=fid_y_top * mm_per_pixel, size_mm=fiducial_mm),
        Fiducial(name="TR", x_mm=(width_px - fiducial_px) * mm_per_pixel, y_mm=fid_y_top * mm_per_pixel, size_mm=fiducial_mm),
        Fiducial(name="BL", x_mm=0.0, y_mm=fid_y_bottom * mm_per_pixel, size_mm=fiducial_mm),
        Fiducial(name="BR", x_mm=(width_px - fiducial_px) * mm_per_pixel, y_mm=fid_y_bottom * mm_per_pixel, size_mm=fiducial_mm),
    ]
    fill_rect(0, fid_y_top, fiducial_px, fiducial_px, fiducial_byte)
    fill_rect(width_px - fiducial_px, fid_y_top, fiducial_px, fiducial_px, fiducial_byte)
    fill_rect(0, fid_y_bottom, fiducial_px, fiducial_px, fiducial_byte)
    fill_rect(width_px - fiducial_px, fid_y_bottom, fiducial_px, fiducial_px, fiducial_byte)

    patches: list[Patch] = []
    for idx, spec in enumerate(specs):
        r, c = divmod(idx, cols)
        x_px = grid_x0 + c * patch_px
        y_px = grid_y0 + r * patch_px
        byte_value = exposure_to_bmp_byte(spec.seed_exposure_s, emax_s, invert)
        fill_rect(x_px, y_px, patch_px, patch_px, byte_value)
        patches.append(Patch(
            id=idx,
            seed_exposure_s=spec.seed_exposure_s,
            replicate=spec.replicate,
            byte_value=byte_value,
            x_mm=x_px * mm_per_pixel,
            y_mm=y_px * mm_per_pixel,
            w_mm=patch_px * mm_per_pixel,
            h_mm=patch_px * mm_per_pixel,
        ))

    return TargetLayout(
        rows=pixels,
        width_px=width_px,
        height_px=height_px,
        patches=patches,
        fiducials=fiducials,
        early_dmax_swatch=EarlyDmaxSwatch(row_start_px=0, row_end_px=dmax_px),
        image_info=ImageInfo(width_px=width_px, height_px=height_px, bottom_up=True),
        seed_lut_values=seed_lut_ramp(emax_s),
        rng_seed=rng_seed,
    )
