# Quick Start Guide — LADET RX-to-Website Integration

## 🚀 Setup (5 minutes)

### 1. Configure RX Firmware
Edit `firmware/rx-mqtt-publisher/rx-mqtt-publisher.ino`:

```cpp
// Line 26-27: WiFi credentials
const char* WIFI_SSID = "YourWiFiName";
const char* WIFI_PASS = "YourPassword";

// Line 30: MQTT broker (must match website)
const char* MQTT_SERVER = "broker.hivemq.com";

// Line 41: Test distance
const int TEST_DISTANCE_M = 100;  // Change to 300 or 700 for other tests
```

### 2. Install Arduino Libraries
Tools → Manage Libraries → Search and install:
- **PubSubClient** (Nick O'Leary)
- **ArduinoJson** (v6.x)
- **LoRa** (Sandeep Mistry)

### 3. Upload to ESP32
- Board: **ESP32 Dev Module**
- Upload Speed: 115200
- Click Upload (→)

### 4. Verify Website Config
Check `.env.local`:
```bash
NEXT_PUBLIC_MQTT_BROKER_URL=wss://broker.hivemq.com:8884/mqtt
NEXT_PUBLIC_DEMO_MODE=false  # Must be false!
```

## ✅ Testing Checklist

### Hardware
- [ ] ESP32 RX powered on
- [ ] Serial Monitor shows "✅ RX Gateway siap!"
- [ ] WiFi connected (IP displayed)
- [ ] MQTT connected (no errors)
- [ ] TX node powered on and transmitting

### RX Serial Monitor Output
Expected logs:
```
═══════════════════════════════════════
📨 PAKET LORA DITERIMA
═══════════════════════════════════════
Size: 215 bytes
RSSI: -67 dBm
SNR:  9.50 dB

Raw packet:
TYPE=DATA,SEQ=1,TIME=12:34:56,...

───────────────────────────────────────
📤 PUBLISH MQTT
───────────────────────────────────────
Topic: ladet/data
Size:  387 bytes

JSON Payload:
{
  "type": "DATA",
  "packet_id": 1,
  "sensor": {...},
  "radio": {...},
  "test": {...}
}

✅ MQTT publish berhasil!
```

### Website Dashboard
- [ ] Run `npm run dev` or access deployed URL
- [ ] Navigate to `/dashboard`
- [ ] Wait 5-10 seconds for MQTT connection
- [ ] Cards update with live values
- [ ] Charts populate with data points
- [ ] Status color changes (green/yellow/orange/red)
- [ ] Units show **"Δg"** (NOT "Hz")

### MQTT Broker Verification
Use [MQTT Explorer](http://mqtt-explorer.com/):
1. Connect to `broker.hivemq.com:1883`
2. Subscribe to `ladet/data`
3. Verify JSON messages arriving every few seconds

## 📊 Expected Values

| Sensor | Typical Range | Unit | Status Trigger |
|--------|---------------|------|----------------|
| Kelembaban Tanah | 0-100 | % | >60% = warning |
| Getaran | 0-5 | Δg | >0.5 = caution |
| Kemiringan | 0-45 | ° | >15° = danger |
| RSSI | -120 to -40 | dBm | <-90 = weak |
| SNR | -5 to 20 | dB | <0 = poor |

## 🔧 Common Issues

### ❌ "WiFi gagal terhubung"
**Fix**: Check SSID/password, ensure 2.4 GHz (not 5 GHz)

### ❌ "MQTT gagal, rc=-2"
**Fix**: Check internet connection, try `test.mosquitto.org`

### 📡 No packets received
**Fix**: 
- Check TX is powered and transmitting
- Verify LoRa frequency matches (915 MHz)
- Reduce distance between TX and RX
- Check antenna connections

### 🌐 Website not updating
**Fix**:
1. Verify `NEXT_PUBLIC_DEMO_MODE=false`
2. Check broker URL matches firmware
3. Open browser console → look for MQTT errors
4. Clear cache and rebuild: `npm run build`

### 🔢 Units show "Hz" instead of "Δg"
**Fix**: 
- Hard refresh browser: Ctrl+Shift+R
- Rebuild: `npm run build && npm run dev`
- Verify `SensorCard.tsx` line 28: `unit: ' Δg'`

## 📝 Test Scenarios

### Test 1: Distance = 100m
```cpp
const int TEST_DISTANCE_M = 100;
```
Upload → Power TX 100m away → Verify RSSI > -80 dBm

### Test 2: Distance = 300m
```cpp
const int TEST_DISTANCE_M = 300;
```
Upload → Power TX 300m away → Verify SNR > 5 dB

### Test 3: Distance = 700m
```cpp
const int TEST_DISTANCE_M = 700;
```
Upload → Power TX 700m away → Check packet loss %

## 📞 Support

**Documentation**: See `firmware/rx-mqtt-publisher/README.md`  
**Architecture**: See `IMPLEMENTATION_SUMMARY.md`  
**Issue**: Check Serial Monitor + Browser Console logs

---

**Version**: 1.0  
**Last Updated**: 2026-09-15
