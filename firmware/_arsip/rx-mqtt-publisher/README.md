# LADET RX Firmware — MQTT Publisher Gateway

ESP32 LoRa receiver yang menerima data sensor dari TX node dan mempublikasikan ke MQTT broker dalam format JSON nested.

## Hardware Requirements

- **ESP32** (DevKit v1 atau sejenisnya)
- **LoRa SX1278/SX1276** module (915 MHz / 868 MHz / 433 MHz)
- **WiFi** connection (untuk MQTT)

## Pin Configuration

**LADET Wiring (harus sama dengan TX):**

```cpp
LORA_SCK   = 18  // SPI Clock
LORA_MISO  = 19  // SPI MISO
LORA_MOSI  = 23  // SPI MOSI
LORA_SS    = 5   // Chip Select (NSS)
LORA_RST   = 14  // Reset
LORA_DIO0  = 4   // IRQ (PENTING: GPIO4, bukan GPIO2!)
```

**LoRa Parameters (harus match dengan TX):**

```cpp
Frequency  = 433 MHz
SF         = 7 / 9 / 12 (sesuai skenario, HARUS sama TX & RX)
Bandwidth  = 125 kHz
Coding Rate= 4/5
Sync Word  = 0x12
CRC        = Enabled
```

## Required Libraries

Install via Arduino IDE Library Manager:

1. **WiFi** (built-in ESP32)
2. **PubSubClient** by Nick O'Leary
3. **ArduinoJson** by Benoit Blanchon (v6.x)
4. **LoRa** by Sandeep Mistry

## Configuration

Edit constants di bagian atas `rx-mqtt-publisher.ino`:

```cpp
// WiFi credentials
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// MQTT broker
const char* MQTT_SERVER = "broker.hivemq.com";  // Atau broker lain
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC  = "ladet/data";

// LoRa frequency
#define LORA_FREQ = 433E6  // 433 MHz untuk LADET Indonesia

// LoRa Spreading Factor — HARUS SAMA DENGAN TX!
// Ganti sesuai skenario pengujian
#define LORA_SF = 7   // Atau 9, atau 12

// Test scenario
const int TEST_DISTANCE_M = 100;  // Ubah sesuai jarak pengujian (100/300/700/1000m)
```

**PENTING**: 
- **SF RX HARUS sama dengan SF TX** pada setiap skenario pengujian
- TX tidak bisa "auto-adjust" SF RX melalui LoRa
- Sebelum setiap test, pastikan TX dan RX di-set ke SF yang sama

## TX Packet Format (Input)

RX menerima paket LoRa dari TX dalam format **comma-delimited key=value**:

```
TYPE=DATA,SEQ=123,TIME=12:34:56,SOIL_RAW=2450,SOIL_MV=1225,THETA=0.35,THETAR=0.10,THETAS=0.45,SAT=0.71,G0=1.00,G=1.02,DG=0.02,TX=2.5,TY=-1.3,TZ=358.7,DTX=0.1,DTY=-0.2,DTZ=0.3,DTH=2.8,STATUS=AMAN,SF=9
```

**21 fields** (parser order-agnostic):
- `TYPE`, `SEQ`, `TIME`
- `SOIL_RAW`, `SOIL_MV`, `THETA`, `THETAR`, `THETAS`, `SAT`
- `G0`, `G`, `DG`
- `TX`, `TY`, `TZ`, `DTX`, `DTY`, `DTZ`, `DTH`
- `STATUS`, `SF`
- `MS` (millis TX, basis pengukuran delay), `RTT` (RTT paket sebelumnya, opsional)

> **Paket khusus `SEQ=0` = KALIBRASI.** TX mengirimnya paling awal tiap run. RX
> **meng-ACK-nya** (RTT-nya disertakan TX di paket #1 agar offset jam terkunci
> absolut sejak paket pertama), tetapi **tidak** mem-publish ke MQTT dan **tidak**
> menghitungnya dalam rx_count/packet loss. Data run = `#1..#50`, dan delay #1
> sudah rill (bukan 0 ms).

## Config Remote (web → RX → TX)

