# TEST 0 - MQTT Connectivity Verification

## Objective
Membuktikan jalur **RX → WiFi → MQTT → Website** berfungsi dengan benar menggunakan data synthetic, **tanpa ketergantungan pada LoRa atau TX**.

## Cara Mengaktifkan TEST 0

### 1. Edit Firmware
Buka `firmware/rx-mqtt-publisher/rx-mqtt-publisher.ino`

**Line 43-44**, uncomment:

```cpp
// TEST MODE — Uncomment untuk TEST 0 (MQTT only, no LoRa)
#define MQTT_TEST_MODE 1
```

### 2. Konfigurasi WiFi
**Line 26-27**:

```cpp
const char* WIFI_SSID = "NamaWiFiAnda";
const char* WIFI_PASS = "PasswordWiFi";
```

### 3. Upload ke ESP32
- Board: **ESP32 Dev Module**
- Upload Speed: 115200
- Click **Upload** (→)

---

## Expected Output

### Serial Monitor (115200 baud)

```
✅ WiFi terhubung!
   IP: 192.168.1.100
✅ MQTT terhubung!
⚠️  MODE: MQTT TEST (no LoRa) — Publishing synthetic data every 5s

═════════════════════════════════════════════════
🧪 TEST 0 — Publishing synthetic packet
═════════════════════════════════════════════════
Expected website values:
  Kelembaban Tanah: 46.76 %
  Getaran:          0.004 Δg
  Kemiringan:       0.6 °
  Status:           aman (green card)
  SF:               SF7
  RSSI:             -67 dBm
  SNR:              9.5 dB
═════════════════════════════════════════════════

📤 PUBLISH MQTT
Topic: ladet/data
Size: 387 bytes

JSON Payload:
{
  "packet_id": 999,
  "sensor": {
    "soil_saturation": 46.76,
    "delta_g": 0.004,
    "tilt_angle": 0.6,
    ...
  },
  "status": "aman",
  "radio": {"sf": "SF7", "rssi": -67, "snr": 9.5}
}

✅ MQTT publish berhasil!
```

### Website Dashboard

1. Start: `npm run dev`
2. Open: `http://localhost:3000/dashboard`
3. Wait 5-10 seconds

**Verify values**:
- Kelembaban Tanah: **46.76 %**
- Getaran: **0.004 Δg** (NOT Hz)
- Kemiringan: **0.6 °**
- Status: **aman** (green)
- RSSI: **-67 dBm**
- SNR: **9.5 dB**

---

## Success Criteria

✅ Serial: WiFi connected, MQTT connected, publishes every 5s
✅ Website: Cards update, values match exactly
✅ Units: Show "Δg" (not "Hz")
✅ No console errors

---

## After TEST 0 Success

### Disable Test Mode
1. Edit `rx-mqtt-publisher.ino`
2. **Line 43**, comment:
   ```cpp
   // #define MQTT_TEST_MODE 1
   ```
3. Re-upload

### Send untuk verifikasi:
- ✅ Serial Monitor log (2-3 publish cycles)
- ✅ Screenshot dashboard (all cards visible)
- ✅ Confirmation: "Semua nilai match"

---

**Next**: TEST 1 (TX → LoRa → RX)

**Duration**: 10-15 minutes
