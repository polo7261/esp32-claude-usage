# 調試 ESP32-S3 CDC-on-boot 與 WiFi 重連

本文檔記錄與本專案相關的 ESP32-S3 調試技巧。

## USB CDC on Boot 的序列埠行為

ESP32-S3 可配置為 USB CDC（虛擬序列埠）模式，由 MCU 本身的 USB 控制器直接模擬，而非透過獨立的 CH340/CP2102 等 UART 晶片。這帶來一個重要的差異：

| 場景 | 獨立 UART 晶片 | ESP32-S3 native CDC |
|---|---|---|
| 板子 reset 時 | 晶片不受影響；`/dev/cu.*` 續存 | MCU 一 reset，USB 控制器跟著 reset，**device 從 bus 上消失** |
| 枚舉時間 | 數百 ms | ~1–2 秒 |
| 開機訊息 | 完整擷取 | **MCU 開機早期的輸出會被丟棄**（enum 前的 write 沒有地方存） |

### 擷取開機訊息的方法

硬體重置後，韌體會等待 USB 主機附著，預留一個時間窗口（通常 6 秒）讓序列埠監聽程式連接。抓住這個窗口的方法：

**Step 1: 手工重置（不用 esptool）**
```bash
# 只用 cat 讀取，不送任何命令——避免觸發 esptool 的 DTR/RTS assert
stty -f /dev/cu.usbmodem* 115200 raw -echo 2>/dev/null
(cat /dev/cu.usbmodem* > /tmp/boot.log 2>&1 &)
sleep 25
pkill -f "cat /dev/cu.usbmodem*"
cat /tmp/boot.log
```

**Step 2: 若需要強制重置**
```bash
ESPTOOL=~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool
$ESPTOOL --port /dev/cu.usbmodem* --after hard-reset --no-stub chip-id

# 等待埠重新出現（最多 3 秒）
for i in $(seq 1 60); do [ -e /dev/cu.usbmodem* ] && break; sleep 0.2; done

# 立刻連接（在韌體的 6 秒等待窗口內）
stty -f /dev/cu.usbmodem* 115200 raw -echo 2>/dev/null
(cat /dev/cu.usbmodem* > /tmp/boot.log 2>&1 &)
sleep 25
pkill -f "cat /dev/cu.usbmodem*"
```

### 常見陷阱

- **`arduino-cli monitor` 掛住**：monitor 預設會 assert DTR/RTS 觸發重置，導致 USB device 消失；等埠重新枚舉時開機訊息已丟。改用上述 `cat` 方法。
- **fd 失效**：若在後台跑 `cat` 時板子 reset，舊 fd 會立刻失效；需要等埠消失後重新枚舉再開檔。上面的迴圈 (`for i in ...`) 正是在等這個。

## WiFi 重連邏輯的常見問題

### 症狀：掉線後永遠自癒不了

掉線 6–10 分鐘後仍未恢復，序列埠不斷輸出：
```
[WiFi] disconnected, reason=2
[WiFi] disconnected, reason=36
E (400835) wifi:sta is connecting, return error
[WiFi] disconnected, reason=2
```

### 根本原因

ESP-IDF 的 `WiFi.setAutoReconnect(true)` 會在背景自動重試，但若應用層也在 loop 裡呼叫 `WiFi.reconnect()`，就會發生衝突：

```cpp
// 不好的做法
if (WiFi.status() != WL_CONNECTED) {
  WiFi.reconnect();      // ← 打斷背景正在進行的握手
  delay(5000);           // ← 每 5 秒重複一次
}
```

每次呼叫 `WiFi.reconnect()` 都會先 disconnect 再 connect，等於**把正在進行的 WPA2 四次握手拆掉重來**。若握手本來需要 5 秒以上（RSSI 邊緣或雜訊高時），就會被 loop 每 5 秒的呼叫反覆掐掉，永遠無法完成。

### 診斷指紋

三個跡象表示發生了這個問題：

1. `reason=2`（AUTH_EXPIRE）與 `reason=36`（STA_LEAVING）交替出現
2. 看到 `E (...) wifi:sta is connecting, return error` 的 error log
3. RSSI 實際上很強（-30 dBm 以上），但就是連不上

### 正確的做法

讓系統 auto-reconnect 機制獨占重連職責，應用層只在**系統已經等了一段時間**後才介入，並且介入時用退避而非固定間隔：

```cpp
// 好的做法：兜底 + 退避，不阻塞畫面
static unsigned long lastReconnect = 0;
static unsigned long reconnectBackoff = 15000;  // 起始 15s

if (WiFi.status() != WL_CONNECTED) {
  if (millis() - lastReconnect > reconnectBackoff) {
    WiFi.reconnect();
    lastReconnect = millis();
    reconnectBackoff = min(reconnectBackoff * 2, 120000UL);  // 倍增到 120s
  }
} else {
  reconnectBackoff = 15000;  // 連上後歸零
}
```

### 觀察指標

修正後再次掉線時，正常的恢復序列應如下：

```
[WiFi] disconnected, reason=202    ← 一次 AUTH_FAIL（暫時性）
[WiFi] associated                  ← 立刻重試成功
[WiFi] got IP: 192.168.1.237
```

不應再看到 `reason=2/36` 迴圈或 `sta is connecting, return error`。

## mDNS 驗證

若要確認 `MDNS.queryHost()` 真的在解析（而非每次都 fallback 到靜態 IP），可用 `tcpdump` 在 Mac 上抓 UDP 5353 查詢：

```bash
# 需要 sudo，執行時拔插 ESP32 或按 reset
sudo tcpdump -i en4 -n -s0 udp port 5353 and host 192.168.1.237
```

若看到 ESP32 IP 發出的 `iMac-2.local` A record 查詢，就證明 mDNS 路徑生效。

## 參考

- ESP-IDF WiFi 事件碼：[esp_wifi_types.h](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types.h)
- Arduino-ESP32 WiFi API：[WiFi.h 文檔](https://github.com/espressif/arduino-esp32/tree/master/libraries/WiFi)
