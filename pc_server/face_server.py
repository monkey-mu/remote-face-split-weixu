import argparse
from pathlib import Path
import time

import numpy as np
from flask import Flask, jsonify, request
from huggingface_hub import hf_hub_download
from ultralytics import YOLO


app = Flask(__name__)
ROOT_DIR = Path(__file__).resolve().parent
MODEL_DIR = ROOT_DIR / "models"
MODEL_PATH = MODEL_DIR / "yolov8n-face.pt"
model = None


def rgb565_to_bgr(frame: bytes, width: int, height: int, endian: str) -> np.ndarray:
    expected = width * height * 2
    if len(frame) != expected:
        raise ValueError(f"expected {expected} bytes, got {len(frame)}")

    dtype = ">u2" if endian == "BE" else "<u2"
    raw = np.frombuffer(frame, dtype=dtype).reshape((height, width))
    r = ((raw >> 11) & 0x1F).astype(np.uint8)
    g = ((raw >> 5) & 0x3F).astype(np.uint8)
    b = (raw & 0x1F).astype(np.uint8)

    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return np.dstack((b, g, r))


def load_model() -> YOLO:
    global model
    if model is not None:
        return model

    MODEL_DIR.mkdir(exist_ok=True)
    if not MODEL_PATH.exists():
        downloaded = hf_hub_download(
            repo_id="deepghs/yolo-face",
            filename="yolov8n-face/model.pt",
            local_dir=MODEL_DIR,
            local_dir_use_symlinks=False,
        )
        Path(downloaded).replace(MODEL_PATH)

    model = YOLO(str(MODEL_PATH))
    return model


@app.get("/health")
def health():
    return jsonify({"ok": True, "model": "yolov8n-face", "time": time.time()})


@app.post("/detect")
def detect():
    try:
        width = int(request.headers.get("X-Image-Width", "240"))
        height = int(request.headers.get("X-Image-Height", "240"))
    except ValueError:
        return jsonify({"error": "invalid image size headers"}), 400

    if width <= 0 or height <= 0 or width * height > 640 * 480:
        return jsonify({"error": "image size out of range"}), 400

    fmt = request.headers.get("X-Image-Format", "RGB565").upper()
    endian = request.headers.get("X-Image-Endian", "BE").upper()
    if fmt != "RGB565":
        return jsonify({"error": f"unsupported format: {fmt}"}), 400
    if endian not in ("BE", "LE"):
        return jsonify({"error": f"unsupported endian: {endian}"}), 400

    try:
        bgr = rgb565_to_bgr(request.get_data(), width, height, endian)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400

    detector = load_model()
    results = detector.predict(
        source=bgr,
        imgsz=320,
        conf=0.35,
        iou=0.45,
        verbose=False,
    )

    result = []
    if results and results[0].boxes is not None:
        xyxy = results[0].boxes.xyxy.cpu().numpy()
        confs = results[0].boxes.conf.cpu().numpy()
        for box, score in list(zip(xyxy, confs))[:5]:
            x1, y1, x2, y2 = box
            result.append(
                {
                    "x1": max(0, min(width - 1, int(x1))),
                    "y1": max(0, min(height - 1, int(y1))),
                    "x2": max(0, min(width - 1, int(x2))),
                    "y2": max(0, min(height - 1, int(y2))),
                    "score": round(float(score), 4),
                }
            )

    return jsonify({"faces": result, "count": len(result), "model": "yolov8n-face"})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--warmup", action="store_true", help="load YOLO before serving")
    args = parser.parse_args()
    if args.warmup:
        load_model()
    app.run(host=args.host, port=args.port, threaded=True)


if __name__ == "__main__":
    main()
