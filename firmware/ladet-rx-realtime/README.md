# LADET RX — Realtime Gateway

Dokumentasi lengkap ada di **[`../README.md`](../README.md)** (satu sumber untuk
TX & RX, supaya tidak ada dua versi yang saling bertentangan).

Isi singkat folder ini:

| File | Keterangan |
|---|---|
| `ladet-rx-realtime.ino` | Gateway: LoRa listen-only → relay + LCD 20×4 + MQTT + Telegram |
| `secrets.h` | Token bot & chat id Telegram. **Tidak ikut Git.** |
| `secrets.example.h` | Contoh format `secrets.h` |

Yang perlu diingat khusus RX:

- LoRa **MOSI = GPIO23** (bukan 25 seperti TX, karena GPIO25 dipakai relay merah).
- RX **tidak pernah memancar**. Jangan tambahkan ACK — itu penyebab radio tuli
  setelah paket pertama pada versi lama.
- `RELAY_ACTIVE_LOW` di baris atas file: `0` untuk modul relay aktif HIGH,
  `1` untuk modul relay aktif LOW.
- LCD opsional. Kalau alamat I2C tidak ditemukan, RX tetap jalan tanpa LCD.
