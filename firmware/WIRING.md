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
| CH3 | LED merah + buzzer BAHAYA (satu kanal) | **25** | menyala saat `BAHAYA` |
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
| `IN3` | GPIO 25 | LED merah + buzzer bahaya |
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

**Polaritas:** modul relay Anda sudah diuji dengan
`firmware/tools/relay-test/` dan terbukti **aktif HIGH** (HIGH = ON).
Karena itu firmware diset `#define RELAY_ACTIVE_LOW 0`.

Kalau suatu saat modul diganti dengan tipe aktif LOW, ubah menjadi `1`.
Ciri modul aktif LOW: relay justru menyala saat pin diberi LOW.

### LCD 20x4 I2C

| Pin LCD (backpack) | Sambung ke | Catatan |
|---|---|---|
| `VCC` | 5 V | boleh dari adaptor relay 5 V, arus ±25 mA |
| `GND` | GND | satu titik dengan GND ESP32 |
| `SDA` | **GPIO 21** | bebas, tidak bentrok dengan LoRa/relay |
| `SCL` | **GPIO 22** | |

- Modul yang dipakai: backpack **PCF8574** (4 pin), ukuran **20 kolom × 4 baris**.
- **Alamat I2C dicari otomatis** saat boot (0x27, 0x3F, lalu scan 0x08–0x77),
  jadi Anda tidak perlu tahu alamatnya.
- Serial akan mencetak `LCD 20x4 : OK di alamat 0x27` atau
  `tidak ditemukan` bila kabel/backpack bermasalah.
- Kalau layar menyala tapi tidak ada tulisan, putar **potensiometer kontras**
  di belakang backpack.

Isi layar saat berjalan:

```
LADET RX  WiFi:OK
Status: WASPADA
S:72.5% G:0.12g
T:4.3° -87dBm
```

Baris 2 berubah jadi `TX HILANG` bila 30 detik tidak ada paket LoRa; relay
tetap memegang level terakhir (disengaja demi keselamatan).

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

## Pin LoRa TX dan RX BERBEDA (sudah dikonfirmasi di PCB)

| Board | MOSI | Keterangan |
|---|---|---|
| **TX** | **25** | hanya LoRa, tidak ada aktuator |
| **RX** | **23** | GPIO25 dipakai relay LED merah + buzzer BAHAYA |

```cpp
// ladet-tx.ino
#define LORA_MOSI    25

// ladet-rx-mqtt.ino
#define LORA_MOSI    23
#define PIN_LED_RED  25
```

SCK/MISO/NSS/RST/DIO0 tetap sama di kedua board (18/19/5/14/4).

Jangan menyamakan `LORA_MOSI` RX dengan TX: kalau RX memakai 25, pin relay
merah bertabrakan dengan jalur SPI dan LoRa gagal baca register. Firmware
menjaga ini otomatis — `ACTUATOR_RED_CONFLICT` akan mematikan kanal merah dan
mencetak peringatan kalau suatu saat angkanya kembali bentrok.

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
