import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from calib.curve_fit import LevelMeasurement, check_dmax_reached, check_monotonic, combine_repeats, fit_curve


def test_combine_repeats_rejects_outlier():
    data = {0.5: [0.40, 0.41, 0.39, 1.50]}  # one wild outlier
    out = combine_repeats(data)
    assert len(out) == 1
    assert abs(out[0].density - 0.40) < 0.05
    assert out[0].n_kept == 3


def test_combine_repeats_low_confidence_flag():
    data = {0.5: [0.40]}
    out = combine_repeats(data)
    assert out[0].low_confidence is True


def test_check_dmax_reached_flags_still_rising_curve():
    # Linear curve all the way to the end -- no shoulder -- should warn.
    levels = [LevelMeasurement(exposure_s=e, density=e, n_kept=3, n_total=3, low_confidence=False)
              for e in [0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0]]
    assert check_dmax_reached(levels) is not None


def test_check_dmax_reached_ok_with_shoulder():
    # Rises then flattens near the top -- a real shoulder.
    xs = [0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0]
    ys = [0.0, 0.5, 0.9, 1.2, 1.38, 1.40, 1.41]
    levels = [LevelMeasurement(exposure_s=x, density=y, n_kept=3, n_total=3, low_confidence=False)
              for x, y in zip(xs, ys)]
    assert check_dmax_reached(levels) is None


def test_check_monotonic_drops_reversal():
    xs = [0.0, 0.5, 1.0, 1.5, 2.0]
    ys = [0.0, 0.5, 1.0, 0.6, 1.5]  # dip at x=1.5
    levels = [LevelMeasurement(exposure_s=x, density=y, n_kept=3, n_total=3, low_confidence=False)
              for x, y in zip(xs, ys)]
    kept, warnings = check_monotonic(levels)
    assert len(warnings) == 1
    assert [lv.exposure_s for lv in kept] == [0.0, 0.5, 1.0, 2.0]


def test_fit_curve_interpolates_measured_points():
    xs = [0.0, 1.0, 2.0, 3.0]
    ys = [0.0, 0.5, 1.0, 1.4]
    levels = [LevelMeasurement(exposure_s=x, density=y, n_kept=3, n_total=3, low_confidence=False)
              for x, y in zip(xs, ys)]
    curve = fit_curve(levels)
    for x, y in zip(xs, ys):
        assert abs(float(curve(x)) - y) < 1e-9
