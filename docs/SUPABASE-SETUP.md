# Riwayat peringatan 24/7 dengan Supabase

Tanpa database, riwayat peringatan hanya tersimpan di browser yang sedang
membuka situs. Uji jangka panjang semalaman tidak akan tercatat.

Dengan langkah di bawah, **RX sendiri** yang menyimpan setiap peringatan ke
database, jadi riwayat tetap lengkap walau tidak ada browser yang terbuka, dan
bisa dibuka dari HP/laptop mana pun.

Total waktu ±10 menit. Gratis.

---

## 1. Buat project Supabase

1. Buka https://supabase.com → **Start your project** → daftar (bisa pakai GitHub/Google).
2. **New project** → isi nama (mis. `ladet`), password database (simpan), region **Singapore**.
3. Tunggu ±2 menit sampai project siap.

## 2. Buat tabel

1. Di project → menu kiri **SQL Editor** → **New query**.
2. Tempel seluruh isi [`supabase/schema.sql`](../supabase/schema.sql).
3. **Run**. Hasil yang benar: `Success. No rows returned`.

Cek di **Table Editor**: tabel `ladet_alerts` (riwayat peringatan) dan
`ladet_heartbeat` (denyut harian perangkat) sudah ada, keduanya masih kosong.

> [!NOTE]
> Skrip ini aman dijalankan berulang. Kalau sebelumnya sudah pernah jalan dan
> kamu hanya menambah bagian heartbeat, cukup jalankan ulang seluruh isinya.

## 3. Ambil kredensial

Menu kiri **Settings** → **API**. Catat dua nilai:

| Nama di Supabase | Dipakai untuk |
|---|---|
| **Project URL** (`https://xxxx.supabase.co`) | firmware + web |
| **anon public** (kunci panjang `eyJhbGciOi...`) | firmware + web |

> [!WARNING]
> Jangan pakai **service_role** key. Kunci itu melewati semua aturan
> keamanan; kalau tertanam di firmware, siapa pun yang memegang board bisa
> mengubah seluruh database.

## 4. Pasang di Vercel (supaya web bisa membaca)

Jalankan di folder proyek:

```powershell
npx vercel env add NEXT_PUBLIC_SUPABASE_URL production
# tempel Project URL saat diminta, lalu Enter

npx vercel env add NEXT_PUBLIC_SUPABASE_ANON_KEY production
# tempel anon key saat diminta, lalu Enter

npx vercel --prod --yes
```

Untuk pengembangan lokal, isi juga `.env.local`:

```
NEXT_PUBLIC_SUPABASE_URL=https://xxxx.supabase.co
NEXT_PUBLIC_SUPABASE_ANON_KEY=eyJhbGciOi...
```

## 5. Pasang di firmware RX

Buka `firmware/ladet-rx-realtime/secrets.h` (salin dari `secrets.example.h`
kalau belum ada), isi dua baris ini:

```cpp
#define SUPABASE_URL       "https://xxxx.supabase.co"
#define SUPABASE_ANON_KEY  "eyJhbGciOi..."
```

Upload ulang **RX** (TX tidak perlu). Di Serial Monitor, saat peringatan
pertama muncul, akan tercetak:

```
🗄️  Supabase /rest/v1/ladet_alerts: tersimpan
```

Kalau tercetak `🗄️ Supabase: gagal konek` atau `HTTP/1.1 401`, berarti URL atau
anon key salah.

## 6. Cek hasilnya

1. Bikin satu peringatan (miringkan node > 5° atau basahi tanah).
2. Supabase → **Table Editor** → `ladet_alerts` → harus muncul baris baru.
3. Buka https://ladet.vercel.app/riwayat → entri muncul, dan tombol
   **⬇ Unduh Riwayat (CSV)** mengunduh seluruh riwayat dari database.

---

## Yang perlu diketahui

| Hal | Penjelasan |
|---|---|
| Kunci ada di firmware | Siapa pun yang memegang board bisa membaca anon key. Dengan aturan di `schema.sql`, kunci itu hanya bisa **menambah** dan **membaca** baris — tidak bisa mengubah atau menghapus. |
| Kalau internet mati | POST gagal, peringatan tetap tersimpan di `ladet/alert/log` (MQTT retained) dan terkirim ke Telegram. Tidak ada antrean ulang. |
| Loop sempat terhenti | Kirim Telegram + POST database masing-masing bisa memblokir ±2,5 detik. Selama itu 1–2 paket LoRa terlewat. Ini hanya terjadi saat level berubah, bukan terus-menerus. |
| Biaya | Free tier Supabase: 500 MB database. Satu peringatan ±120 byte → puluhan ribu baris masih aman. |
| Heartbeat harian | RX menulis 1 baris ke `ladet_heartbeat` tiap 24 jam (saat boot juga). Isinya uptime, sisa heap, alasan reset terakhir, RSSI WiFi, dan umur paket terakhir — cukup untuk tahu perangkat hidup atau reboot sendiri tanpa ke lapangan. |
| Project bisa di-pause | Supabase free tier mem-pause project yang 7 hari tanpa aktivitas. Heartbeat harian mencegahnya. Kalau project telanjur ter-pause, buka dashboard → **Restore project**. |
| Backup | Free tier tidak punya point-in-time recovery. Unduh CSV dari `/riwayat` (atau **Table Editor → Export**) sebulan sekali, simpan di Drive. |
| Hapus data lama | SQL Editor: `delete from public.ladet_alerts where created_at < now() - interval '90 days';` |
