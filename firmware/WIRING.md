# LADET — Wiring Pin Lengkap (TX & RX)

Dokumen ini untuk dicek langsung terhadap PCB. Semua nomor adalah **nomor GPIO ESP32
(DevKit 30/38 pin)**, bukan nomor pin fisik pada header.

Ringkasan cepat:

| Peran | TX | RX |
|---|---|---|
| LoRa SCK | 18 | 18 |
| LoRa MISO | 19 | 19 |
| LoRa MOSI | **25** | **25** ⚠️ |
| LoRa NSS/CS | 5 | 5 |
| LoRa RST | 14 | 14 |
| LoRa DIO0 | 4 | 4 |
| I2C SDA (ADXL345 + BNO055) | 21 | — |
| I2C SCL (ADXL345 + BNO055) | 22 | — |
| Soil moisture (analog) | 34 | — |
| LED hijau (relay) | — | **27** |
| LED kuning (relay) | — | **26** |
| LED merah + buzzer BAHAYA (1 relay) | — | **25** ⚠️ |
| Buzzer WASPADA (relay) | — | **33** |

⚠️ = bentrok, baca bagian [GPIO 25](#-gpio-25-bentrok-wajib-dicek) dulu.

---

## 1. TX — `ladet-tx`

### LoRa SX1278 (SPI)

| Sinyal modul | GPIO ESP32 | Catatan |
|---|---|---|
| SCK | 18 | VSPI default |
| MISO | 19 | VSPI default |
| MOSI | 25 | **bukan** default 23 — sesuai wiring LADET v2 |
| NSS / CS | 5 | dikendalikan `LoRa.setPins()` |
| RST | 14 | |
| DIO0 | 4 | **bukan** GPIO2 |
| VCC | 3.3 V | modul 3.3 V saja, jangan 5 V |
| GND | GND | |

### Sensor I2C (ADXL345 + BNO055)

| Sinyal | GPIO ESP32 |
|---|---|
| SDA | 21 |
| SCL | 22 |
| VCC | 3.3 V (keduanya) |
| GND | GND |

ADXL345: alamat I2C `0x53`, CS ke 3.3 V (mode I2C).
BNO055: alamat I2C `0x28`.

### Soil moisture (SEN0308 / probe analog)

| Sinyal | GPIO ESP32 | Catatan |
|---|---|---|
| AO (analog out) | 34 | GPIO34 = input-only, memang untuk ADC |
| VCC | 3.3 V | bisa juga 5 V bila modulnya butuh; sinyal AO tetap ≤ 3.3 V |
| GND | GND | |

> GPIO34–39 **hanya input**. Jangan dipakai untuk LED/relay/buzzer.

### Tidak dipakai lagi

| Komponen | Status |
|---|---|
| DS3231 / RTC | **Dilepas.** Tidak ada pin I2C khusus untuknya; firmware tidak lagi menuntutnya saat boot. Jam absolut diisi RX lewat NTP. |

---

## 2. RX — `ladet-rx-mqtt`

### LoRa SX1278 (SPI) — sama dengan TX

| Sinyal modul | GPIO ESP32 |
|---|---|
| SCK | 18 |
| MISO | 19 |
| MOSI | **25** |
| NSS / CS | 5 |
| RST | 14 |
| DIO0 | 4 |
| VCC | 3.3 V |
| GND | GND |

### Modul relay 4 kanal (aktuator)

| Kanal | Fungsi | GPIO | Perilaku |
|---|---|---|---|
| CH1 | LED hijau | **27** | menyala saat `AMAN` |
| CH2 | LED kuning | **26** | menyala saat `HATI-HATI` dan `WASPADA` |
| CH3 | LED merah + buzzer BAHAYA (satu kanal) | **25** ⚠️ | menyala saat `BAHAYA` |
| CH4 | Buzzer WASPADA | **33** | menyala saat `WASPADA` saja |

Logika yang dijalankan RX (dari field `STATUS=` kiriman TX lewat LoRa):

| Status dari TX | Hijau 27 | Kuning 26 | Merah 25 | Buzzer 33 | Telegram |
|---|---|---|---|---|---|
| AMAN | ON | off | off | off | — |
| HATI-HATI | off | ON | off | off | ✅ |
| WASPADA | off | ON | off | **ON** | ✅ |
| BAHAYA | off | off | **ON** | off | ✅ |

Buzzer BAHAYA tidak di pin 33 — sudah menyatu di kanal relay merah.
Jadi saat BAHAYA: LED merah + buzzer bahaya berbunyi bersamaan dari satu relay.

### Wiring modul relay

Modul relay 4 kanal yang umum dipakai (optocoupler, aktif **LOW**):

| Pin modul | Sambung ke | Catatan |
|---|---|---|
| `IN1` | GPIO 27 | LED hijau |
| `IN2` | GPIO 26 | LED kuning |
| `IN3` | GPIO 25 ⚠️ | LED merah + buzzer bahaya |
| `IN4` | GPIO 33 | buzzer waspada |
| `GND` (sisi logika) | GND ESP32 | **wajib satu GND** |
| `VCC` | **5 V terpisah** | jangan ambil dari pin 3.3 V ESP32 |
| `JD-VCC` (kalau ada jumper) | 5 V terpisah | lepas jumper VCC–JD-VCC bila memakai opto terisolasi |

Kontak relay (sisi beban):

```
5V adaptor ── COM
              NO ── LED + / buzzer +     (menyala saat relay aktif)
GND adaptor ── LED − / buzzer −
```

Kalau beban (LED/buzzer) berbagi adaptor 5 V dengan modul relay, GND adaptor
harus tetap tersambung ke GND ESP32 (common ground), kecuali modul opto
benar-benar terisolasi dan Anda paham risikonya.

**Arus:** tiap kanal relay aktif ±70 mA. 4 kanal bersamaan ±280 mA — pin 5 V
ESP32 (dari USB) tidak sanggup dan bisa brownout. Pakai adaptor 5 V 1–2 A
terpisah. Ini juga melindungi RX supaya tidak reboot saat semua relay menyala.

**Polaritas:** firmware menganggap relay **aktif LOW** (`RELAY_ACTIVE_LOW 1`).
Kalau relay Anda justru menyala saat IN diberi HIGH, ubah satu baris di
`ladet-rx-mqtt.ino` menjadi `#define RELAY_ACTIVE_LOW 0`.

## 🚨 Semua relay nyala saat boot (tanpa TX) — cara mendiagnosa

Gejala: board diberi daya, belum ada paket LoRa, tapi **semua** LED/buzzer
langsung menyala. Kontak **NO/NC tidak berpengaruh** di sini — kalau beban
menyala, artinya kumparan relay memang sedang diberi tegangan.

Artinya pin yang seharusnya "OFF" dibaca modul sebagai "ON". Ada dua penyebab,
dan keduanya diperbaiki dengan cara berbeda:

| Penyebab | Tanda | Perbaikan |
|---|---|---|
| Modul **aktif HIGH** | relay mati saat pin HIGH | `#define RELAY_ACTIVE_LOW 0` |
| Modul 5 V aktif LOW, tapi **3.3 V tidak cukup** mematikan optocoupler | relay nyala terus walau pin ditulis HIGH | VCC logika → 3.3 V, JD-VCC → 5 V, jumper dilepas |

### Cara memastikan (2 menit)

Upload **`firmware/tools/relay-test/relay-test.ino`** ke board RX, buka Serial
Monitor 115200. Sketch itu mengganti semua pin LOW 5 detik → HIGH 5 detik.

- Relay **mati saat LOW** → modul aktif HIGH → set `RELAY_ACTIVE_LOW 0`.
- Relay **mati saat HIGH** → modul aktif LOW → biarkan `RELAY_ACTIVE_LOW 1`.
- Relay **nyala terus** → masalah level tegangan → perbaiki VCC/JD-VCC seperti
  tabel di atas.

### Perbaikan hardware untuk modul 5 V (paling umum)

Modul relay optocoupler 4 kanal punya jumper `VCC–JD-VCC`:

```
                    ┌─ lepas jumper ini
VCC  ── opto/logika ┘
JD-VCC ── kumparan relay ── 5 V adaptor

VCC     → 3.3 V ESP32   (supaya HIGH 3.3 V benar-benar mematikan opto)
JD-VCC  → 5 V adaptor   (kumparan tetap kuat)
GND     → GND ESP32 dan GND adaptor (satu titik)
```

Jangan memberi VCC logika 5 V saat sinyal ESP32 hanya 3.3 V — itulah yang
membuat relay "tidak mau mati".

---

## ⚠️ GPIO 25 bentrok — wajib dicek

Di firmware, **GPIO 25 dipakai LoRa MOSI**:

```cpp
#define LORA_MOSI    25
#define PIN_LED_RED  25   // ← angka yang sama
```

Satu GPIO tidak bisa menjadi MOSI SPI **dan** output relay sekaligus. Kalau
dipaksa, salah satu pasti rusak: relay akan mengikuti sinyal SPI (klik cepat
tiap byte) atau LoRa gagal baca register.

Karena itu firmware sekarang:

- mencetak peringatan besar saat boot,
- **tidak menulis** pin merah selama bentrok (supaya SPI tidak dirusak),
- sehingga kanal merah + buzzer BAHAYA **nonaktif** sampai wiring diperbaiki.
  LED kuning, LED hijau, buzzer waspada, dan Telegram tetap jalan.

### Cara memastikan MOSI LoRa Anda sebenarnya di pin berapa

1. **Lihat jalur PCB**: telusuri dari pin `MOSI` modul LoRa ke kaki ESP32.
   Kalau nyambung ke GPIO **25**, berarti kode sudah benar dan relay merah
   tidak boleh di 25.
2. **Ukur dengan multimeter (kontinuitas)**: mode buzzer, satu probe di pin
   MOSI modul, probe lain di kaki GPIO ESP32. Yang berbunyi itulah pin aslinya.
3. **Cek silkscreen/header**: kalau PCB menamai header aktuator "25", cek
   apakah label itu memang GPIO25 atau hanya nomor urut kanal.

### Dua pilihan perbaikan (pilih salah satu)

**Pilihan A — pindahkan kabel relay merah (paling mudah, tanpa ubah PCB):**

```
Kabel IN3 relay: GPIO25  →  GPIO32
```

Lalu di `ladet-rx-mqtt.ino`:

```cpp
#define PIN_LED_RED  32     // was 25
```

GPIO32 bebas, bukan input-only, dan tidak dipakai LoRa/Serial.

**Pilihan B — kalau hasil ukur menunjukkan MOSI LoRa sebenarnya di GPIO23:**

```cpp
#define LORA_MOSI    23     // di TX *dan* RX
#define PIN_LED_RED  25     // tetap sesuai PCB
```

Ini hanya benar kalau jalur tembaga PCB memang ke GPIO23. Kalau tidak,
LoRa akan mati total — jadi **ukur dulu, jangan menebak**.

Setelah salah satu diterapkan: compile ulang, upload ulang **kedua** board
(jika LORA_MOSI diubah), lalu cek Serial RX — peringatan bentrok harus hilang.

---

## Pin yang jangan dipakai

| GPIO | Alasan |
|---|---|
| 6–11 | terhubung flash internal ESP32 — memakai ini membuat board crash |
| 34, 35, 36, 39 | input-only, tidak bisa jadi output relay |
| 1, 3 | UART0 (Serial Monitor / upload) |
| 0, 2, 12, 15 | pin strapping saat boot — hindari untuk relay |

Sisa pin aman untuk pengembangan: 13, 16, 17, 23, 32.

---

## Urutan upload & cek

1. **TX** — `firmware/ladet-tx/ladet-tx.ino` → Serial 115200 → tunggu
   `TX STATUS: BERHASIL`.
2. **RX** — salin `secrets.example.h` → `secrets.h`, isi token + chat id
   (opsional; tanpa itu aktuator tetap jalan) → upload.
3. Cek Serial RX:
   - `Aktuator : merah=... kuning=... hijau=... buzzer=... (relay aktif LOW)`
   - tidak ada `PERINGATAN BENTROK PIN`
   - `✅ RX Gateway siap!`
4. Uji paksa tiap relay dengan menekan/mengubah kondisi sensor TX, atau
   sementara ubah `applyActuators()` untuk menyalakan satu kanal.
5. Uji Telegram: matikan WiFi RX → naikkan status ke WASPADA → nyalakan WiFi →
   pesan harus terkirim (antrian maksimal 30 menit).