Dashboard mem-publish konfigurasi ke `ladet/config/sf` (`SF7|SF9|SF12`) dan
`ladet/config/distance` (angka meter) — keduanya **retained**. RX berlangganan
keduanya (`subscribe` di `reconnectMQTT`) dengan aturan aman:

1. Saat paket data masih mengalir (< 15 dtk sejak paket terakhir), config
   **diantri** — SF tidak pernah berubah di tengah run.
2. Kanal kosong → RX kirim `TYPE=CONF,SF=n,DIST=d` ke TX **di SF lama**.
3. TX (config-listen: jendela 10 dtk saat boot & selamanya setelah run
   selesai) membalas `ACK=CONF` di SF lama, **lalu** menerapkan SF baru.
4. Setelah ACK, RX menerapkan SF baru dan mem-publish status ke
   `ladet/config/status` — `state`: `applied | queued | rejected |
   tx_unconfirmed | idle`.

Jika TX tidak mengonfirmasi (mati/jauh), RX **tidak** mengubah SF-nya dan web
menampilkan peringatan — mencegah TX & RX desinkron. Karena `ladet/config/*`
retained, RX yang baru nyala otomatis menerima config terakhir dan menjalankan
handshake yang sama sebelum menerapkannya.

## Status Kehadiran Perangkat (supaya dashboard tidak "menebak")

- `ladet/rx/online` — RX mem-publish `"online"` (retained) saat konek ke broker.
  RX juga mendaftarkan **LWT (Last Will & Testament)**: kalau RX mati/listrik
  putus tanpa disconnect, **broker otomatis** mem-publish `"offline"` (retained).
- `ladet/tx/online` — RX menandai `"online"` begitu ada paket LoRa masuk, dan
  `"offline"` kalau 30 detik sunyi (TX mati / di luar jangkauan).

Dashboard menampilkan status asli ini di panel skenario — bukan sekadar status
koneksi browser↔broker yang tidak menjamin perangkat menyala.

## MQTT Output Format (Nested JSON)

RX mempublikasikan ke topic `ladet/data` dengan struktur nested:

```json
{
  "type": "DATA",
  "packet_id": 123,
  "timestamp": "12:34:56",
  "sensor": {
    "soil_saturation": 71.0,
    "g_initial": 1.00,
    "g": 1.02,
    "delta_g": 0.02,
    "tilt_x": 2.5,
    "tilt_y": -1.3,
    "tilt_z": 358.7,
    "delta_tilt_x": 0.1,
    "delta_tilt_y": -0.2,
    "delta_tilt_z": 0.3,
    "tilt_angle": 2.8
  },
  "status": "aman",
  "radio": {
    "sf": "SF9",
    "rssi": -67,
    "snr": 9.5
  },
  "test": {
    "distance_m": 100,
    "delay_ms": 42,
    "delay_source": "clock",
    "packet_loss": 2.0,
    "total_expected": 50,
    "total_received": 49,
    "rx_count": 49,
    "rx_ms": 128340,
    "seq_stuck": false
  }
}
```

### Field Mappings (TX → JSON)

| TX Field | JSON Path | Transformation | Notes |
|----------|-----------|----------------|-------|
| `SAT` | `sensor.soil_saturation` | `×100` (0-1 → 0-100%) | Saturasi tanah |
| `DG` | `sensor.delta_g` | Direct | **Delta akselerasi (g), BUKAN Hz!** |
| `DTH` | `sensor.tilt_angle` | Direct | Sudut kemiringan total |
| `STATUS` | `status` | `.toLowerCase()` | AMAN → aman |
| `SF` | `radio.sf` | `"SF" + value` | 9 → "SF9" |
| `LoRa.packetRssi()` | `radio.rssi` | Direct | Measured by RX |
| `LoRa.packetSnr()` | `radio.snr` | Direct | Measured by RX |
| `TEST_DISTANCE_M` | `test.distance_m` | Constant | From RX config |
| `MS` | `test.delay_ms` | `rx_millis - tx_millis` | Tx→RX delay, dikoreksi offset jam (lihat `firmware/DELAY_FIX.md`) |
| SEQ gap tracking | `test.packet_loss` | `((expected-received)/expected)×100` | Packet loss % from sequence gaps |
| — | `test.total_expected` | Counter | Total expected packets |
| — | `test.total_received` | Counter | Total received packets |
| — | `test.rx_count` | Counter (**tidak** di-reset tiap run) | Nomor paket yang diterima RX; dipakai dashboard sebagai kunci anti-duplikat supaya 1 paket = 1 baris tabel/grafik walau `SEQ` dari TX diulang |
| — | `test.rx_ms` | `millis()` RX | Waktu terima di sisi RX |
| — | `test.seq_stuck` | `true/false` | `true` = `SEQ` dari TX tidak maju ≥ 3 paket berturut-turut → `test.packet_loss` tidak valid; upload ulang `firmware/ladet-tx/ladet-tx.ino` |

