# 📋 TEST 1 — LoRa Link Validation (TX → RX)

**Tanggal**: 2026-09-16
**Durasi**: 15-20 menit
**Hardware**: ESP32 TX + ESP32 RX, jarak 1-5 meter
**Tujuan**: Pastikan TX → LoRa → RX berfungsi, semua 21 field ter-parse benar

---

## 🏗️ STEP 1: Siapkan Hardware

- [ ] ESP32 TX + LoRa SX1278 terpasang
- [ ] ESP32 RX + LoRa SX1278 terpasang
- [ ] **Sensor TX:** ADXL345, BNO055, DS3231, SEN0308 (atau pakai `ladet-tx.ino` SIMULATED_MODE)
- [ ] Wiring LoRa **SAMA** di kedua ESP32:

```
LoRa Pin  →  ESP32 GPIO
─────────────────────────
SCK       →  GPIO 18
MISO      →  GPIO 19
MOSI      →  GPIO 25   ← Penting! Bukan 23!
NSS/SS    →  GPIO 5
RST       →  GPIO 14
DIO0      →  GPIO 4
```

- [ ] Kedua ESP32 terhubung ke PC via USB
- [ ] **TX dan RX diletakkan berdekatan** (1-5 meter)

---

## 💻 STEP 2: Upload Firmware TX

**Pilih salah satu:**

### Opsi A: Simulated (tanpa sensor fisik)
**File:** `firmware/ladet-tx/ladet-tx.ino`

- [ ] Line 28: `#define LORA_FREQ 433E6` (433 MHz)
- [ ] Line 29: `#define LORA_SF 7` (atau 9/12 sesuai test plan)
- [ ] Line 43: `#define SIMULATED_MODE 1` ← **aktif untuk testing tanpa sensor**

### Opsi B: Real (dengan sensor fisik)
**File:** firmware TX asli (yang sudah ada di Arduino IDE)

- [ ] Wiring sensor: I2C (SDA=21, SCL=22), Soil=GPIO34
- [ ] Kalibrasi soil dry/wet sesuai kondisi lapangan
- [ ] `#define LORA_SF 7` SAMA dengan RX
- [ ] `#define TOTAL_PACKETS 50`

**Upload:** Arduino IDE → Open `.ino` → Board: ESP32 Dev Module → Upload ▶️

---

## 💻 STEP 3: Upload Firmware RX

**File:** `firmware/rx-mqtt-publisher/rx-mqtt-publisher.ino`

- [ ] Line 26: `WIFI_SSID = "SSID_WIFI_ANDA"` ← **GANTI!**
- [ ] Line 27: `WIFI_PASS = "PASSWORD_WIFI_ANDA"` ← **GANTI!**
- [ ] Line 43: `#define LORA_FREQ 433E6` ← **SAMA dengan TX!**
- [ ] Line 47: `#define LORA_SF 7` ← **SAMA dengan TX!**
- [ ] Line 53: `// #define MQTT_TEST_MODE 1` ← **TETAP COMMENTED!**

**Upload:** Arduino IDE → Open `.ino` → Board: ESP32 Dev Module → Upload ▶️

---

## 🔌 STEP 4: Buka Serial Monitor

**Baud rate: 115200** untuk kedua ESP32

### Serial Monitor TX — Expected:
```
═══════════════════════════════════════════════════════════
  LADET TX — Sensor Transmitter
═══════════════════════════════════════════════════════════
  SF       : 7
  Freq     : 433.0 MHz
  Interval : 2000 ms
  Packets  : 50
  ACK Wait : 1000 ms

📡 Menginisialisasi LoRa... ✅
   Wiring: SCK=18 MISO=19 MOSI=25 NSS=5 RST=14 DIO0=4
📐 Kalibrasi baseline...
✅ Baseline: G0=1.0000 TiltX=0.00 TiltY=0.00 TiltZ=90.00

───────────────────────────────────────────────────────────
📤 PACKET #1 DIKIRIM (267 bytes)
TYPE=DATA,SEQ=1,TIME=...,...,SF=7
✅ LoRa TX berhasil!
───────────────────────────────────────────────────────────
⏳ Menunggu ACK dari RX...
📥 RX RESPONSE: ACK=1
✅ ACK diterima — RTT: 12.3 ms
```

- [ ] LoRa init ✅
- [ ] MOSI = GPIO25 (bukan 23!)
- [ ] Baseline terkalibrasi
- [ ] Packet terkirim, SEQ 1→50, berhenti di 50
- [ ] ACK diterima setiap paket
- [ ] RTT terukur

