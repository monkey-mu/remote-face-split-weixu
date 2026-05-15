# ESP32S3 + 电脑端人脸检测分离方案

这是一个独立示例工程，不会修改原来的 `who_weixue` 目录。

## 方案说明

- ESP32S3 负责摄像头采集、LCD 显示、触摸和 LVGL UI。
- ESP32S3 将 `240x240` 的 RGB565 图像通过 HTTP 发送给电脑。
- 电脑端使用 `yolov8n-face` 检测人脸，并返回 JSON 格式的人脸框坐标。
- ESP32S3 收到坐标后，在本地屏幕预览画面上画框。

当前实现是“人脸检测”，也就是找到脸的位置；如果后续要识别具体是谁，还需要在电脑端继续接入人脸特征模型，比如 InsightFace。

## 1. 启动电脑端服务

在 Windows PowerShell 中运行：

```powershell
cd remote_face_split\pc_server
python -m venv .venv
.\.venv\Scripts\pip install -r requirements.txt
.\.venv\Scripts\python face_server.py --host 0.0.0.0 --port 8080 --warmup
```

第一次启动会从 Hugging Face 下载 `yolov8n-face` 权重到：

```text
pc_server\models\yolov8n-face.pt
```

查看电脑局域网 IP：

```powershell
ipconfig
```

ESP32S3 和电脑必须在同一个局域网。Windows 防火墙如果弹窗询问是否允许 Python 访问网络，请允许专用网络访问。

## 2. 配置并烧录 ESP32S3

在 WSL2 或你的 ESP-IDF 环境中运行：

```bash
cd remote-face-split-weixu/esp32s3_client
idf.py set-target esp32s3
idf.py menuconfig
```

进入 `Remote Face Client` 配置：

- `WiFi SSID`：你的 WiFi 名称
- `WiFi password`：你的 WiFi 密码
- `PC detect URL`：电脑端检测地址，例如 `http://192.168.1.23:8080/detect`
- `HTTP timeout ms`：建议保持 `10000`
- `Detect interval ms`：建议先用 `800`

然后编译、烧录、监视串口：

```bash
idf.py build flash monitor
```

如果屏幕文字出现彩色边缘、底色发蓝或颜色明显异常，请删除旧配置后重新配置：

```bash
rm -f sdkconfig sdkconfig.old
idf.py set-target esp32s3
idf.py menuconfig
```

本工程需要保持 `CONFIG_LV_COLOR_16_SWAP=y`，这和原 `who_weixue` 工程的 LVGL 颜色配置一致。

## 3. WSL2 同步代码

如果你通过 GitHub 同步：

```bash
cd ~/esp_projects
git clone https://github.com/monkey-mu/remote-face-split-weixu.git
cd remote-face-split-weixu/esp32s3_client
idf.py build
```

后续更新：

```bash
cd ~/esp_projects/remote-face-split-weixu
git pull
```

## 4. 通信协议

ESP32S3 请求：

- 请求方法：`POST /detect`
- 请求体：原始 RGB565 图像数据
- 图像尺寸：`240x240`
- 请求头：

```text
Content-Type: application/octet-stream
X-Image-Width: 240
X-Image-Height: 240
X-Image-Format: RGB565
X-Image-Endian: BE
```

电脑端响应：

```json
{
  "faces": [
    {"x1": 10, "y1": 20, "x2": 100, "y2": 130, "score": 0.92}
  ],
  "count": 1,
  "model": "yolov8n-face"
}
```

坐标基于原始 `240x240` 摄像头画面。

## 5. 常见问题

### 电脑端有 POST 200，但 ESP32S3 没有框

先确认 `PC detect URL` 使用的是电脑的局域网 IP，不要使用 `localhost`。

### 画面卡顿

当前 ESP32S3 预览和检测已经拆成两个任务：预览持续刷新，检测按间隔异步进行。若还觉得卡，可以把 `Detect interval ms` 调大，例如 `1200` 或 `1500`。

### 首次启动电脑端很慢

第一次会下载 YOLO 权重，这是正常的。下载完成后，后续会直接使用本地模型文件。
