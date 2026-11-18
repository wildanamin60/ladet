# Fix: Delay membengkak sampai 181+ detik

## Gejala

Di grafik dashboard, nilai **Delay (ms)** naik terus setiap paket dan bisa
mencapai **181 detik +++**, padahal harusnya delay LoRa hanya puluhan sampai
ratusan milidetik. Grafik terlihat seperti "selisih dari data n ke data 1"
(selisih kumulatif sejak awal), bukan delay per paket.

## Akar Masalah

Ada **tiga** bug yang saling menumpuk.

### Bug 1 — `delayMs` = uptime RX, bukan delay per paket

`millis()` di TX dan RX punya **titik nol yang berbeda** (waktu boot
masing-masing board). Jadi:

```
rxTime - send_ms  =  delay_asli + offset_boot
```

`offset_boot` bisa ratusan ribu ms (RX sudah menyala lama sebelum TX boot).
Kode lama di RX:

```cpp
} else if (data.send_ms > 0 && data.send_ms < rxTime) {
    delayMs = rxTime - data.send_ms;   // ❌ = delay_asli + offset_boot
}
```

Karena `offset_boot` ≈ uptime RX, nilai "delay" = **waktu yang sudah berjalan
sejak RX/run mulai** → naik ~1000 ms tiap detik → membengkak sampai ratusan
detik. Inilah yang dilihat pengguna di grafik.

### Bug 2 — ACK dikirim terlalu lambat sehingga RTT tidak pernah terukur

RTT diukur TX dari `kirim paket` sampai `terima ACK`. Di RX, ACK dikirim
**setelah** `Serial.printf` + `publishToMQTT()`. MQTT publish bersifat
blocking (100–500 ms; jauh lebih lama saat WiFi reconnecting), sehingga:

- RTT jadi memuat waktu proses MQTT → `delay_ms = RTT/2` ikut membengkak.
- Kalau ACK melewati timeout 1000 ms di TX, `lastRTT` tetap `-1` dan field
  `RTT=` **tidak pernah dikirim sama sekali**. Akibatnya RX selalu jatuh ke
  cabang fallback (Bug 1) → delay = uptime RX.

### Bug 3 — Payload yang sama bisa terhitung dua kali

RX mem-publish satu payload lengkap per paket ke topic `ladet/data`, tapi
dashboard juga subscribe topic lain (`ladet/qos/delay_ms`, dst). Bila payload
yang sama masuk dua kali (retain/QoS/reconnect), grafik & tabel menghitungnya
sebagai **2 data** padahal itu **1 paket**.

## Perbaikan

### A. Firmware RX — `rx-mqtt-publisher.ino`

**1. ACK dikirim paling awal.** ACK dipindah ke posisi tepat setelah
`parsePacket()`, sebelum `Serial.printf` dan `publishToMQTT()`, supaya RTT
murni radio round-trip.

**2. Kalibrasi offset jam TX↔RX.** `delayMs` sekarang benar-benar
`t_terima(RX) − t_kirim(TX)`, dengan 3 tingkat sumber sesuai ketersediaan data:

```
offset_boot  = rawDiff(paket sebelumnya) − RTT/2      // ABSOLUT (pakai RTT TX)
offset_boot  = min(rawDiff) yang pernah terlihat      // RELATIF (jaring pengaman)
delayMs      = (rxTime − send_ms) − offset_boot
```

Refine memakai RTT yang dikirim TX (diukur `micros()` di sisi TX, jadi bebas
dari masalah offset). Karena RTT di paket ke-N adalah RTT paket ke-N-1, nilai
`rawDiff` paket sebelumnya disimpan di `prevRawDiffMs`.

> **Kenapa perlu jaring pengaman relatif?** Delay satu arah yang *absolut*
> secara matematis **tidak bisa** dihitung dari selisih dua jam yang tidak
> tersinkron: `rawDiff = offset_boot + delay` adalah 2 variabel dalam 1
> persamaan. Satu-satunya sumber absolut adalah RTT dari TX. Kalau TX belum
> mengirim `RTT=`, RX otomatis memakai `min(rawDiff)` sebagai offset →
> hasilnya delay **relatif** terhadap paket tercepat. Angkanya tidak akan
> pernah naik mengikuti uptime RX.

**3. Sanity guard.** `delay > 5000 ms` mustahil untuk LoRa (airtime SF12
maksimum ≈ 2 s). Kalau terjadi → pakai RTT/2; kalau RTT juga tidak ada →
tandai `delay_source = "invalid"` (dashboard membuang data ini).

