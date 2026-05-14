import argparse
import time

import cv2
import numpy as np
from flask import Flask, jsonify, request


app = Flask(__name__)
face_cascade = cv2.CascadeClassifier(
    cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
)
if face_cascade.empty():
    raise RuntimeError("failed to load OpenCV Haar cascade")


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


@app.get("/health")
def health():
    return jsonify({"ok": True, "time": time.time()})


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

    small_w = 160
    small_h = max(1, int(height * small_w / width))
    small = cv2.resize(bgr, (small_w, small_h), interpolation=cv2.INTER_AREA)
    gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
    gray = cv2.equalizeHist(gray)
    faces = face_cascade.detectMultiScale(
        gray,
        scaleFactor=1.08,
        minNeighbors=5,
        minSize=(22, 22),
    )

    result = []
    scale_x = width / small_w
    scale_y = height / small_h
    for x, y, w, h in faces[:5]:
        result.append(
            {
                "x1": int(x * scale_x),
                "y1": int(y * scale_y),
                "x2": int((x + w) * scale_x),
                "y2": int((y + h) * scale_y),
                "score": 1.0,
            }
        )

    return jsonify({"faces": result, "count": len(result)})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    app.run(host=args.host, port=args.port, threaded=True)


if __name__ == "__main__":
    main()
