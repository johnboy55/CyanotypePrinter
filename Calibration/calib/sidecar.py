"""JSON sidecar written by gen_target.py and consumed by analyze_target.py.

The sidecar is the single source of truth for target geometry -- analyze_target.py
never re-derives or guesses patch positions, exposures, or byte values from the
BMP or from layout formulas; it only ever reads them from here.
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field

SIDECAR_VERSION = 1


class SidecarError(ValueError):
    pass


@dataclass
class Fiducial:
    name: str
    x_mm: float
    y_mm: float
    size_mm: float


@dataclass
class Patch:
    id: int
    seed_exposure_s: float
    replicate: int
    byte_value: int
    x_mm: float
    y_mm: float
    w_mm: float
    h_mm: float


@dataclass
class EarlyDmaxSwatch:
    row_start_px: int
    row_end_px: int
    note: str = "printed in the first slices of the job -- inspect this strip before the rest of the target finishes"


@dataclass
class ImageInfo:
    width_px: int
    height_px: int
    bottom_up: bool = True


@dataclass
class Sidecar:
    created: str
    mm_per_pixel: float
    emax_s: float
    seed_lut_file: str
    seed_lut_values: list[float]
    invert_image: bool
    image: ImageInfo
    early_dmax_swatch: EarlyDmaxSwatch
    fiducials: list[Fiducial]
    patches: list[Patch]
    rng_seed: int
    version: int = SIDECAR_VERSION

    def to_dict(self) -> dict:
        return asdict(self)


def save_sidecar(path: str, sidecar: Sidecar) -> None:
    with open(path, "w") as f:
        json.dump(sidecar.to_dict(), f, indent=2)
        f.write("\n")


_REQUIRED_KEYS = (
    "version", "created", "mm_per_pixel", "emax_s", "seed_lut_file",
    "seed_lut_values", "invert_image", "image", "early_dmax_swatch",
    "fiducials", "patches", "rng_seed",
)


def load_sidecar(path: str) -> dict:
    with open(path, "r") as f:
        d = json.load(f)
    validate_sidecar(d)
    return d


def validate_sidecar(d: dict) -> None:
    missing = [k for k in _REQUIRED_KEYS if k not in d]
    if missing:
        raise SidecarError(f"sidecar missing required keys: {missing}")
    if d["version"] != SIDECAR_VERSION:
        raise SidecarError(f"unsupported sidecar version {d['version']} (expected {SIDECAR_VERSION})")
    if len(d["seed_lut_values"]) != 16:
        raise SidecarError("seed_lut_values must have exactly 16 entries")
    if len(d["fiducials"]) != 4:
        raise SidecarError(f"expected 4 fiducials, found {len(d['fiducials'])}")
    if not d["patches"]:
        raise SidecarError("sidecar has no patches")
    seen_ids = set()
    for p in d["patches"]:
        if p["id"] in seen_ids:
            raise SidecarError(f"duplicate patch id {p['id']}")
        seen_ids.add(p["id"])
