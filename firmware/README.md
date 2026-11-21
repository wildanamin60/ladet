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
- **LCD 20x4 I2C** (SDA=21, SCL=22): status level, WiFi, nilai sensor terakhir;
  alamat I2C dicari otomatis, LCD opsional (tanpa LCD tetap jalan)
- **Telegram dikirim dari RX** (bukan dashboard), antri maks 30 menit saat offline
- Cap waktu: NTP (WIB) → fallback `uptime N s` kalau internet mati
- Subscribe config web (`ladet/config/sf|distance`) + handshake `TYPE=CONF` ke TX
- Status kehadiran: LWT `ladet/rx/online`, heartbeat `ladet/tx/online`
- SF aktif tersimpan di **NVS** (tahan reset)
- Telegram: salin `secrets.example.h` → `secrets.h`, isi token + chat id
  (`secrets.h` tidak ikut Git)
- ⚠️ WiFi, broker, dan wiring **SUDAH terisi sesuai hardware-mu**
  (WiFi `Alieeen3`, **MOSI RX=23**, **MOSI TX=25**) — jangan ditimpa file dari tempat lain
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

## Kalau RX "berhenti" di tengah run

Urutan yang benar: RX mengirim `ACK=<seq>` **sebelum** publish MQTT, jadi
`MQTT sent` selalu sesudah ACK.

**Watchdog loop (baru).** RX memasang Task Watchdog 30 detik. Kalau satu
iterasi `loop()` tersangkut lebih lama dari itu, ESP32 mencetak:

```
E (123456) task_wdt: Task watchdog got triggered.
E (123456) task_wdt: Task name: loopTask
E (123456) task_wdt: CPU 0: loopTask  (backtrace alamat fungsi yang macet)
```

lalu **reboot sendiri**. Baris itu adalah bukti langsung di mana program
berhenti — kirim ke saya apa adanya.

**Denyut hidup:**
- RX: `⏱️ RX hidup — 8 dtk tanpa paket LoRa (heap ..., WiFi OK, MQTT OK)`
  (hanya muncul saat tidak ada paket, tidak mengganggu log run)
- RX: `📨 PAKET n/50 ... | heap 250000 B` — heap menurun terus = bocor memori
- TX: `[TX hidup] SEQ=17 SF=7 mode=RUN/MONITOR heap=...` tiap 10 dtk
- LCD baris 4 diawali angka detik (`123s -61dBm T:1.1`); angka berhenti
  bertambah = program RX macet, bukan TX.

Cara membacanya:

| Yang terlihat | Artinya |
|---|---|
| RX mencetak `RX hidup … tanpa paket` terus | RX sehat, **TX berhenti mengirim** → cek TX |
| LCD berubah ke `Status: TX HILANG`, detik tetap jalan | RX sehat, TX/radio yang berhenti |
| TX tetap mencetak `[TX hidup]` | TX hidup, data tidak sampai → radio/antenna/jarak |
| Serial RX **diam total**, LCD detik membeku | RX hang → watchdog akan reboot sendiri ≤30 dtk |
| Serial TX diam tapi RX masih terima paket | **port USB TX mati** (bukan firmware). Tutup Serial Monitor, cabut-pasang kabel USB, pilih port COM lagi |

Port USB ESP32 sering hilang sendiri setelah board reset/brownout: Serial
Monitor Arduino IDE tetap terbuka di port lama dan tampak "diam", padahal
board-nya jalan normal.

## Folder lain
- `_arsip\` — versi firmware lama & dokumen kerja. **Bukan untuk di-upload.**
