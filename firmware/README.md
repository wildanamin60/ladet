# LADET — Firmware Realtime (TX & RX)

Dua sketch, dua ESP32, satu link LoRa. Ini versi **lapangan/realtime**: ringan,
tanpa ACK, tanpa SEQ, tanpa handshake. Kalau butuh data pengujian QoS (RTT,
packet loss per SEQ, SF bisa diganti), itu ada di tier skripsi dan **bukan**
folder ini.

```
firmware/
├─ ladet-tx-realtime/     ← sensor node (tanah, getaran, kemiringan)
├─ ladet-rx-realtime/     ← gateway (LoRa → relay + LCD + MQTT + Telegram)
├─ tools/sensor-calibration/  ← sketch kalibrasi, bukan untuk dipasang permanen
├─ ladet-rx-skripsi/      ← tier skripsi, jangan dipakai untuk lapangan
└─ _arsip/                ← firmware lama, bukan untuk di-upload
```

## Radio (WAJIB sama di TX & RX)

| Parameter | Nilai |
|---|---|
| Frekuensi | 433 MHz |
| Spreading Factor | 7 (permanen, tidak bisa diganti dari web) |
| Bandwidth | 125 kHz |
| Coding Rate | 4/5 |
| Sync Word | 0x12 |
| CRC | aktif |
| TX power | 4 dBm (batas AMS1117, jangan dinaikkan) |

## TX — `ladet-tx-realtime/ladet-tx-realtime.ino`

ESP32 + LoRa SX1278 + ADXL345 + BNO055 + soil SEN0308.

| Fungsi | GPIO |
|---|---|
| LoRa SCK / MISO / MOSI | 18 / 19 / **25** |
| LoRa SS / RST / DIO0 | 5 / 14 / 4 |
| I2C SDA / SCL | 21 / 22 |
| Soil (analog) | 34 |
| Baterai (opsional, pembagi 2:1) | 35 |

- Kirim paket tiap **2 detik**, fire-and-forget, tanpa menunggu balasan.
- Tidak memakai mode tidur: paket harus tetap keluar tiap 2 detik.
- Telemetri baterai opsional lewat `BATTERY_MONITOR`. Butuh pembagi tegangan
  100k/100k dari baterai ke GPIO35; selama belum dipasang biarkan `0` supaya
  pin melayang tidak terbaca sebagai angka acak.
- Payload: `SAT=0.412,DG=0.031,DTH=0.42,STATUS=AMAN` (±45 byte).
- Baseline diambil saat boot: node **diam dan datar** 3 detik, 20 sampel.
- `DTH` = **selisih** kemiringan terhadap baseline, bukan sudut lereng absolut.
  Geser node secara datar = `0.00`. Miringkan = berubah. Ini benar.
- 15 detik pertama setelah boot, `DTH` dipaksa `0.00` (BNO055 belum stabil).
- Watchdog 15 detik → restart otomatis kalau loop macet.
- Radio dicek tiap loop; kalau chip keluar mode LoRa, di-init ulang sendiri.

Ambang (di firmware, sumber kebenaran status):

| Sensor | Ambang | Status |
|---|---|---|
| Soil | saturasi > 60 % | HATI-HATI |
| Getaran ADXL345 | Δg > 0.50 | WASPADA |
| Kemiringan BNO055 | Δθ > 5.0° | BAHAYA |

Status paket = yang paling berat di antara ketiganya.

## RX — `ladet-rx-realtime/ladet-rx-realtime.ino`

ESP32 + LoRa SX1278 + relay + LCD 20×4 + WiFi.

| Fungsi | GPIO |
|---|---|
| LoRa SCK / MISO / MOSI | 18 / 19 / **23** (GPIO25 dipakai relay merah) |
| LoRa SS / RST / DIO0 | 5 / 14 / 4 |
| Relay hijau (AMAN) | 27 |
| Relay kuning (HATI-HATI / WASPADA) | 26 |
| Relay merah (BAHAYA) | 25 |
| Buzzer (WASPADA / BAHAYA) | 33 |
| LCD SDA / SCL | 21 / 22 |

