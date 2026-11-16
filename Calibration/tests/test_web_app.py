"""Web app end-to-end: render a synthetic photo of a printed target + reference
card, push it through upload -> preview-grid -> analyze -> download -> send,
with the printer's HTTP endpoint mocked out."""
import io
import json
import math
import os
import sys

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

flask = pytest.importorskip("flask")

from calib.analysis import fiducial_centers_mm  # noqa: E402
from calib.sidecar import Sidecar, save_sidecar  # noqa: E402
from calib.target_layout import build_target  # noqa: E402
from web.app import create_app  # noqa: E402

PX_PER_MM = 10
ORIGIN = 40  # px margin around the target in the fake photo
CARD_DENSITIES = [0.05, 0.35, 0.65, 0.95, 1.25, 1.55]


def true_density(e):
    return 0.05 + (1.6 - 0.05) * (1 - math.exp(-2.0 * e))


def luma(d):
    return 240.0 * (10 ** (-d)) ** 0.7


def to_px(x_mm, y_mm):
    return ORIGIN + x_mm * PX_PER_MM, ORIGIN + y_mm * PX_PER_MM


@pytest.fixture
def setup(tmp_path):
    layout = build_target(emax_s=3.0, rng_seed=7)
    sidecar = Sidecar(
        created="test", mm_per_pixel=2.0, emax_s=3.0, seed_lut_file="x", seed_lut_values=layout.seed_lut_values,
        invert_image=True, image=layout.image_info, early_dmax_swatch=layout.early_dmax_swatch,
        fiducials=layout.fiducials, patches=layout.patches, rng_seed=layout.rng_seed,
    )
    sidecar_path = tmp_path / "t_sidecar.json"
    save_sidecar(str(sidecar_path), sidecar)
    sc = json.loads(sidecar_path.read_text())

    w_mm, h_mm = layout.width_px * 2.0, layout.height_px * 2.0
    # Wide enough for the target or the row of card squares, whichever is wider.
    card_row_mm = len(CARD_DENSITIES) * 12.0
    W, H = int(2 * ORIGIN + max(w_mm, card_row_mm) * PX_PER_MM), int(2 * ORIGIN + (h_mm + 30) * PX_PER_MM)
    img = np.full((H, W), luma(true_density(0.0)))

    def fill(x_mm, y_mm, ww, hh, value):
        x0, y0 = to_px(x_mm, y_mm)
        img[int(y0):int(y0 + hh * PX_PER_MM), int(x0):int(x0 + ww * PX_PER_MM)] = value

    for f in sc["fiducials"]:
        fill(f["x_mm"], f["y_mm"], f["size_mm"], f["size_mm"], luma(true_density(3.0)))
    for p in sc["patches"]:
        fill(p["x_mm"], p["y_mm"], p["w_mm"], p["h_mm"], luma(true_density(p["seed_exposure_s"])))
    card_pts = []
    for i, d in enumerate(CARD_DENSITIES):
        x_mm, y_mm = i * 12.0, h_mm + 10
        fill(x_mm, y_mm, 10, 10, luma(d))
        card_pts.append(to_px(x_mm + 5, y_mm + 5))

    rgb = np.clip(np.stack([img] * 3, axis=-1), 0, 255).astype(np.uint8)
    photo = io.BytesIO()
    Image.fromarray(rgb).save(photo, format="PNG")
    photo.seek(0)

    card_csv = "label,density\n" + "".join(f"p{i},{d}\n" for i, d in enumerate(CARD_DENSITIES))
    fids = [to_px(x, y) for x, y in fiducial_centers_mm(sc)]

    app = create_app(tmp_path / "data")
    app.testing = True
    return app.test_client(), sidecar_path, photo, card_csv, fids, card_pts


def test_full_flow(setup, monkeypatch):
    client, sidecar_path, photo, card_csv, fids, card_pts = setup

    r = client.post("/api/sidecars", data={"file": (open(sidecar_path, "rb"), "t_sidecar.json")})
    assert r.status_code == 200, r.json
    sidecar_name = r.json["name"]

    r = client.post("/api/cards", data={"file": (io.BytesIO(card_csv.encode()), "synth.csv")})
    assert r.status_code == 200, r.json
    card_id = r.json["id"]

    r = client.post("/api/photos", data={"photo": (photo, "photo.png")})
    assert r.status_code == 200, r.json
    photo_id = r.json["id"]
    assert client.get(r.json["url"]).status_code == 200

    r = client.post("/api/preview-grid", json={"sidecar": sidecar_name, "fiducials": fids})
    assert r.status_code == 200, r.json
    assert len(r.json["quads"]) == 48

    r = client.post("/api/analyze", json={
        "photo_id": photo_id, "sidecar": sidecar_name, "card": card_id, "target_dmax": 1.5,
        "mode": "Lstar", "fiducials": fids, "card_points": card_pts, "lut_name": "synth test!",
    })
    assert r.status_code == 200, r.json
    res = r.json
    assert res["lut_name"] == "synth_test"
    vals = res["values"]
    assert len(vals) == 16 and vals[0] == 0.0 and vals == sorted(vals) and vals[-1] <= 3.0

    r = client.get(res["download_url"])
    assert r.status_code == 200
    assert r.data.decode().strip() == res["text"]
    assert client.get(res["plot_url"]).status_code == 200
    assert client.get("/api/luts").json["luts"][0]["name"] == "synth_test"

    sent = {}

    class FakeResp:
        def __enter__(self): return self
        def __exit__(self, *a): return False
        def read(self): return b'{"ok":true,"file":"synth_test.txt"}'

    def fake_urlopen(req, timeout):
        sent["url"], sent["body"] = req.full_url, req.data
        return FakeResp()

    monkeypatch.setattr("urllib.request.urlopen", fake_urlopen)
    r = client.post("/api/send-to-printer", json={"lut_name": "synth_test", "host": "http://192.168.1.50/"})
    assert r.status_code == 200, r.json
    assert sent["url"] == "http://192.168.1.50/lut?name=synth_test"
    assert sent["body"].decode().strip() == res["text"]
    assert client.get("/api/options").json["printer_host"] == "192.168.1.50"


def test_rejects_bad_input(setup):
    client = setup[0]
    assert client.get("/api/photos/../../etc").status_code in (400, 404)
    assert client.get("/api/luts/nope.txt").status_code == 404
    r = client.post("/api/send-to-printer", json={"lut_name": "nope", "host": "1.2.3.4"})
    assert r.status_code == 404
