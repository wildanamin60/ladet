# 🔍 DEBUG GUIDE - TEST 0 (MQTT Not Updating)

**Date**: 2026-09-15  
**Issue**: Serial Monitor shows "MQTT connected" but dashboard doesn't update

---

## ✅ Perbaikan yang Sudah Dilakukan

1. **React Key Duplicate Fixed**
   - File: `components/PacketTable.tsx` line 61
   - Before: `key={row.id ?? i}`
   - After: `key={`${row.created_at}-${row.packet_id}-${i}`}`
   - Status: ✅ FIXED

2. **MQTT Debug Log Added**
   - File: `app/(admin)/dashboard/page.tsx` line 159
   - Added: `console.log('[MQTT] Message received:', topic, parsed);`
   - Purpose: Verify browser receives MQTT messages
   - Status: ✅ ADDED

---

## 🔧 Checklist Debugging

### 1️⃣ CEK SERIAL MONITOR ESP32 RX

Pastikan terlihat output seperti ini:

```
✅ WiFi connected!
   IP: 192.168.x.x

✅ MQTT broker connected!
   Server: broker.hivemq.com:1883
   Client: ladet-rx-xxxxxx

📡 Publishing to: ladet/data
{
  "type": "ladet-v1",
  "packet_id": 1,
  "timestamp": "2026-09-15T13:15:41Z",
  "sensor": {
    "soil_saturation": 46.76,
    "g_initial": 0.976,
    "g": 0.980,
    "delta_g": 0.004,
    "tilt_angle": 0.6
  },
  "status": "aman",
  "radio": {
    "sf": "SF7",
    "rssi": -95,
    "snr": 8.5
  },
  "test": {
    "distance_m": 100,
    "delay_ms": null
  }
}
✅ MQTT publish berhasil!
```

**❌ Jika TIDAK ada:**
- Cek line 50: `#define MQTT_TEST_MODE 1` harus uncommented
- Cek WiFi credentials (line 26-27)
- Cek koneksi internet

---

### 2️⃣ CEK BROWSER CONSOLE (F12)

Buka DevTools → Console tab, cari log ini:

```
[MQTT] Message received: ladet/data {
  node_id: "LADET-NODE-01",
  timestamp: 1726405641434,
  soil_saturation: 46.76,
  vibration_freq: 0.004,   // ← Ini dari delta_g
  tilt_angle: 0.6,
  rssi: -95,
  snr: 8.5,
  delay_ms: 0,
  packet_id: 1,
  spreading_factor: "SF7",
  distance_m: 100,
  level: "aman"
}
```

**✅ Jika ADA:**
- MQTT connection OK
- Parsing OK
- Cek apakah dashboard berubah (refresh jika perlu)

**❌ Jika TIDAK ADA:**
- Browser tidak menerima message dari broker
- Lanjut ke checklist #3

---

### 3️⃣ CEK .env.local

File: `D:\SKRIPSI\ladet-dashboard\.env.local`

```bash
# HARUS seperti ini:
NEXT_PUBLIC_MQTT_BROKER_URL=wss://broker.hivemq.com:8884/mqtt
NEXT_PUBLIC_DEMO_MODE=false

# BUKAN:
# NEXT_PUBLIC_DEMO_MODE=true  ← Ini akan pakai demo data!
```

**❌ Jika DEMO_MODE=true:**
1. Edit ke `false`
2. Restart `npm run dev`
3. Hard refresh browser (Ctrl+Shift+R)

---

### 4️⃣ CEK TOPIC MATCH

**ESP32 RX (firmware):**
```cpp
const char* MQTT_TOPIC = "ladet/data";  // Line 29
```

**Website (lib/mqtt.ts):**
```typescript
export const LADET_TOPICS = [
  'ladet/data',  // ← Line 41
  // ... other topics
] as const;
```

**❌ Jika TIDAK match:**
- Edit salah satu supaya sama
- Upload firmware baru ke ESP32
- Restart website

---

### 5️⃣ CEK NETWORK TAB (DevTools)

