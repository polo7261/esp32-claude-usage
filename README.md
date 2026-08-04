# Claude Usage Monitor（ESP32-S3 小螢幕）

用一片 Waveshare **ESP32-S3-LCD-1.47**（ST7789V2, 172×320）即時顯示 Claude Code 的用量：

- **第 1 頁 `Usage`**：Session（5 小時窗）、Weekly（7 天）、Credits 額度花費
- **第 2 頁 `Token`**：本週 token 依模型拆分（Opus 5 / Haiku 4.5 / …），
  上緣有這 5 小時窗的 token 數與 $/hr 燃燒率
- **之後**：帳號實際擁有的 model-scoped 週限額（Max 方案才有），每頁 3 條
- 每條 bar 各有自己的色系；≥75% 轉黃、≥90% 轉紅
- 每 30 秒 Pac-Man 過場動畫 🟡👻 並自動翻到下一頁（底部有頁碼點）

## 兩個資料來源

畫面上的數字來自**彼此獨立**的兩個來源，快取也各自獨立（一邊掛掉不影響另一邊）：

| | 來源 | 回答的問題 |
|---|---|---|
| **額度 %** | `api/oauth/usage`（Keychain OAuth token） | 方案額度用掉幾成 —— 會撞牆的那個 |
| **Token / $** | `ccusage`（讀本機 `~/.claude/projects/**/*.jsonl`） | 實際消耗多少 token，依模型拆開 |

> ⚠️ **ccusage 的金額不是帳單。** 那是「這些 token 照 API 定價值多少錢」的估值，
> 訂閱制不這樣收費。Credits 那條（來自 API 的 `spend`）才是真的會扣的額度。

> **為什麼需要第二個來源**：usage API 完全不回傳 token 數，而 model-scoped
> 週限額（`seven_day_opus` / Fable 那條）是 **Max 方案限定**。Pro 帳號那些欄位
> 一律 `null`，所以走 API 拿不到「各模型分別用了多少」。ccusage 讀本機轉錄檔，
> 不受方案限制。

> **model bar 不捏造數字**：`models[]` 是從 API 實際存在的桶掃出來的
> （`limits[].weekly_scoped` 加上非 null 的 `seven_day_<model>`）。
> 沒有就不畫那一頁。要預覽版面用 `FAKE_MODELS=1`，畫面會標橘色 `DEMO`。

## 需要另外裝

```bash
npm i -g ccusage      # Token 頁的資料來源；沒裝就只是少一頁，其餘照常運作
```

架構分兩半：

```
Mac (server.py, port 8266)  ←──區網──  ESP32 (claude_usage.ino)
  讀 macOS Keychain 的 Claude Code       每 60 秒 GET /usage
  OAuth token → 打 usage API             把 JSON 畫成進度條
  （180 秒快取，避免 429）
```

## 需求

- macOS 上裝好並登入過 Claude Code（token 存在 Keychain，`server.py` 直接讀，不用另外設定）
- Python 3（`server.py` 無必要的第三方依賴；若你的 Python 沒有可用的 CA 憑證庫，會自動改用 `certifi`）
- Arduino IDE + ESP32 board support（esp32 by Espressif）
- Arduino 函式庫：**LovyanGFX**、**ArduinoJson 7**（程式用 `JsonDocument`，v6 編不過）

## 使用

1. Mac 端啟動 server，`BIND_HOST` 設為 ESP32 連得到的那張網卡的 IP：

   ```bash
   BIND_HOST=192.168.1.97 python3 server.py
   # Serving on http://192.168.1.97:8266/usage
   ```

   不帶 `BIND_HOST` 只會綁 `127.0.0.1`：ESP32 連不到，但可以先用
   `curl http://127.0.0.1:8266/usage` 確認伺服器本身正常。

   這個 endpoint 沒有認證，所以刻意不預設綁 `0.0.0.0` —— 在持有公網 IP
   且防火牆關閉的機器上，那等於把它直接曝露到網際網路。真的需要時用
   `ALLOW_PUBLIC_BIND=1` 覆寫。

2. 建立 `secrets.h`（**已列入 `.gitignore`，不會被 commit**）：

   ```bash
   cd firmware/claude_usage
   cp secrets.h.example secrets.h
   ```

   填入四個值：

   ```cpp
   #define WIFI_SSID "your-ssid"        // 2.4GHz
   #define WIFI_PASS "your-password"
   #define SERVER_IP_OCTETS 192, 168, 1, 100   // 跑 server.py 那台的區網 IP
   #define SERVER_PORT 8266
   ```

   `.ino` 裡不再有任何 IP 字面值 —— fetch URL 和 Diag 探測都是從
   `SERVER_IP_OCTETS` 組出來的，改一處就好。

3. Arduino IDE 選板子 **ESP32S3 Dev Module**，燒錄即可。

## 踩雷筆記

- usage endpoint 打太密會 429，server 端 180 秒快取 + ESP32 端 60 秒輪詢是實測安全值。
- 這片 1.47" 面板有 34px 的 X offset，直接轉 `lcd.setRotation()` 會花屏，橫向動畫是用 sprite `pushRotateZoom` 旋轉 90° 畫的。
- WiFi 訊號弱時 HTTP 偶發 -1（SYN 掉包），韌體內建一次快速重試 + 連線診斷輸出。