**4. Reset saat pengujian baru.** TX di-reset tiap skenario (SF7 → SF9 → SF12)
sementara RX terus menyala, jadi offset berubah total. Kalibrasi di-reset saat
terdeteksi run baru (`SEQ == 1`, atau SEQ menurun bila paket 1 hilang).

**5. Field baru `delay_source`** dikirim di `test.delay_source` supaya dashboard
( dan kamu ) tahu cara delay diukur:

| Nilai | Arti | Butuh TX baru? |
|---|---|---|
| `clock` | **ABSOLUT** — offset dari RTT TX, paling akurat | ya |
| `clock-rel` | **RELATIF** — 0 = paket tercepat, TX belum kirim `RTT=` | tidak |
| `rtt` | `RTT/2` langsung (TX tidak mengirim `MS=` sama sekali) | — |
| `invalid` | di luar rentang wajar, dibuang dashboard | — |
| `unknown` | belum terhitung | — |

**6. Diagnostik otomatis.** Kalau sampai paket ke-3 TX masih belum mengirim
`RTT=`, RX mencetak kotak peringatan di Serial Monitor yang menyuruh upload
`ladet-tx.ino`. Jadi kamu tidak perlu menebak apakah TX perlu di-flash ulang.

### B. Dashboard

| Berkas | Perubahan |
|---|---|
| `lib/mqtt.ts` | Teruskan `delay_source` pada `LadetPayload` + `normalizePayload` |
| `app/(admin)/dashboard/page.tsx` | **Dedupe** per `packet_id` (1 paket = 1 data); buang data dengan delay di luar rentang wajar; kirim `label` `#SEQ` ke grafik |
| `components/RealtimeChart.tsx` | Chart delay pakai sumbu X `#SEQ` + judul "1 batang = 1 paket"; tooltip menampilkan nomor paket & waktu terima |
| `lib/demo.ts` | `packet_id` demo jadi sekuensial (`resetDemoSequence()`), bukan acak |

## Bukti / Verifikasi

`firmware/verify-delay-algorithm.js` menjalankan ulang algoritma yang sama
dengan simulasi: TX & RX punya epoch `millis()` berbeda (RX menyala 137 s lebih
dulu), delay radio asli 40–60 ms, 50 paket, interval 2 s. **Dua skenario diuji**:
A = TX sudah mendukung `RTT=` (jalur absolut), B = TX belum (jalur relatif).

```
node firmware/verify-delay-algorithm.js
```

Hasil:

```
SKENARIO A — TX mendukung RTT=  (jalur ABSOLUT)
  delay MAX             : 62 ms
  delay rata-rata       : 51.7 ms
  MAE vs delay asli     : 3.0 ms
  rata-rata 10 paket awal: 49.6 ms
  rata-rata 10 paket akhir: 52.6 ms  (tumbuh 3.0 ms)
   ✅ LULUS

SKENARIO B — TX TIDAK mengirim RTT=  (jalur RELATIF)
  delay MAX             : 7 ms
  rata-rata 10 paket awal: 1.8 ms
  rata-rata 10 paket akhir: 1.2 ms  (tumbuh -0.6 ms)
   ✅ LULUS

BANDINGKAN dengan cara LAMA (tanpa koreksi offset sama sekali):
  paket 1  : 137053 ms
  paket 50 : 137042 ms  <-- ≈ uptime RX (naik ~1000 ms tiap detik)
✅ SEMUA LULUS
```

Artinya: cara lama menghasilkan ~137000 ms (dan naik terus mengikuti uptime),
sedangkan cara baru **selalu stabil** — baik saat TX mendukung RTT (MAE 3.0 ms,
sisa error murni dari pembulatan `RTT/2` + overhead proses ACK) maupun saat TX
belum mendukung RTT (delay relatif, tetap tidak membengkak).

Skrip keluar dengan kode `1` kalau ada skenario yang gagal, jadi bisa dipakai
sebagai gate di CI/pretest.

## Cara Pakai (urutan upload)

### Langkah 1 — Isi WiFi di RX (WAJIB)
`rx-mqtt-publisher.ino` baris ~29–30:
```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";  // ← ganti SSID WiFi 2.4 GHz
const char* WIFI_PASS = "YOUR_WIFI_PASS";  // ← ganti password
```
Kalau masih `YOUR_WIFI_SSID`, RX tidak akan pernah connect ke MQTT (Serial
Monitor hanya mencetak `.....`). ESP32 **tidak bisa** WiFi 5 GHz.