Buka DevTools → Network tab → Filter: `WS` (WebSocket)

**✅ Harus terlihat:**
```
wss://broker.hivemq.com:8884/mqtt
Status: 101 Switching Protocols
```

**❌ Jika connection failed:**
- Firewall blocking WebSocket
- Antivirus blocking port 8884
- ISP blocking HiveMQ

**Solusi:**
1. Disable firewall sementara
2. Coba browser lain (Chrome/Edge)
3. Coba hotspot HP (bypass ISP block)

---

### 6️⃣ RESTART FULL SYSTEM

Jika masih belum work, restart semuanya:

```powershell
# Terminal 1: Stop website
Ctrl+C

# Arduino IDE: Restart ESP32
- Tekan tombol RESET pada ESP32
- Atau upload ulang firmware

# Terminal 1: Start website lagi
npm run dev

# Browser:
- Hard refresh: Ctrl+Shift+R
- Atau: Clear cache + reload
```

---

## 🎯 Expected Result (TEST 0 Success)

### Serial Monitor ESP32:
```
✅ MQTT publish berhasil!  (setiap 5 detik)
```

### Browser Console:
```
[MQTT] Message received: ladet/data {...}  (setiap 5 detik)
```

### Dashboard:
- **Kelembaban**: 46.76 %
- **Getaran**: 0.004 Δg (bukan Hz!)
- **Kemiringan**: 0.6 °
- **Status**: 🟢 AMAN
- **MQTT**: 🟢 MQTT ON

### Tabel Data:
- Baris baru muncul setiap 5 detik
- Tidak ada duplicate key warning di console

---

## 🐛 Troubleshooting Common Issues

### Issue: "MQTT publish berhasil" tapi console tidak log
**Root cause**: Browser tidak subscribe ke topic

**Fix:**
```typescript
// Cek app/(admin)/dashboard/page.tsx line 153
client.on('connect', () => { 
  setConnected(true); 
  LADET_TOPICS.forEach(t => client.subscribe(t)); // ← Pastikan ini ada!
});
```

---

### Issue: Console log ada tapi dashboard tidak update
**Root cause**: `normalizePayload()` tidak dipanggil

**Fix:**
```typescript
// Cek app/(admin)/dashboard/page.tsx line 156-160
client.on('message', (topic, msg) => {
  const parsed = parsePayload(topic, msg);  // ← Ini memanggil normalizePayload()
  if (!parsed) return;
  console.log('[MQTT] Message received:', topic, parsed);
  setLive(prev => { const u = { ...prev, ...parsed } as LadetPayload; applyPayload(u); return u; });
});
```

---

### Issue: Data update tapi nilai salah (0 semua)
**Root cause**: JSON parsing error atau field name mismatch

**Check:**
1. Serial Monitor JSON harus nested: `sensor`, `radio`, `test`
2. Browser console parsed harus flat: `soil_saturation`, `vibration_freq`, dst
3. `normalizePayload()` mapping correct (line 102-122 di lib/mqtt.ts)

---

## 📤 Kirim Hasil untuk Verifikasi

Setelah TEST 0 berhasil, kirimkan:

1. **Screenshot Serial Monitor** (show JSON + "MQTT publish berhasil")
2. **Screenshot Browser Console** (show "[MQTT] Message received")
3. **Screenshot Dashboard** (show real-time data update)
4. **Konfirmasi nilai**:
   - Kelembaban: 46.76%
   - Getaran: 0.004 Δg
   - Kemiringan: 0.6°
   - Status: Aman

---

## ✅ Next Step: TEST 1

Setelah TEST 0 verified, lanjut ke TEST 1 (TX → LoRa → RX):

1. Comment `// #define MQTT_TEST_MODE 1` (line 50)
2. Set `LORA_SF` sesuai TX (line 47)
3. Upload firmware baru
4. Power TX + RX
5. Verify 21 fields parsed correctly

---

**Prepared by**: Kiro AI  
**Last update**: 2026-09-15  
**Status**: Ready for TEST 0 debugging
