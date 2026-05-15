# ESP32S3 + PC face recognition split demo

This folder is a standalone implementation that does not modify `who_weixue`.

Architecture:

- ESP32S3 keeps camera, LCD, touch and LVGL UI.
- ESP32S3 captures `240x240` RGB565 frames and sends them to the PC over HTTP.
- The PC runs `yolov8n-face` detection and returns JSON boxes.
- ESP32S3 draws the camera preview and returned face boxes locally.

## 1. Start the PC server

```powershell
cd remote_face_split\pc_server
python -m venv .venv
.\.venv\Scripts\pip install -r requirements.txt
.\.venv\Scripts\python face_server.py --host 0.0.0.0 --port 8080 --warmup
```

The first startup downloads `yolov8n-face` from Hugging Face into `pc_server\models\yolov8n-face.pt`.

Check the PC IP address:

```powershell
ipconfig
```

The ESP32S3 and PC must be on the same LAN. If Windows Firewall asks, allow Python to accept private network connections.

## 2. Configure and flash ESP32S3

```powershell
cd remote_face_split\esp32s3_client
idf.py set-target esp32s3
idf.py menuconfig
```

Set:

- `Remote Face Client -> WiFi SSID`
- `Remote Face Client -> WiFi password`
- `Remote Face Client -> PC detect URL`, for example `http://192.168.1.23:8080/detect`

Then build and flash:

```powershell
idf.py build flash monitor
```

## Protocol

ESP32S3 request:

- `POST /detect`
- Body: raw RGB565 little-endian frame
- Headers:
  - `Content-Type: application/octet-stream`
  - `X-Image-Width: 240`
  - `X-Image-Height: 240`
  - `X-Image-Format: RGB565`
  - `X-Image-Endian: BE`

PC response:

```json
{
  "faces": [
    {"x1": 10, "y1": 20, "x2": 100, "y2": 130, "score": 1.0}
  ],
  "count": 1
}
```

Coordinates are in the original `240x240` camera frame.