- **Listen-only.** RX tidak pernah memancar, jadi tidak ada lonjakan arus PA
  yang dulu bikin radio tuli setelah paket pertama.
- Aktuator dan LCD tetap jalan walau WiFi/MQTT mati.
- Paket tanpa field `STATUS` dibuang, tidak pernah ditampilkan sebagai AMAN.
- WiFi `sigapcare`, MQTT `broker.hivemq.com:1883`, topic `ladet/data`.
- LWT `ladet/rx/online`, heartbeat TX `ladet/tx/online` (offline jika 15 dtk sunyi).
- Telegram dikirim **dari RX**, bukan dari web. Token ada di `secrets.h`
  (tidak ikut Git) — salin dari `secrets.example.h`.
- Watchdog 15 detik, cek register radio tiap 1 detik, re-init jika tuli > 6 detik.
- OTA: hostname `ladet-rx`, sandi default `ladet-ota` (bisa diganti lewat
  `OTA_PASSWORD` di `secrets.h`). Dipakai kalau perlu update RX tanpa colok USB.
- Heartbeat: 1 baris ke tabel `ladet_heartbeat` tiap 24 jam dan sekali saat
  boot — isinya uptime, sisa heap, alasan reset, RSSI WiFi, umur paket terakhir.

### JSON yang dipublish ke `ladet/data`

```json
{
  "type": "DATA",
  "timestamp": "2026-11-26 18:42:11",
  "status": "aman",
  "sensor": { "soil_saturation": 42.5, "delta_g": 0.031, "tilt_angle": 0.42 },
  "radio":  { "sf": "SF7", "rssi": -67, "snr": 9.5 }
}
```

`soil_saturation` dalam persen, `delta_g` dalam g, `tilt_angle` dalam derajat
(delta terhadap baseline). `sensor.battery_v` hanya muncul kalau TX dikirim
dengan `BATTERY_MONITOR 1`.

### Peringatan terakhir (retained) — `ladet/alert/log`

Setiap kali level naik, RX mengirim **satu** peringatan terakhir sebagai
**retained message** ke `ladet/alert/log`. Riwayat lengkapnya disimpan di
Supabase oleh RX (lihat `docs/SUPABASE-SETUP.md`).

```json
[{"t":"2026-11-26 18:42:11","l":3,"s":42.5,"g":0.031,"d":0.42}]
```

`l` = 1 HATI-HATI, 2 WASPADA, 3 BAHAYA. Karena retained, browser yang baru
dibuka langsung menerima kondisi terakhir walau web sedang tertutup saat
kejadian. Log ini juga dikirim ulang setiap RX tersambung ke broker.

Isinya hanya di RAM: kalau RX reboot, entri ini kosong lagi — data lama tetap
ada di database.

## Upload

1. Arduino IDE → **Tools → Board → ESP32 Dev Module**. Jangan pilih
   `ESP32S3`/`ESP32-S31` — upload akan gagal dengan pesan
   "This chip is ESP32, not ESP32-…".
2. **RX saja**: **Tools → Partition Scheme → "Minimal SPIFFS (1.9MB APP with
   OTA/128KB SPIFFS)"**. Skema default hanya punya satu slot aplikasi 1.2 MB,
   jadi OTA tidak punya tempat menulis firmware baru. Ganti sekali lewat USB,
   setelah itu OTA bisa dipakai. TX tetap skema default.
3. Buka folder sketch, bukan file `.ino` tunggal.
4. Library yang dibutuhkan: `LoRa` (Sandeep Mistry), `PubSubClient`,
   `ArduinoJson`, `LiquidCrystal_I2C`, `Adafruit ADXL345`, `Adafruit BNO055`,
   `Adafruit Unified Sensor`.
5. Upload TX dulu, cek Serial 115200, baru RX.

Boot TX yang benar:

```
Baseline terkunci: G0=1.001 g | pitch=-0.42 roll=1.13 derajat
Pemanasan kemiringan 15 detik (DTH diabaikan sementara)
▶️  Streaming realtime dimulai...
```

Boot RX yang benar:

```
LCD 20x4        : OK (0x27)
🌐 Menghubungkan WiFi: sigapcare OK (192.168.1.x)
✅ MQTT terhubung
LoRa SX1278     : OK (433 MHz, SF7, Listen-Only)
✅ RX Realtime Gateway siap!
```

## Update RX tanpa kabel (OTA)

RX harus terhubung ke WiFi yang sama dengan laptop, lalu di Arduino IDE pilih
port `ladet-rx at 192.168.x.x` (muncul sendiri) dan Upload seperti biasa.

Syarat: **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA)**.
Dengan skema default (satu slot 1.2 MB) OTA gagal dengan pesan
`Not Enough Space` walau kodenya benar.
Sandi default `ladet-ota`; ganti dengan menambahkan di `secrets.h`:

```cpp
#define OTA_PASSWORD "sandi-baru"
```

TX **tidak** punya OTA — di lereng tidak ada WiFi, jadi update TX tetap lewat USB.

## Daya & ketahanan jangka panjang

TX di lereng pakai baterai + panel surya, RX di rumah pakai listrik PLN.

TX **tidak** memakai mode tidur: paket harus tetap keluar tiap 2 detik, dan
light sleep tidak sepadan risikonya bagi keandalan. Angka yang dipakai untuk
merencanakan daya:

| Skenario | Rata-rata | Baterai 12 Ah |
|---|---|---|
| Sekarang (selalu bangun) | ~70 mA | ±7 hari |
| Kalau nanti perlu hemat (light sleep antar paket) | ~20 mA | ±25 hari |

Kebutuhan sekarang ±1,7 Ah/hari (±8 Wh). Panel 20 Wp di Jember (±3–4 jam
matahari efektif) menghasilkan ±60 Wh/hari — cukup bahkan untuk 3–4 hari
hujan. Light sleep bisa ditambahkan lagi nanti tanpa mengubah interval
maupun format paket.

Ukur dulu dengan USB power meter sebelum membeli panel/baterai; angka di atas
perkiraan datasheet, bukan hasil ukur.

| Hal | Tindakan |
|---|---|
| Probe soil resistif | Diberi tegangan terus-menerus = korosi dalam hitungan bulan. Ganti ke sensor capacitive v2.0, atau catu probe hanya saat membaca (1 transistor). |
| Petir | TVS di jalur 5 V, ferrite di kabel probe, box digrounding, antena LoRa jangan jadi titik tertinggi. |
| Air | Box IP65, cable gland, silika gel, sambungan disolder + heat shrink. |
| Duty cycle | 2 detik pada SF7 = ±5 % duty cycle, di atas 1 % yang lazim. Kalau penguji menanyakannya, naikkan `PACKET_INTERVAL` ke 10 detik (RX tidak perlu diubah). |
| Supabase free tier | Di-pause kalau 7 hari tanpa aktivitas → heartbeat harian mencegahnya. |
| Backup | Unduh CSV dari `/riwayat` sebulan sekali, simpan di Drive. |

## Kalau tidak jalan

| Gejala | Penyebab | Tindakan |
|---|---|---|
| RX tidak menerima apa pun | SF/frekuensi beda, atau radio belum ter-init | Cek Serial RX: harus ada `LoRa SX1278 : OK` |
| Paket pertama masuk lalu diam | radio keluar mode LoRa (catu daya) | Pasang kapasitor 100–470 µF di VCC modul LoRa |
| LCD `TX HILANG` | TX mati / di luar jangkauan | Cek Serial TX, cek jarak |
| Relay tidak pindah | `RELAY_ACTIVE_LOW` salah | Balik jadi `1` kalau modul relay aktif LOW |
| Telegram tidak masuk | bot belum jadi admin channel, atau `secrets.h` kosong | Cek Serial `📨 Telegram: ...` |
| Serial TX diam padahal RX terima paket | port USB TX lepas | Tutup Serial Monitor, cabut-pasang USB |

## Kalibrasi sensor

Pakai `tools/sensor-calibration/sensor-calibration.ino` (tanpa LoRa/WiFi, supaya
pembacaan bersih). Hasilnya dimasukkan ke `SOIL_ADC_DRY` / `SOIL_ADC_WET` di
`ladet-tx-realtime.ino`. Detail menu ada di komentar sketch.
