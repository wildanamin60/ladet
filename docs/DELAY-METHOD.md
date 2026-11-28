# Metode Pengukuran Delay LADET

Dokumen ini menjelaskan **apa yang sebenarnya diukur** oleh kolom `Delay (ms)` di
dashboard, kenapa angkanya sebesar itu, dan parameter apa yang benar-benar bisa
menurunkannya. Dipakai sebagai rujukan penulisan Bab 3 (metode) dan Bab 4 (hasil).

## 1. Definisi yang dipakai

```
RTT   = t(ACK diterima TX) − t(paket data mulai dikirim TX)
delay = RTT / 2
```

`RTT` diukur **seluruhnya di jam TX** (`millis()`/`micros()` yang sama), jadi bebas
dari masalah dua board yang titik nol jamnya berbeda. `RTT/2` adalah estimator satu
arah standar — sama seperti yang dipakai `ping`.

Kenapa bukan waktu tempuh paket data satu arah secara langsung? Karena TX dan RX
tidak punya jam yang tersinkron (tidak ada GPS/PPS). Tanpa sinkronisasi, satu-satunya
besaran yang bisa diukur secara empiris adalah RTT. Perhitungan `rxTime − send_ms`
hanya bisa dipakai setelah offset boot dikalibrasi, dan kalibrasi itu sendiri
memakai `RTT/2` (lihat `hitungDelay()` di `firmware/ladet-rx-skripsi/`).

> Konsekuensi yang harus ditulis di skripsi: `RTT/2` **sedikit lebih kecil** dari
> airtime paket data, karena paket ACK hanya 6 byte sedangkan paket data 174 byte.
> Selisihnya ±40 % di SF7. Angka ini adalah estimasi delay satu arah, bukan durasi
> paket data di udara.

## 2. Rumus airtime (Semtech AN1200.13)

```
Tsym    = 2^SF / BW
Tpreamble = (n_preamble + 4.25) × Tsym
n_payload = 8 + ceil( (8×PL − 4×SF + 28 + 16×CRC − 20×IH) / (4×(SF − 2×DE)) ) × (CR+4)
Tairtime  = Tpreamble + n_payload × Tsym
```

Parameter LADET: `CR = 4/5`, `CRC = on`, `preamble = 8`, `IH = 0` (header eksplisit),
`DE = 1` untuk SF11/SF12.

## 3. Hasil perhitungan

Payload TX sekarang **174 byte** (setelah `THETAR`/`THETAS` dibuang), ACK 6 byte,
asumsi turnaround 20 ms.

| BW | SF | Airtime data | Airtime ACK | RTT | **Delay (RTT/2)** | Tabel 3.6 |
|---|---|---|---|---|---|---|
| 125 kHz | 7 | 282 ms | 36 ms | 338 ms | **169 ms** | Cukup |
| 125 kHz | 9 | 882 ms | 124 ms | 1026 ms | **513 ms** | Buruk |
| 125 kHz | 12 | 6398 ms | 991 ms | 7409 ms | **3705 ms** | Buruk |
| **500 kHz** | 7 | 70 ms | 10 ms | 100 ms | **50 ms** | **Baik** |
| **500 kHz** | 9 | 220 ms | 34 ms | 274 ms | **136 ms** | **Baik** |
| **500 kHz** | 12 | 1599 ms | 250 ms | 1869 ms | **934 ms** | Buruk |

Verifikasi terhadap data lapangan (payload 200 B, BW 125 kHz): prediksi 187 / 574 ms,
terukur 187 / 585 ms.

SF12 tetap di kategori Buruk pada bandwidth apa pun. Itu sifat fisik SF12 (4096 chip
per simbol), bukan kesalahan alat — tulis sebagai trade-off jangkauan vs delay.

## 4. Yang TIDAK mempengaruhi delay

| Faktor | Pengaruh ke delay | Penjelasan |
|---|---|---|
| TX power (4 dBm vs 17 dBm) | **0 ms** | Daya hanya mengubah RSSI/SNR/link budget → packet loss & jangkauan. Tidak ada variabel daya di rumus airtime. |
| Jarak 1 m vs 1 km | **0,0033 ms** | Propagasi radio = 3,3 µs/km. Nol dibanding ratusan ms airtime. |
| RSSI / SNR | 0 ms | Efeknya ke loss, bukan waktu. |
| Antena, orientasi | 0 ms | Sama: loss, bukan delay. |

Jadi "SF7 pada jarak 1 m kok 190 ms" bukan anomali: seluruh waktunya habis di udara.

## 5. Yang mempengaruhi delay

| Parameter | Efek | Biaya |
|---|---|---|
| Bandwidth 125 → 500 kHz | airtime ÷4 | sensitivitas RX turun ±6 dB |
| Panjang payload 200 → 174 B | −36 ms di SF7/125k | tidak ada |
| SF 12 → 9 → 7 | ÷4 tiap langkah | jangkauan turun |
| Coding rate 4/5 | sudah yang tercepat | — |
| Preamble 8 | kecil | — |

## 6. Catatan tentang Tabel 3.6 (ITU-T G.1010)

G.1010 adalah standar **kualitas media percakapan IP** (voice/video, mouth-to-ear),
bukan telemetri sensor. Membandingkan SF12 dengan tabel ini secara metodologis
"kelas salah". Saran penulisan: tetap pakai tabelnya sebagai acuan, tetapi
tambahkan pernyataan bahwa:

1. Delay LADET adalah delay satu arah hasil estimasi `RTT/2`;
2. untuk telemetri monitoring, 1–4 detik masih dapat diterima selama interval
   pengiriman lebih panjang dari delay;
3. kategori "Buruk" pada SF12 adalah konsekuensi memilih jangkauan maksimum.

## 7. Cara reproduksi

```powershell
node C:\Users\User\.gemini\antigravity\brain\<id>\scratch\airtime-bw.js
node C:\Users\User\.gemini\antigravity\brain\<id>\scratch\payload-len.cjs
```

## 8. Checklist sebelum pengujian

- [ ] `LORA_SF` **dan** `LORA_BW` di `ladet-tx-skripsi.ino` == `ladet-rx-skripsi.ino`.
- [ ] Banner serial RX menampilkan `SF / BW : SF7 / 500 kHz`.
- [ ] Sketsa `ladet-tx-realtime` / `ladet-rx-realtime` masih 125 kHz — jangan
      dipasangkan dengan sketsa skripsi yang 500 kHz.
- [ ] Tekan **Mulai Pengujian** di dashboard sebelum tiap run.
