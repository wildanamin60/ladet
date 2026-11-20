# Deploy LADET Dashboard (Vercel)

Arsitektur data resmi:

```
TX (ESP32 + LoRa, tanpa WiFi)
   │  paket LoRa: TYPE=DATA,SEQ=...,DG=...,DTH=...,STATUS=...
   ▼
RX (ESP32 + LoRa + WiFi)
   │  publish JSON nested ke MQTT `ladet/data`
   ▼
Dashboard (Next.js, MQTT over WebSocket)
```

Dashboard **hanya** menerima `ladet/data` dari RX. Payload flat sisa jalur
TX→MQTT (mode pameran) ditolak oleh `parsePayload`.

## 0. Status deploy saat ini

| Item | Nilai |
|---|---|
| Akun Vercel | `wildanamin60` (team `muhammad-wildanun-amien-s-projects`) |
| Project | `ladet` |
| URL produksi | **https://ladet.vercel.app** (dipakai bersama warga/juri) |
| GitHub | **https://github.com/wildanamin60/ladet** (public) |
| Auto-deploy | aktif — push ke branch `main` langsung deploy produksi |

`ladet.vercel.app` adalah domain utama project. URL lama
`ladet-dashboard.vercel.app` dibuat ulang otomatis oleh Vercel setiap deploy
(warisan nama project sebelum rename), jadi `next.config.ts` mengalihkannya
(redirect 308) ke `ladet.vercel.app` — satu-satunya link yang perlu dibagikan.

## 1. Environment variables (wajib diset sebelum build)

| Nama | Nilai | Wajib |
|---|---|---|
| `NEXT_PUBLIC_MQTT_BROKER_URL` | `wss://broker.hivemq.com:8884/mqtt` | ✅ |
| `NEXT_PUBLIC_DEMO_MODE` | `false` | ✅ |
| `NEXT_PUBLIC_SUPABASE_URL` | `https://xxxx.supabase.co` | opsional |
| `NEXT_PUBLIC_SUPABASE_ANON_KEY` | `eyJ...` | opsional |
| `NEXT_PUBLIC_TELEGRAM_BOT_TOKEN` | token bot | opsional |
| `NEXT_PUBLIC_TELEGRAM_CHAT_ID` | `@channel` / `-100...` | opsional |

> Semua route bersifat **static**, jadi `NEXT_PUBLIC_*` dibaca saat build.
> Setelah mengubah env, lakukan **redeploy** — bukan sekadar restart.

Telegram juga bisa diisi dari halaman `/pengaturan` (disimpan di localStorage
browser petugas), tapi env lebih praktis untuk dipakai bersama.

## 2. Deploy lewat CLI (tanpa GitHub)

```bash
npm i -g vercel
vercel link            # buat/ hubungkan project
vercel env add NEXT_PUBLIC_MQTT_BROKER_URL production
vercel env add NEXT_PUBLIC_DEMO_MODE production
vercel --prod
```

## 3. Deploy lewat GitHub (sudah aktif)

Repo: https://github.com/wildanamin60/ladet — sudah terhubung ke project
Vercel `ladet`.

```bash
git add -A
git commit -m "pesan perubahan"
git push origin main        # otomatis deploy produksi ke ladet.vercel.app
```

Environment variables tetap dikelola di dashboard Vercel (bukan di repo),
karena `.env.local` sengaja di-ignore.

## 4. Checklist setelah deploy

- [ ] Buka `/` → badge harus `OFFLINE`/`MENUNGGU RX` selama RX belum hidup.
- [ ] Nyalakan TX + RX → badge `LIVE` dan muncul "Data terakhir dari gateway RX".
- [ ] Matikan RX → badge berubah `MENUNGGU RX` (LWT), bukan tetap LIVE.
- [ ] Matikan TX (RX tetap hidup) → badge tetap `MENUNGGU RX`.
- [ ] `/login` → `/dashboard` menampilkan data realtime.
- [ ] Browser console tidak ada error Supabase/MQTT.
- [ ] `/pengaturan` → ubah ambang tanah jadi lebih rendah → status bisa naik.

## 5. Catatan keamanan

- Login engineer masih **client-side** (`lib/auth.ts`): kredensial ikut
  ter-compile ke bundle JS dan route admin hanya dijaga `localStorage`.
  Untuk URL yang disebar luas, aktifkan **Vercel Deployment Protection**
  atau pindahkan autentikasi ke server.
- Supabase dan Telegram bersifat opsional. Tanpa keduanya dashboard tetap
  berjalan lewat MQTT; halaman riwayat akan memakai data demo.
- **Notifikasi Telegram otomatis dikirim oleh RX**, bukan dashboard — supaya
  tetap berjalan 24 jam walau halaman ditutup dan tetap antre saat internet
  putus. Token bot untuk RX disimpan di `firmware/ladet-rx-mqtt/secrets.h`
  (gitignored), **bukan** di env Vercel. Token di env/localStorage sekarang
  hanya dipakai tombol **Test Kirim** di `/pengaturan`.

## 6. Catatan perilaku

- Ambang batas di `/pengaturan` disimpan **per browser** (localStorage),
  hanya bisa **menaikkan** level, dan dibatasi tupoksi sensor
  (tanah maks HATI-HATI, getaran maks WASPADA, kemiringan maks BAHAYA).
- Nilai getaran memakai satuan **Δg** (delta getaran ADXL345), bukan Hz.
- Supabase polling hanya aktif sebagai **cadangan** ketika MQTT tidak
  mengirim data; data MQTT yang masih segar tidak pernah ditimpa.