## Compilation & Upload

1. Open `rx-mqtt-publisher.ino` in Arduino IDE
2. Select board: **ESP32 Dev Module**
3. Select port: (your ESP32 COM port)
4. Configure WiFi/MQTT credentials
5. Set `TEST_DISTANCE_M` for current test scenario
6. Verify (✓) → Upload (→)

## Serial Monitor Output

Expected log when receiving packet:

```
═══════════════════════════════════════════════════════════
  LADET RX — Gateway MQTT Publisher
═══════════════════════════════════════════════════════════

🌐 Menghubungkan ke WiFi: MyWiFi
✅ WiFi terhubung!
   IP: 192.168.1.100
📡 Menginisialisasi LoRa... ✅
   Frequency: 915.0 MHz
✅ RX Gateway siap!
📡 MQTT Topic: ladet/data
📍 Test Distance: 100 m

═══════════════════════════════════════════════════════════
📨 PAKET LORA DITERIMA
═══════════════════════════════════════════════════════════
Size: 215 bytes
RSSI: -67 dBm
SNR:  9.50 dB

Raw packet:
TYPE=DATA,SEQ=1,TIME=12:34:56,...,SF=9

───────────────────────────────────────────────────────────
📤 PUBLISH MQTT
───────────────────────────────────────────────────────────
Topic: ladet/data
Size:  387 bytes

JSON Payload:
{
  "type": "DATA",
  "packet_id": 1,
  ...
}

✅ MQTT publish berhasil!
═══════════════════════════════════════════════════════════
```

## Testing Checklist

- [ ] WiFi connection successful
- [ ] LoRa initialization OK
- [ ] MQTT broker connection established
- [ ] Receiving packets from TX
- [ ] RSSI/SNR values reasonable
- [ ] JSON structure correct (use [MQTT Explorer](http://mqtt-explorer.com/))
- [ ] Website dashboard updating in real-time
- [ ] All 21 TX fields parsed correctly

## Troubleshooting

### ❌ WiFi gagal terhubung
- Cek SSID dan password
- Pastikan router 2.4 GHz (ESP32 tidak support 5 GHz)

### ❌ LoRa init gagal
- Cek wiring (SS, RST, DIO0)
- Pastikan module power supply cukup (3.3V @ 100mA)

### ❌ MQTT gagal, rc=-2
- Broker unreachable → cek internet connection
- rc=-4 → connection timeout
- Coba broker lain: `test.mosquitto.org`

### 📡 RSSI terlalu rendah (< -100 dBm)
- Jarak terlalu jauh
- Obstacle menghalangi
- Coba naikkan SF di TX (SF12 = jarak lebih jauh)

### 🔄 Website tidak update
- Cek broker sama dengan yang di `.env.local`
- Cek topic: `ladet/data`
- Pastikan `NEXT_PUBLIC_DEMO_MODE=false`

## Website Integration

Website akan otomatis normalize nested JSON ke flat `LadetPayload` via `normalizePayload()` di `lib/mqtt.ts`.

**Backward compatible**: mendukung format lama (flat) dan baru (nested).

---

**Author**: LADET Team  
**Version**: 1.0  
**Last Updated**: 2026-09-15
