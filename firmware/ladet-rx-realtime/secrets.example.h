/**
 * CONTOH konfigurasi rahasia RX — JANGAN di-upload apa adanya.
 *
 * Cara pakai:
 *   1. Salin file ini menjadi `secrets.h` (folder yang sama).
 *   2. Isi token bot + chat id Anda.
 *   3. Upload `ladet-rx-mqtt.ino` seperti biasa.
 *
 * `secrets.h` sudah masuk .gitignore, jadi token TIDAK ikut ke GitHub.
 * Kalau file ini tidak ada, firmware tetap jalan normal — hanya notifikasi
 * Telegram yang dilewati (Serial akan mencetak "Telegram: belum dikonfigurasi").
 *
 * Mendapatkan token:
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
 */

#define TELEGRAM_BOT_TOKEN "123456789:AAHContohTokenGantiDenganPunyaAnda"
#define TELEGRAM_CHAT_ID   "123456789"
