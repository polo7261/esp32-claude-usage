# Claude Usage Monitor（ESP32-S3 小螢幕）

用一片 Waveshare **ESP32-S3-LCD-1.47**（ST7789V2, 172×320）即時顯示 Claude Code 的用量：

- Session（5 小時窗）用量 % 與重置倒數
- Weekly 用量 % 與重置時間
- 有 model-scoped 週限額（如 Fable）時多畫一條 bar
- 換頁時有 Pac-Man 過場動畫 🟡👻

架構分兩半：

```
Mac (server.py, port 8266)  ←──區網──  ESP32 (claude_usage.ino)
  讀 macOS Keychain 的 Claude Code       每 60 秒 GET /usage
  OAuth token → 打 usage API             把 JSON 畫成進度條
  （180 秒快取，避免 429）
```

## 需求

- macOS 上裝好並登入過 Claude Code（token 存在 Keychain，`server.py` 直接讀，不用另外設定）
- Python 3（`server.py` 無第三方依賴）
- Arduino IDE + ESP32 board support（esp32 by Espressif）
- Arduino 函式庫：**LovyanGFX**、**ArduinoJson**

## 使用

1. Mac 端啟動 server：

   ```bash
   python3 server.py
   # Serving on http://0.0.0.0:8266/usage
   ```

2. 改 `firmware/claude_usage/claude_usage.ino` 開頭三個值：
   - `WIFI_SSID` / `WIFI_PASS`：你的 WiFi（2.4GHz）
   - `SERVER_URL`：跑 server.py 那台 Mac 的區網 IP（檔內 Diag 診斷段的 IP 也一併改）

3. Arduino IDE 選板子 **ESP32S3 Dev Module**，燒錄即可。

## 踩雷筆記

- usage endpoint 打太密會 429，server 端 180 秒快取 + ESP32 端 60 秒輪詢是實測安全值。
- 這片 1.47" 面板有 34px 的 X offset，直接轉 `lcd.setRotation()` 會花屏，橫向動畫是用 sprite `pushRotateZoom` 旋轉 90° 畫的。
- WiFi 訊號弱時 HTTP 偶發 -1（SYN 掉包），韌體內建一次快速重試 + 連線診斷輸出。