### Serial Monitor RX — Expected:
```
═══════════════════════════════════════════════════════════
  LADET RX — Gateway MQTT Publisher
═══════════════════════════════════════════════════════════
✅ WiFi terhubung!   IP: 192.168.x.x
✅ MQTT terhubung!
✅ RX Gateway siap!

═══════════════════════════════════════════════════════════
📨 PAKET LORA DITERIMA
Size: 267 bytes | RSSI: -45 dBm | SNR: 10.50 dB

Raw packet:
TYPE=DATA,SEQ=1,TIME=2026-09-16 10:30:05,...,SF=7

───────────────────────────────────────────────────────────
📤 PUBLISH MQTT
Topic: ladet/data
Size:  387 bytes
...
✅ MQTT publish berhasil!
───────────────────────────────────────────────────────────
📤 ACK dikirim: ACK=1
═══════════════════════════════════════════════════════════
```

- [ ] WiFi connected
- [ ] MQTT connected
- [ ] LoRa packet diterima
- [ ] RSSI dan SNR terbaca
- [ ] JSON terpublish ke MQTT `ladet/data`
- [ ] ACK=<seq> terkirim kembali ke TX

---

## ✅ STEP 5: Validasi 21 Field

Bandingkan **Raw packet** di RX Serial Monitor:

| # | Field     | Expected    | Match? |
|---|-----------|-------------|--------|
| 1 | TYPE      | DATA        | [ ]    |
| 2 | SEQ       | 1,2,3...    | [ ]    |
| 3 | TIME      | HH:MM:SS    | [ ]    |
| 4 | SOIL_RAW  | ~1910       | [ ]    |
| 5 | SOIL_MV   | ~1550       | [ ]    |
| 6 | THETA     | ~0.3636     | [ ]    |
| 7 | THETAR    | 0.1000      | [ ]    |
| 8 | THETAS    | 0.4500      | [ ]    |
| 9 | SAT       | ~0.4676     | [ ]    |
|10 | G0        | 1.0000      | [ ]    |
|11 | G         | ~1.0004     | [ ]    |
|12 | DG        | ~0.0004     | [ ]    |
|13 | TX        | ~0.00       | [ ]    |
|14 | TY        | ~0.00       | [ ]    |
|15 | TZ        | ~90.00      | [ ]    |
|16 | DTX       | ~0.00       | [ ]    |
|17 | DTY       | ~0.00       | [ ]    |
|18 | DTZ       | ~0.00       | [ ]    |
|19 | DTH       | ~0.60       | [ ]    |
|20 | STATUS    | AMAN        | [ ]    |
|21 | SF        | 7           | [ ]    |

> Catatan: Nilai simulasi berubah karena noise, tapi harus dalam range wajar

---

## 🌐 STEP 6: Verifikasi Website (opsional untuk TEST 1)

- [ ] Buka `http://localhost:3000/dashboard`
- [ ] Console (F12): `[MQTT] ✅ Client connected`
- [ ] Console: `[Dashboard] 📨 Message received`
- [ ] Status: **ONLINE** (hijau)
- [ ] Sensor cards menampilkan data

---

## 🔧 TROUBLESHOOTING

| Problem | Solusi |
|---------|--------|
| LoRa init gagal | Cek wiring GPIO5/14/4/25, solder, reset ESP32 |
| WiFi tidak connect | Cek SSID/password di Line 26-27 RX |
| MQTT publish gagal | Cek internet, test `broker.hivemq.com:1883` |
| RX tidak terima data | **SF harus SAMA**, frequency SAMA, dekatkan (<5m) |
| ACK TIMEOUT di TX | Pastikan RX sudah upload firmware terbaru (ada ACK send) |
| Website tidak update | Cek `.env.local` DEMO_MODE=false, restart dev server |

---

## 🎯 SUCCESS CRITERIA

| # | Checkpoint | Status |
|---|-----------|--------|
| 1 | LoRa init berhasil di TX dan RX | [ ] |
| 2 | TX mengirim 50 paket (SEQ 1→50) | [ ] |
| 3 | RX menerima semua paket dari TX | [ ] |
| 4 | RSSI dan SNR terbaca | [ ] |
| 5 | Semua 21 field ter-parse benar | [ ] |
| 6 | JSON terpublish ke MQTT `ladet/data` | [ ] |
| 7 | ACK dikirim RX → diterima TX (RTT terukur) | [ ] |
| 8 | TX berhenti otomatis di paket 50 | [ ] |
| 9 | Website menampilkan data (opsional) | [ ] |

**Semua ✅ = TEST 1 PASS → Lanjut ke TEST 2**

---

## ➡️ SETELAH TEST 1 PASS

Lanjut ke **TEST 2** (End-to-End):
1. Buka website dashboard (`localhost:3000/dashboard`)
2. Verifikasi data muncul di Dashboard
3. Bandingkan nilai TX Serial ↔ Website
4. Cek Home page juga menampilkan data sama