### Langkah 2 — Upload RX
Buka `firmware/rx-mqtt-publisher/rx-mqtt-publisher.ino` di Arduino IDE:

| Setelan board | Nilai |
|---|---|
| Board | **ESP32 Dev Module** |
| Baud rate Serial Monitor | **115200** |
| Frekuensi LoRa | 433E6, BW 125 kHz, SF7 |

### Langkah 3 — Baca Serial Monitor RX
Tunggu sampai `✅ MQTT Connected`, lalu jalankan pengujian 50 paket
(**nyalakan RX dulu**, baru reset TX).

Cek baris-baris ini:

| Yang muncul | Artinya |
|---|---|
| `📤 ACK terkirim: "ACK=<seq>"` | ACK benar-benar dikirim RX (muncul di **setiap** paket) |
| `🔧 Kalibrasi awal offset: <nilai> ms (delay relatif dulu, menunggu RTT…)` | **normal di paket 1–2**: pada paket 1 TX memang belum bisa mengirim RTT. Ini **bukan** berarti TX tidak mendukung RTT |
| `🔧 Offset dari RTT TX (ABSOLUT): <nilai> ms → delay ABSOLUT aktif ✅` | TX sudah mendukung RTT → **delay absolut** ✅ (muncul sejak paket 2) |
| `⏱️ Delay: <puluhan> ms (sumber: clock)` | delay absolut, stabil ✅ |
| `🔧 Offset kalibrasi RELATIF (TX belum kirim RTT=)` | jalur relatif — grafik tetap stabil, tapi angka **relatif** |
| kotak `⚠️ TX BELUM MENGIRIM FIELD RTT=` (muncul di paket ke-3) | **upload `ladet-tx.ino` ke TX** untuk delay absolut |
| `⚠️ delay ... > sanity` / `(sumber: invalid)` | data dibuang dashboard; cek kesamaan SF/BW/frekuensi |

### Langkah 4 — (Opsional, hanya untuk delay ABSOLUT) Upload TX
Kalau Langkah 3 memunculkan kotak peringatan RTT, upload
`firmware/ladet-tx/ladet-tx.ino` ke board TX. Pastikan SF/BW/frekuensi sama
dengan RX (433E6, BW 125 kHz, SF7). TX tidak perlu diubah kalau sudah mendukung
`RTT=`.

### Langkah 5 — Cek dashboard
1. Klik tombol skenario di dashboard (memicu `resetScenario()`) **atau** refresh
   halaman (kalau produksi, jalankan `npm run build` dulu).
2. Buka tab **Delay per Paket (1 batang = 1 paket)** — sumbu X harus `#1`,
   `#2`, `#3`, … (bukan timestamp berulang).
3. Yang benar: **batang rata**, nilai puluhan ms, tidak ada tren naik.

## Catatan Metodologi

- Rumus yang dipakai sesuai `lib/calculations.ts`:
  **`Delay = t_terima − t_kirim`**.
- `delay_source = "clock"` → delay **absolut** dari selisih jam TX/RX yang
  offset-nya sudah dipatok memakai RTT TX. `delay_source = "clock-rel"` →
  delay **relatif** (0 = paket tercepat) karena TX belum mengirim `RTT=`.
  `delay_source = "rtt"` → estimasi `RTT/2` apa adanya (hanya dipakai bila
  `MS=` tidak ada sama sekali).
- Keterbatasan yang perlu ditulis di skripsi: delay satu arah yang absolut
  **tidak bisa** diperoleh dari dua jam yang tidak tersinkron. Yang tersedia:
  (a) `RTT/2` dengan asumsi jalur simetris, atau (b) selisih jam yang
  offset-nya dipatok memakai RTT. Keduanya butuh field `RTT=` dari TX.
  Tanpa `RTT=`, yang bisa dilaporkan adalah delay **relatif**/variasi delay.
- Kalau memakai jalur `clock` (absolut), ada bias kecil ≈ waktu proses ACK
  (1–3 ms) karena `RTT/2` menghitungnya sebagai bagian dari delay. Bias ini
  sistematis dan jauh di bawah skala delay LoRa, tapi sebaiknya disebut
  sebagai keterbatasan pengukuran.
- Bias kalibrasi paket pertama (delay paket 1 dianggap 0) hilang begitu RTT
  pertama masuk, yaitu di paket ke-2 — jadi paket 3 dan seterusnya sudah
  akurat. Paket 1 selalu dilaporkan `0 ms` dan boleh dibuang saat analisis.

