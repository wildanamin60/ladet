/**
 * CONTOH konfigurasi rahasia RX — JANGAN di-upload apa adanya.
 *
 * Cara pakai:
 *   1. Salin file ini menjadi `secrets.h` (folder yang sama).
 *   2. Isi token bot + chat id Telegram (opsional).
 *   3. Isi SUPABASE_URL + SUPABASE_ANON_KEY (opsional, untuk riwayat 24/7).
 *   4. Upload `ladet-rx-realtime.ino` seperti biasa.
 *
 * `secrets.h` sudah masuk .gitignore, jadi token TIDAK ikut ke GitHub.
 * Kalau file ini tidak ada, firmware tetap jalan normal — hanya notifikasi
 * Telegram dan penyimpanan riwayat yang dilewati.
 *
 * Mendapatkan token Telegram:
 *   Telegram → chat @BotFather → /newbot → ikuti instruksi → salin token
 *   (bentuknya 123456789:AAH...)
 *
 * Mendapatkan chat id:
 *   - Kirim pesan apa pun ke bot Anda lebih dulu (wajib, kalau tidak bot
 *     tidak boleh mengirim pesan ke Anda).
 *   - Buka di browser:
 *       https://api.telegram.org/bot<TOKEN>/getUpdates
 *   - Cari "chat":{"id":123456789}
 *   - Untuk grup: masukkan bot ke grup, kirim pesan, lihat getUpdates.
 *     Chat id grup biasanya negatif, contoh -1001234567890.
 *
 * Mendapatkan URL + anon key Supabase:
 *   Supabase → project Anda → Settings → API
 *   - Project URL     : https://abcdefgh.supabase.co
 *   - anon public key : eyJhbGciOi...   (kunci panjang)
 *   Tabel `ladet_alerts` harus sudah dibuat — lihat `supabase/schema.sql`.
 */

#define TELEGRAM_BOT_TOKEN "123456789:AAHContohTokenGantiDenganPunyaAnda"
#define TELEGRAM_CHAT_ID   "123456789"

// Kosongkan ("" ) kalau tidak memakai database.
#define SUPABASE_URL       "https://contohproject.supabase.co"
#define SUPABASE_ANON_KEY  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.contoh"

// Sandi update firmware lewat WiFi (OTA). Ganti sebelum dipasang di lapangan.
#define OTA_PASSWORD       "ladet-ota"
