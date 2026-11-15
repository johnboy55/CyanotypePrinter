#!/usr/bin/env python3
"""Calibration web app: the phone-friendly version of analyze_target.py.

Upload a photo of a developed target from a phone, tap the fiducials and
reference-card patches in the browser, and get the final LUT back -- saved on
this machine, downloadable, and optionally pushed straight to the printer's
ESP8266 (which hands it to the Mega to save on the SD card).

LAN-only by design: there is no authentication. Don't expose it to the internet.

Run:
    python web/app.py                     # http://<this-machine>:8080
    CALIB_DATA_DIR=/srv/calib python web/app.py --port 8080

Everything it keeps (sidecars, photos, LUTs, plots, settings) lives under
CALIB_DATA_DIR, default ~/cyanotype-calib.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

import numpy as np
from flask import Flask, abort, jsonify, request, send_from_directory
from PIL import Image, ImageOps

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from calib.analysis import FIDUCIAL_ORDER, AnalysisError, analyze, fiducial_centers_mm, write_diagnostic_plot  # noqa: E402
from calib.camera_response import CameraResponseError, load_reference_card  # noqa: E402
from calib.lut_format import format_lut  # noqa: E402
from calib.perspective import PerspectiveError, compute_homography, mm_to_photo_px  # noqa: E402
from calib.sidecar import SidecarError, load_sidecar, validate_sidecar  # noqa: E402

BUILTIN_CARDS = ["q14", "q13", "colorchecker_classic", "colorchecker_mini"]
# Long-edge cap for stored photos. Phone photos are 12-48MP; sampling only needs
# the patches to be tens of pixels across, and a smaller image keeps the
# browser canvas responsive on a phone.
PHOTO_MAX_EDGE = 3000
# The Mega shows the LUT name on a 21-char OLED line and stores it in a 24-byte
# buffer including ".txt", so names are capped to fit both.
LUT_NAME_MAX = 19
_NAME_RE = re.compile(r"[^A-Za-z0-9_-]+")
_ID_RE = re.compile(r"^[0-9a-f]{32}$")

STATIC_DIR = Path(__file__).resolve().parent / "static"
REPO_CALIB_DIR = Path(__file__).resolve().parent.parent


def safe_name(raw: str, max_len: int = 64) -> str:
    name = _NAME_RE.sub("_", raw.strip()).strip("_")
    return name[:max_len]


def create_app(data_dir: str | os.PathLike | None = None) -> Flask:
    data = Path(data_dir or os.environ.get("CALIB_DATA_DIR") or Path.home() / "cyanotype-calib")
    dirs = {k: data / k for k in ("sidecars", "photos", "luts", "plots", "cards")}
    for d in dirs.values():
        d.mkdir(parents=True, exist_ok=True)
    settings_path = data / "settings.json"

    app = Flask(__name__, static_folder=None)
    app.config["MAX_CONTENT_LENGTH"] = 60 * 1024 * 1024  # phone photos can be big

    # --- helpers -----------------------------------------------------------

    def load_settings() -> dict:
        try:
            return json.loads(settings_path.read_text())
        except (OSError, ValueError):
            return {}

    def save_settings(update: dict) -> None:
        s = load_settings()
        s.update(update)
        settings_path.write_text(json.dumps(s, indent=2))

    def sidecar_paths() -> dict[str, Path]:
        """Uploaded sidecars, plus any gen_target.py output sitting in the repo's
        Calibration/ folder (so a target generated on this machine is just there)."""
        out: dict[str, Path] = {}
        for p in sorted(REPO_CALIB_DIR.glob("*_sidecar.json")):
            out[p.name] = p
        for p in sorted(dirs["sidecars"].glob("*.json")):
            out[p.name] = p
        return out

    def card_from_request(card_name: str):
        if card_name in BUILTIN_CARDS:
            return load_reference_card(card_name)
        # Uploaded custom cards are stored as cards/<name>.csv and referenced as "custom:<name>".
        if card_name.startswith("custom:"):
            path = dirs["cards"] / (safe_name(card_name[7:]) + ".csv")
            if path.is_file():
                return load_reference_card("custom-csv", str(path))
        raise CameraResponseError(f"unknown reference card '{card_name}'")

    def card_options() -> list[dict]:
        out = []
        for name in BUILTIN_CARDS:
            out.append({"id": name, "labels": load_reference_card(name).labels})
        for p in sorted(dirs["cards"].glob("*.csv")):
            try:
                card = load_reference_card("custom-csv", str(p))
            except (CameraResponseError, OSError, ValueError, KeyError):
                continue
            out.append({"id": "custom:" + p.stem, "labels": card.labels})
        return out

    def photo_path(photo_id: str) -> Path:
        if not _ID_RE.match(photo_id or ""):
            abort(400, "bad photo id")
        p = dirs["photos"] / f"{photo_id}.jpg"
        if not p.is_file():
            abort(404, "photo not found")
        return p

    def get_sidecar(name: str) -> dict:
        path = sidecar_paths().get(name)
        if path is None:
            abort(400, f"unknown sidecar '{name}'")
        return load_sidecar(str(path))

    def lut_list() -> list[dict]:
        out = []
        for p in sorted(dirs["luts"].glob("*.txt"), key=lambda p: p.stat().st_mtime, reverse=True):
            out.append({
                "name": p.stem,
                "modified": datetime.datetime.fromtimestamp(p.stat().st_mtime).isoformat(timespec="seconds"),
                "has_plot": (dirs["plots"] / f"{p.stem}.png").is_file(),
            })
        return out

    def points(raw, n: int, what: str) -> list[tuple[float, float]]:
        try:
            pts = [(float(x), float(y)) for x, y in raw]
        except (TypeError, ValueError):
            abort(400, f"{what} must be a list of [x, y] pairs")
        if len(pts) != n:
            abort(400, f"expected {n} {what}, got {len(pts)}")
        return pts

    @app.errorhandler(400)
    @app.errorhandler(404)
    @app.errorhandler(413)
    def json_error(e):
        return jsonify(error=getattr(e, "description", str(e))), e.code

    # --- pages / static ----------------------------------------------------

    @app.get("/")
    def index():
        return send_from_directory(STATIC_DIR, "index.html")

    # --- options -----------------------------------------------------------

    @app.get("/api/options")
    def options():
        return jsonify(
            sidecars=list(sidecar_paths().keys()),
            cards=card_options(),
            luts=lut_list(),
            fiducial_order=list(FIDUCIAL_ORDER),
            printer_host=load_settings().get("printer_host", ""),
            lut_name_max=LUT_NAME_MAX,
        )

    @app.post("/api/sidecars")
    def upload_sidecar():
        f = request.files.get("file")
        if f is None or not f.filename:
            abort(400, "no file")
        try:
            d = json.load(f.stream)
            validate_sidecar(d)
        except (ValueError, SidecarError) as e:
            abort(400, f"not a valid sidecar: {e}")
        name = safe_name(Path(f.filename).stem) + ".json"
        (dirs["sidecars"] / name).write_text(json.dumps(d, indent=2))
        return jsonify(name=name)

    @app.post("/api/cards")
    def upload_card():
        f = request.files.get("file")
        if f is None or not f.filename:
            abort(400, "no file")
        stem = safe_name(Path(f.filename).stem) or "card"
        path = dirs["cards"] / f"{stem}.csv"
        f.save(path)
        try:
            card = load_reference_card("custom-csv", str(path))
        except (CameraResponseError, ValueError, KeyError) as e:
            path.unlink(missing_ok=True)
            abort(400, f"not a valid reference-card CSV: {e}")
        return jsonify(id="custom:" + stem, labels=card.labels)

    # --- photos ------------------------------------------------------------

    @app.post("/api/photos")
    def upload_photo():
        f = request.files.get("photo")
        if f is None:
            abort(400, "no photo")
        try:
            img = Image.open(f.stream)
            # Apply the EXIF rotation now, so the pixels the browser shows (and the
            # user taps on) are the same pixels the analysis samples.
            img = ImageOps.exif_transpose(img).convert("RGB")
        except Exception as e:  # PIL raises a zoo of types for bad uploads
            abort(400, f"could not read image: {e}")
        img.thumbnail((PHOTO_MAX_EDGE, PHOTO_MAX_EDGE), Image.LANCZOS)
        photo_id = uuid.uuid4().hex
        img.save(dirs["photos"] / f"{photo_id}.jpg", quality=95)
        return jsonify(id=photo_id, width=img.width, height=img.height, url=f"/api/photos/{photo_id}")

    @app.get("/api/photos/<photo_id>")
    def get_photo(photo_id):
        p = photo_path(photo_id)
        return send_from_directory(p.parent, p.name)

    # --- analysis ----------------------------------------------------------

    @app.post("/api/preview-grid")
    def preview_grid():
        """Where every patch lands in the photo given the tapped fiducials -- drawn
        over the photo so a mis-tap is obvious before the analysis runs."""
        body = request.get_json(force=True)
        sidecar = get_sidecar(body.get("sidecar", ""))
        fids = points(body.get("fiducials"), 4, "fiducial points")
        try:
            h = compute_homography(fiducial_centers_mm(sidecar), fids)
        except PerspectiveError as e:
            abort(400, str(e))
        quads = []
        for p in sidecar["patches"]:
            x, y, w, hh = p["x_mm"], p["y_mm"], p["w_mm"], p["h_mm"]
            quads.append([mm_to_photo_px(h, cx, cy) for cx, cy in ((x, y), (x + w, y), (x + w, y + hh), (x, y + hh))])
        return jsonify(quads=quads)

    @app.post("/api/analyze")
    def run_analysis():
        body = request.get_json(force=True)
        sidecar = get_sidecar(body.get("sidecar", ""))
        try:
            card = card_from_request(body.get("card", ""))
        except (CameraResponseError, ValueError, KeyError) as e:
            abort(400, str(e))
        fids = points(body.get("fiducials"), 4, "fiducial points")
        card_pts = points(body.get("card_points"), len(card.labels), "reference-card points")
        try:
            target_dmax = float(body.get("target_dmax", 1.8))
        except (TypeError, ValueError):
            abort(400, "target_dmax must be a number")
        mode = body.get("mode", "Lstar")
        if mode not in ("Lstar", "density"):
            abort(400, "mode must be Lstar or density")
        lut_name = safe_name(body.get("lut_name", ""), LUT_NAME_MAX)
        if not lut_name:
            lut_name = "lut_" + datetime.datetime.now().strftime("%m%d_%H%M")

        photo = np.asarray(Image.open(photo_path(body.get("photo_id", ""))).convert("RGB"), dtype=np.float64)
        try:
            result = analyze(photo, sidecar, card, fids, card_pts, target_dmax, mode)
        except AnalysisError as e:
            abort(400, str(e))

        text = format_lut(result.exposures)
        (dirs["luts"] / f"{lut_name}.txt").write_text(text + "\n", newline="\n")
        write_diagnostic_plot(str(dirs["plots"] / f"{lut_name}.png"), result)
        return jsonify(
            lut_name=lut_name,
            values=[round(v, 2) for v in result.exposures],
            text=text,
            warnings=result.warnings,
            plot_url=f"/api/plots/{lut_name}.png",
            download_url=f"/api/luts/{lut_name}.txt",
        )

    # --- LUTs --------------------------------------------------------------

    @app.get("/api/luts")
    def list_luts():
        return jsonify(luts=lut_list())

    @app.get("/api/luts/<name>.txt")
    def download_lut(name):
        name = safe_name(name, LUT_NAME_MAX)
        if not (dirs["luts"] / f"{name}.txt").is_file():
            abort(404, "LUT not found")
        return send_from_directory(dirs["luts"], f"{name}.txt", as_attachment=True, mimetype="text/plain")

    @app.get("/api/plots/<name>.png")
    def get_plot(name):
        name = safe_name(name, LUT_NAME_MAX)
        if not (dirs["plots"] / f"{name}.png").is_file():
            abort(404, "plot not found")
        return send_from_directory(dirs["plots"], f"{name}.png")

    @app.post("/api/send-to-printer")
    def send_to_printer():
        """Proxies the LUT to the ESP8266's POST /lut (server-side, so the phone's
        browser never has to talk cross-origin to the printer)."""
        body = request.get_json(force=True)
        name = safe_name(body.get("lut_name", ""), LUT_NAME_MAX)
        path = dirs["luts"] / f"{name}.txt"
        if not name or not path.is_file():
            abort(404, "LUT not found")
        host = (body.get("host") or "").strip()
        host = re.sub(r"^https?://", "", host).rstrip("/")
        if not host or not re.match(r"^[A-Za-z0-9.\-:]+$", host):
            abort(400, "enter the printer's address (shown on the Mega's ESP32 screen), e.g. 192.168.1.50")
        save_settings({"printer_host": host})

        url = f"http://{host}/lut?name={urllib.parse.quote(name)}"
        req = urllib.request.Request(url, data=path.read_bytes(), method="POST",
                                     headers={"Content-Type": "text/plain"})
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                reply = json.loads(resp.read() or b"{}")
        except urllib.error.HTTPError as e:
            try:
                reply = json.loads(e.read() or b"{}")
            except ValueError:
                reply = {}
            return jsonify(ok=False, error=reply.get("error") or f"printer returned HTTP {e.code}"), 502
        except (urllib.error.URLError, OSError, ValueError) as e:
            return jsonify(ok=False, error=f"could not reach printer at {host}: {e}"), 502
        return jsonify(ok=True, printer=reply)

    return app


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0", help="interface to listen on (default all)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--data-dir", help="where LUTs/photos/sidecars are kept (default $CALIB_DATA_DIR or ~/cyanotype-calib)")
    args = ap.parse_args(argv)

    app = create_app(args.data_dir)
    try:
        from waitress import serve
    except ImportError:
        print("waitress not installed -- falling back to Flask's development server", file=sys.stderr)
        app.run(host=args.host, port=args.port)
    else:
        print(f"serving on http://{args.host}:{args.port}")
        serve(app, host=args.host, port=args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
