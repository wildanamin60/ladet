# LADET — Firmware (SATU folder untuk TX & RX)

Dua sketch yang di-upload ke dua ESP32. **Selalu compile & upload dari folder ini** —
jangan dari `Documents\Arduino\...` (di situ ada versi lama yang membingungkan).

## 1️⃣ TX — `ladet-tx\ladet-tx.ino`
ESP32 + LoRa SX1278 + ADXL345 + BNO055 + Soil (SEN0308).
- **Tanpa RTC** — DS3231 tidak dipakai; waktu absolut diisi RX (NTP).
- Kalibrasi #0 → data #1..#50, field RTT= untuk delay absolut
- Config-listen: ganti SF dari web (jendela 10 dtk saat boot & setelah run)
- SF aktif tersimpan di **NVS** (tahan reset)
- Wiring LoRa: SCK=18, MISO=19, **MOSI=25**, SS=5, RST=14, DIO0=4
- `LORA_TX_POWER` harus **sama untuk semua skenario** S1–S12

## 2️⃣ RX — `ladet-rx-mqtt\ladet-rx-mqtt.ino`
ESP32 + LoRa SX1278 + WiFi (MQTT ke broker → dashboard) + relay aktuator.
- Terima paket → hitung delay (RTT/kalibrasi) & packet loss (gap SEQ) → publish `ladet/data`
- **Aktuator lokal** (tetap jalan tanpa internet): hijau=27 (`AMAN`),
  kuning=26 (`HATI-HATI`/`WASPADA`), merah=25 (`BAHAYA`), buzzer=33 (`WASPADA`)
- **Telegram dikirim dari RX** (bukan dashboard), antri maks 30 menit saat offline
- Cap waktu: NTP (WIB) → fallback `uptime N s` kalau internet mati
- Subscribe config web (`ladet/config/sf|distance`) + handshake `TYPE=CONF` ke TX
- Status kehadiran: LWT `ladet/rx/online`, heartbeat `ladet/tx/online`
- SF aktif tersimpan di **NVS** (tahan reset)
- Telegram: salin `secrets.example.h` → `secrets.h`, isi token + chat id
  (`secrets.h` tidak ikut Git)
- ⚠️ WiFi, broker, dan wiring **SUDAH terisi sesuai hardware-mu**
  (WiFi `Alieeen3`, **MOSI=25** — sama dengan TX) — jangan ditimpa file dari tempat lain
- ⚠️ **GPIO25 bentrok** dengan relay merah — baca `WIRING.md` sebelum memasang relay

## Pin lengkap
Lihat **`WIRING.md`** — tabel pin TX & RX, wiring relay, catatan daya, dan
cara memastikan MOSI LoRa sebenarnya di pin berapa.

## Urutan upload
1. Buka `ladet-tx\ladet-tx.ino` → board **ESP32 DevKit** → Upload → buka Serial (115200)
2. Buka `ladet-rx-mqtt\ladet-rx-mqtt.ino` → board **ESP32 DevKit** → Upload → Serial (115200)
3. Cek boot:
   - TX: `SF tersimpan : ... (dipulihkan dari NVS)` → jendela config 10 dtk → baseline → run `#0` kalibrasi
   - RX: WiFi ✅ → MQTT ✅ → `📤 Config status: {...,"state":"idle"}` → `✅ RX Gateway siap!`

## Ganti SF / jarak lewat web
Dashboard → panel **Skenario Pengujian** → klik SF/jarak → **Ya, terapkan**:
- `● SFx/xm aktif (TX+RX ✓)` → benar-benar berlaku di dua board (tahan reset)
- `⏳ diantri…` → run sedang berjalan, config menunggu kanal kosong (≥15 dtk)
- `⚠ TX tidak konfirmasi` → TX mati/jauh → RX **tidak berubah** (aman dari desinkron)

Bukti fisik di Serial: TX `⚙️ CONFIG: SF x -> y TER-APPLY`, RX `✅ TX konfirmasi → SF RX aktif: y`,
dan **tidak ada** baris `⚠️ DESINKRON`.

## Folder lain
- `_arsip\` — versi firmware lama & dokumen kerja. **Bukan untuk di-upload.**
