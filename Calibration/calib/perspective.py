"""Interactive corner/patch picking and perspective-corrected patch sampling.

v1 is deliberately manual (matplotlib ginput clicks) rather than automatic
fiducial/patch detection -- this repo has zero prior Python/CV tooling to
build on, and a human click is far more robust than a first-pass detector
against unknown lighting/paper/phone-camera combinations. Automatic detection
is a documented stretch goal once real photos exist to tune against.
"""
from __future__ import annotations

import numpy as np

try:
    import cv2
except ImportError:  # pragma: no cover - exercised only when cv2 truly missing
    cv2 = None


class PerspectiveError(ValueError):
    pass


def pick_points(image: np.ndarray, n: int, title: str) -> list[tuple[float, float]]:
    """Interactive: shows `image`, asks the user to click `n` points in order,
    returns their (x, y) pixel coordinates. Requires a display; not usable
    over a headless/SSH session without X forwarding."""
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots()
    ax.imshow(image)
    ax.set_title(title)
    pts = fig.ginput(n=n, timeout=0)
    plt.close(fig)
    if len(pts) != n:
        raise PerspectiveError(f"expected {n} clicks, got {len(pts)}")
    return pts


def to_grayscale(image: np.ndarray) -> np.ndarray:
    if image.ndim == 2:
        return image.astype(np.float64)
    r, g, b = image[..., 0], image[..., 1], image[..., 2]
    return 0.299 * r + 0.587 * g + 0.114 * b


def sample_point_mean(gray: np.ndarray, xy: tuple[float, float], radius_px: float = 8) -> float:
    x, y = xy
    h, w = gray.shape
    x0, x1 = max(0, int(x - radius_px)), min(w, int(x + radius_px) + 1)
    y0, y1 = max(0, int(y - radius_px)), min(h, int(y + radius_px) + 1)
    if x1 <= x0 or y1 <= y0:
        raise PerspectiveError(f"sample point {xy} falls outside the image")
    return float(np.mean(gray[y0:y1, x0:x1]))


def compute_homography(
    fiducial_mm: list[tuple[float, float]], fiducial_photo_px: list[tuple[float, float]]
) -> np.ndarray:
    """Maps sidecar mm-space coordinates -> photo pixel coordinates, fit from
    4 corresponding fiducial-center points (order: TL, TR, BL, BR -- must
    match the order the user was asked to click in)."""
    if cv2 is None:
        raise PerspectiveError("opencv-python is required for perspective correction")
    if len(fiducial_mm) != 4 or len(fiducial_photo_px) != 4:
        raise PerspectiveError("need exactly 4 fiducial correspondences")
    src = np.array(fiducial_mm, dtype=np.float32)
    dst = np.array(fiducial_photo_px, dtype=np.float32)
    h = cv2.getPerspectiveTransform(src, dst)

    # Sanity-check: the fit shouldn't wildly distort the known physical aspect
    # ratio of the fiducial rectangle -- catches corners clicked out of order
    # or badly mis-clicked.
    mm_w = np.linalg.norm(np.array(fiducial_mm[1]) - np.array(fiducial_mm[0]))
    mm_h = np.linalg.norm(np.array(fiducial_mm[2]) - np.array(fiducial_mm[0]))
    px_w = np.linalg.norm(np.array(fiducial_photo_px[1]) - np.array(fiducial_photo_px[0]))
    px_h = np.linalg.norm(np.array(fiducial_photo_px[2]) - np.array(fiducial_photo_px[0]))
    if mm_w <= 0 or mm_h <= 0 or px_w <= 0 or px_h <= 0:
        raise PerspectiveError("degenerate fiducial geometry (zero width/height)")
    mm_ratio = mm_w / mm_h
    px_ratio = px_w / px_h
    if not (0.3 < (px_ratio / mm_ratio) < 3.0):
        raise PerspectiveError(
            f"clicked fiducial aspect ratio ({px_ratio:.2f}) is wildly different from the "
            f"target's known aspect ratio ({mm_ratio:.2f}) -- corners likely mis-clicked "
            f"or clicked out of order (expected TL, TR, BL, BR)"
        )
    return h


def mm_to_photo_px(homography: np.ndarray, x_mm: float, y_mm: float) -> tuple[float, float]:
    if cv2 is None:
        raise PerspectiveError("opencv-python is required for perspective correction")
    pt = np.array([[[x_mm, y_mm]]], dtype=np.float32)
    out = cv2.perspectiveTransform(pt, homography)
    return float(out[0, 0, 0]), float(out[0, 0, 1])


def sample_patch_mean(
    gray: np.ndarray,
    homography: np.ndarray,
    x_mm: float,
    y_mm: float,
    w_mm: float,
    h_mm: float,
    shrink: float = 0.6,
) -> float:
    """Mean luma over the central `shrink` fraction of a patch, in photo
    pixel space, via a perspective-warped polygon mask (so a rotated/skewed
    photo still samples the right pixels rather than an axis-aligned box)."""
    if cv2 is None:
        raise PerspectiveError("opencv-python is required for perspective correction")
    cx, cy = x_mm + w_mm / 2, y_mm + h_mm / 2
    hw, hh = (w_mm * shrink) / 2, (h_mm * shrink) / 2
    corners_mm = [(cx - hw, cy - hh), (cx + hw, cy - hh), (cx + hw, cy + hh), (cx - hw, cy + hh)]
    corners_px = np.array([mm_to_photo_px(homography, x, y) for x, y in corners_mm], dtype=np.int32)

    mask = np.zeros(gray.shape[:2], dtype=np.uint8)
    cv2.fillConvexPoly(mask, corners_px, 1)
    if mask.sum() == 0:
        raise PerspectiveError(f"patch at ({x_mm},{y_mm})mm warped to zero pixels in the photo")
    return float(np.mean(gray[mask.astype(bool)]))
