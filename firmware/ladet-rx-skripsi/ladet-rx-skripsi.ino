/**
 * LADET RX — Gateway MQTT Publisher
 * 
 * Menerima data sensor dari TX via LoRa, lalu publish ke MQTT broker.
 * Format TX: comma-delimited key=value pairs
 * 
 * Hardware:
 * - ESP32
 * - LoRa SX1278/SX1276
 * - WiFi (untuk MQTT)
 * 
 * Topic MQTT: ladet/data
 * Payload: JSON nested (sensor/radio/test)
 */

#include <WiFi.h>
#include <time.h>               // NTP — pengganti RTC
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LoRa.h>
#include <Preferences.h>   // simpan SF aktif di flash (NVS) — tahan reset
#include <esp_system.h>    // esp_reset_reason() - penyebab reboot terakhir
#include <esp_task_wdt.h>  // watchdog loop: hang >90 dtk → reboot sendiri

// ══════════════════════════════════════════════════════════════════════════════
// PROFIL FIRMWARE — SATU SUMBER, DUA BUILD
// ══════════════════════════════════════════════════════════════════════════════
// LADET_PROFILE_THESIS 1 = firmware PENGUJIAN SKRIPSI (ringan):
//     hanya LoRa + WiFi/MQTT. LCD, relay, buzzer, dan Telegram TIDAK
//     dikompilasi sama sekali. Tujuannya menjaga data 50 paket:
//       • tidak ada TLS Telegram yang bisa membekukan loop 2–8 detik
//         (selama itu paket LoRa tidak didengarkan → hilang),
//       • tidak ada lonjakan arus relay/buzzer yang memicu brownout,
//       • RAM & CPU lega, jumlah restart turun.
//
// LADET_PROFILE_THESIS 0 = firmware IMPLEMENTASI LAPANGAN:
//     realtime terus + aktuator (relay/buzzer) + LCD + notifikasi Telegram.
//
// Logika LoRa, parsing paket, hitung delay, packet loss, NVS, antrian
// store-and-forward, dan payload MQTT tetap SAMA di kedua profil. Jadi data
// hasil pengujian identik dengan yang nanti dipakai di lapangan.
#define LADET_PROFILE_THESIS 1
// 1 = pengujian skripsi, 0 = implementasi lapangan
// Protokol: LoRa + WiFi/MQTT. LCD/relay/buzzer/Telegram TIDAK dipakai.
// Logika LoRa, parsing, delay, packet loss, NVS, payload MQTT identik dengan file
// implementasi (LADET_PROFILE_THESIS=0), jadi kalau ada perubahan di salah satu,
// salin perubahan yang sama ke file yang lain agar data tetap konsisten.

#if !LADET_PROFILE_THESIS
#include <WiFiClientSecure.h>   // Telegram (HTTPS)
#include <Wire.h>               // I2C untuk LCD 20x4 (PCF8574)
#include <LiquidCrystal_I2C.h>  // LCD I2C 20x4
#endif

// ══════════════════════════════════════════════════════════════════════════════
// KONFIGURASI — SESUAIKAN DENGAN SETUP ANDA
// ══════════════════════════════════════════════════════════════════════════════

// WiFi
// ⚠️ WAJIB DIISI sebelum upload — kalau masih "YOUR_WIFI_SSID" RX tidak akan
//    pernah connect ke MQTT (Serial Monitor akan mencetak "....." terus-menerus).
//    Perhatikan: harus WiFi 2.4 GHz (ESP32 tidak support 5 GHz).
const char* WIFI_SSID = "sigapcare";  // Ganti dengan SSID WiFi
const char* WIFI_PASS = "ladet123";  // Ganti dengan password WiFi

// MQTT Broker
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;  // Native MQTT TCP (sudah terbukti working)
const char* MQTT_TOPIC  = "ladet/data";

// LoRa Pins — LADET Wiring (harus sama dengan TX)
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23   // RX: GPIO23 — BEDA dengan TX! GPIO25 di RX dipakai relay LED merah
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0   4   // PENTING: GPIO4, bukan GPIO2

// LoRa Frequency
#define LORA_FREQ  433E6  // 433 MHz untuk LADET

// LoRa Spreading Factor — HARUS SAMA DENGAN TX
// Ganti sesuai skenario pengujian: 7, 9, atau 12
#define LORA_SF     7

// Daya kirim ACK dari RX. SAMAKAN dengan TX: hardware LADET memakai regulator
// AMS1117 yang tidak kuat menahan lonjakan arus PA. Di TX, 4 dBm terbukti
// stabil dan ≥14 dBm menyebabkan BROWNOUT. Default library LoRa = 17 dBm —
// itu sebabnya RX mati/radio tuli tepat setelah mengirim ACK pertama.
// ACK hanya beberapa byte dan jaraknya dekat, jadi 4 dBm sudah cukup.
#define ACK_TX_POWER  4

// Batas waktu tunggu TxDone saat RX membalas ACK.
// WAJIB ADA. Library LoRa (LoRa.cpp: deadline-free) menunggu flag TxDone TANPA
// batas waktu: `while ((readRegister(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) == 0) yield();`
// Kalau radio tidak pernah melaporkan TxDone (PA tersendat / brownout / EMI),
// loop() membeku di situ dan Task Watchdog baru me-reset ESP32 setelah 90 detik.
// Selama itu SEMUA paket TX hilang — persis gejala "paket ke-3 lalu 46 hilang"
// yang dilaporkan CSV sebagai "RX restart". Nilai ini longgar: ACK 8 byte hanya
// butuh ±25 ms di SF7 dan ±1 dtk di SF12.
#define ACK_TX_TIMEOUT_MS 3000UL

// 1 = RX ikut mode TX untuk mengirim ACK=<seq> (dipakai TX mengukur RTT →
//     delay ABSOLUT). 0 = listen-only seperti ladet-rx-realtime (delay jadi
//     RELATIF, sumber "clock-rel"). Ubah ke 0 hanya untuk uji pembanding.
#define RX_SEND_ACK 1

// Test Configuration — UBAH SESUAI SKENARIO PENGUJIAN
const int TEST_DISTANCE_M = 100;  // 100m, 300m, 700m, atau 1000m (sesuai revisi sempro)

// TEST MODE — Uncomment untuk TEST 0 (MQTT only, no LoRa)
// #define MQTT_TEST_MODE 1

// ══════════════════════════════════════════════════════════════════════════════
// AKTUATOR LOKAL (relay) — WAJIB tetap jalan walau WiFi/MQTT mati
// ══════════════════════════════════════════════════════════════════════════════
// RX tidak menghitung ulang level: ia memakai field STATUS= yang dikirim TX
// lewat LoRa. Jadi keputusan tetap di lereng, RX hanya menjalankan relay:
//
//   AMAN       → LED hijau
//   HATI-HATI  → LED kuning
//   WASPADA    → LED kuning + buzzer waspada
//   BAHAYA     → LED merah (kanal relay yang sama juga membunyikan buzzer bahaya)
//
// ⚠️ PIN LoRa RX BERBEDA DENGAN TX (sudah dikonfirmasi lewat PCB):
//    TX : MOSI = GPIO25  (tidak ada aktuator)
//    RX : MOSI = GPIO23  (GPIO25 dipakai relay LED merah + buzzer BAHAYA)
//    SCK/MISO/NSS/RST/DIO0 tetap sama di kedua board.
//    Jangan menyamakan LORA_MOSI RX dengan TX — kalau RX memakai 25, relay
//    merah akan bertabrakan dengan jalur SPI dan LoRa gagal baca register.
#define PIN_LED_RED      25   // merah + buzzer BAHAYA (satu kanal relay)
#define PIN_LED_YELLOW   26
#define PIN_LED_GREEN    27
#define PIN_BUZZER       33   // buzzer WASPADA

// LCD 20x4 I2C (backpack PCF8574, 4 pin: VCC GND SDA SCL).
// Alamat TIDAK di-hardcode: dicari otomatis saat boot (0x27/0x3F/0x20–0x27).
// Kalau LCD tidak terpasang, firmware tetap jalan normal tanpa layar.
#define LCD_SDA          21
#define LCD_SCL          22
#define LCD_COLS         20
#define LCD_ROWS         4

// Modul relay umum (optocoupler) aktif LOW: IN=LOW → kontak menutup.
// Kalau relay Anda justru aktif HIGH, ubah angka ini menjadi 0.
//
// ⚠️ MODUL ANDA SUDAH DIUJI: AKTIF HIGH (HIGH = relay ON, LOW = OFF).
// Itulah sebabnya semua relay menyala saat boot ketika nilainya masih 1:
// firmware menulis HIGH untuk "OFF", tapi modul membacanya sebagai ON.
// Kalau suatu saat modul relay diganti dengan tipe aktif LOW, ubah ke 1.
#define RELAY_ACTIVE_LOW 0

// Pin merah yang bentrok dengan jalur LoRa TIDAK boleh ditulis dari program:
// menulis pin MOSI/SCK/SS akan merusak komunikasi SPI. Selama bentrok, kanal
// merah (dan buzzer BAHAYA yang menempel di sana) dinonaktifkan dan alasannya
// dicetak di Serial — lebih baik jujur daripada alarm yang diam-diam mati.
constexpr bool ACTUATOR_RED_CONFLICT =
    (PIN_LED_RED == LORA_MOSI) || (PIN_LED_RED == LORA_MISO) ||
    (PIN_LED_RED == LORA_SCK)  || (PIN_LED_RED == LORA_SS);

// ══════════════════════════════════════════════════════════════════════════════
// TELEGRAM — dikirim dari RX (bukan dashboard) supaya tetap berjalan 24 jam
// walau tidak ada browser yang membuka dashboard. Hanya bisa terkirim kalau
// RX punya internet; kalau jaringan mati, pesan disimpan lalu dikirim saat
// jaringan kembali (maksimal 30 menit).
// ══════════════════════════════════════════════════════════════════════════════
// Simpan token & chat id di file `secrets.h` di folder yang sama (tidak ikut
// Git). Tanpa file itu firmware tetap jalan normal, hanya notifikasi dilewati.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef TELEGRAM_BOT_TOKEN
  #define TELEGRAM_BOT_TOKEN ""   // isi lewat secrets.h
#endif
#ifndef TELEGRAM_CHAT_ID
  #define TELEGRAM_CHAT_ID  ""
#endif

// ══════════════════════════════════════════════════════════════════════════════
// GLOBAL OBJECTS
// ══════════════════════════════════════════════════════════════════════════════

WiFiClient espClient;
PubSubClient mqttClient(espClient);

unsigned long lastReconnectAttempt = 0;
unsigned long mqttRetryMs = 5000UL;      // backoff reconnect MQTT (5..60 dtk)

// ══════════════════════════════════════════════════════════════════════════════
// ANTRIAN STORE-AND-FORWARD (tahan WiFi/MQTT putus saat pengujian)
// ══════════════════════════════════════════════════════════════════════════════
// Masalah yang diperbaiki: dulu, begitu WiFi/MQTT putus, publishToMQTT() langsung
// return dan paket LoRa yang SUDAH diterima RX dibuang. Dashboard lalu melihat
// lompatan SEQ (mis. 4 → 29) seolah 24 paket hilang, padahal RX menerimanya —
// hanya gagal dikirim. Untuk data skripsi itu fatal.
//
// Sekarang payload JSON disimpan di RAM saat publish gagal, lalu dikirim ulang
// berurutan begitu MQTT tersambung. 80 slot ≈ cukup untuk ~4 menit TX 2 dtk
// (80 × ~700 B ≈ 56 KB; heap bebas RX masih >200 KB). Bila antrean penuh,
// paket TERTUA dibuang supaya yang terbaru tetap masuk (dan dicatat di Serial).
#define PENDING_MAX 80
String pendingData[PENDING_MAX];
uint16_t pendingHead  = 0;    // slot tertua
uint16_t pendingCount = 0;    // jumlah slot terisi
unsigned long pendingDropped = 0;
bool pendingWarned = false;

/** Simpan satu payload JSON ke antrean RAM (dipanggil saat publish gagal). */
void enqueuePending(const char* json) {
  if (pendingCount >= PENDING_MAX) {
    // Penuh: buang yang tertua agar paket terbaru tetap tercatat.
    pendingData[pendingHead] = "";
    pendingHead = (uint16_t)((pendingHead + 1) % PENDING_MAX);
    pendingCount--;
    pendingDropped++;
    Serial.printf("⚠️  Antrean penuh — %lu paket tertua dibuang\n", pendingDropped);
  }
  uint16_t tail = (uint16_t)((pendingHead + pendingCount) % PENDING_MAX);
  pendingData[tail] = String(json);
  pendingCount++;
  if (!pendingWarned) {
    pendingWarned = true;
    Serial.printf("💾 MQTT putus — paket disimpan untuk dikirim ulang (antrean %u)\n",
                  (unsigned)pendingCount);
  }
}

/**
 * Kirim ulang antrean saat MQTT tersambung. Dipanggil dari loop() saja.
 * Dibatasi beberapa paket per putaran supaya loop() tidak memblokir radio
 * terlalu lama; sisa antrean dilanjutkan pada loop berikutnya (milidetik lagi).
 */
void flushPending(uint8_t maxPerLoop = 10) {
  if (!mqttClient.connected()) return;
  uint8_t sent = 0;
  unsigned long t0 = millis();
  // Batas waktu 2 dtk: kalau broker lambat, publish() bisa memblokir belasan
  // detik per paket. Tanpa batas ini, 10 paket antrean bisa menahan loop
  // >2 menit → radio tidak didengarkan (paket hilang) dan watchdog me-reset RX.
  while (pendingCount > 0 && sent < maxPerLoop && mqttClient.connected()
         && (millis() - t0) < 2000UL) {
    if (!mqttClient.publish(MQTT_TOPIC, pendingData[pendingHead].c_str())) {
      break;  // masih gagal → coba lagi pada loop berikutnya
    }
    pendingData[pendingHead] = "";   // bebaskan heap segera
    pendingHead = (uint16_t)((pendingHead + 1) % PENDING_MAX);
    pendingCount--;
    sent++;
  }
  if (pendingCount == 0) {
    if (pendingWarned) {
      Serial.printf("✅ Antrean tuntas — semua paket tertunda sudah terkirim\n");
      pendingWarned = false;
    }
  } else if (sent > 0) {
    Serial.printf("📤 Antrean tersisa %u paket\n", (unsigned)pendingCount);
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// STRUCT UNTUK MENYIMPAN DATA SENSOR
// (didefinisikan SEBELUM fungsi-fungsi config — jebakan .ino Arduino: auto-
//  prototype fungsi ber-tipe SensorData harus berada SETELAH struct-nya)
// ══════════════════════════════════════════════════════════════════════════════

struct SensorData {
  char type[16];
  int seq;
  int soil_raw;
  int soil_mv;
  float theta;
  float theta_r;
  float theta_s;
  float sat;
  float g0;
  float g;
  float dg;
  float tx;
  float ty;
  float tz;
  float dtx;
  float dty;
  float dtz;
  float dth;
  unsigned long send_ms;   // TX timestamp (millis)
  float rtt_ms;           // RTT dari TX (ms), -1 jika tidak ada
  char status[16];
  int sf;
  float theta0;   // sudut acuan (baseline saat run dimulai) — dari field THETA0=
  float thetam;   // sudut terukur (BNO Z) — dari THETAM= (paket lama) atau = TZ (paket slim)
  bool has_thetam; // true = field THETAM= eksplisit diterima (paket lama)
};


// ══════════════════════════════════════════════════════════════════════════════
// CONFIG REMOTE — web -> MQTT -> RX -> LoRa -> TX
// ══════════════════════════════════════════════════════════════════════════════
// Web publish "SF9" / "300" ke ladet/config/sf|distance (retained); RX
// berlangganan keduanya. Alur aman supaya TX & RX tidak pernah beda SF:
//   1) Config DIANTRI selama paket data masih mengalir (< 15 dtk sejak
//      paket terakhir) — SF tidak pernah berubah di tengah run.
//   2) Kanal kosong → RX kirim "TYPE=CONF,SF=n" ke TX MASIH di SF lama.
//   3) TX balas "ACK=CONF" (di SF lama), lalu menerapkan SF baru.
//   4) Setelah ACK, RX menerapkan SF baru & melaporkan status ke
//      ladet/config/status (state: applied|queued|rejected|tx_unconfirmed).
int activeSF        = LORA_SF;         // SF radio RX yang sedang aktif
int activeDistance  = TEST_DISTANCE_M; // jarak metadata yang dilaporkan payload
int pendingSF       = -1;              // SF yang diminta web (-1 = tidak ada)
int pendingSFTries  = 0;               // berapa kali handshake tertunda oleh paket data
unsigned long pendingSFSince = 0;      // kapan SF diminta (batas waktu retry)
int pendingDistance = -1;
unsigned long lastLoRaRxMs = 0;        // millis() paket LoRa TERAKHIR (apa pun: data/BEACON) — bukti TX hidup
unsigned long lastDataRxMs = 0;         // millis() paket DATA terakhir
// TX CONTINUOUS mengirim data tiap ±2 dtk, jadi kanal TIDAK PERNAH benar-benar
// sunyi 15 dtk. Config SF dikirim di celah antar paket: setelah 1,2 dtk tanpa
// data (masih jauh sebelum paket berikutnya) handshake CONF dijalankan.
const unsigned long CONFIG_QUIET_MS   = 1200UL;
const unsigned long CONF_WAIT_MS      = 1500UL;  // timeout tunggu ACK=CONF
const int           CONF_MAX_ATTEMPTS = 10;
const char* CONFIG_STATUS_TOPIC = "ladet/config/status";

// ── Penjaga radio (self-healing) ─────────────────────────────────────────────
// SX1278 bisa "mati diam-diam": setelah brownout/EMI (mis. relay menyala)
// chip kembali ke mode FSK — bit LongRangeMode hilang — sehingga berhenti
// menerima LoRa walau ESP32 tetap hidup, MQTT tetap online, dan uptime naik.
// Itu persis gejala "paket pertama masuk, sisanya tidak pernah terdengar".
// Penjaga ini membaca register chip lewat SPI tiap 2 dtk dan melakukan init
// ulang penuh kalau chip tidak sehat atau tuli terlalu lama.
const unsigned long RADIO_CHECK_MS    = 2000UL;   // interval baca register
// Ambang "tuli" kini dihitung per-SF di superviseLoRa(): SF7 9 dtk, SF9 15 dtk,
// SF12 30 dtk (satu paket SF12 memang butuh ±6–8 dtk, jadi ambang tetap 9 dtk
// akan salah menganggap radio tuli dan re-init justru membuang paket).
const unsigned long RADIO_COOLDOWN_MS = 5000UL;   // jeda antar init ulang
unsigned long lastRadioCheckMs   = 0;
unsigned long lastRadioRecoverMs = 0;
uint8_t radioBadChecks        = 0;   // berapa kali chip terbaca tidak sehat
uint8_t radioRecoverAttempts  = 0;   // init ulang beruntun tanpa paket masuk
bool radioEverHeard           = false; // TX pernah terdengar? (jangan re-init sia-sia)
unsigned long ackTimeouts     = 0;   // berapa kali ACK tidak selesai (TxDone tak datang)

// ── Jaga WiFi tanpa reboot ──────────────────────────────────────────────────
// ESP32 punya auto-reconnect bawaan SDK, tapi kalau AP restart atau sinyal
// hilang lama, sambungan kadang tidak pernah kembali sendiri. Dulu satu-satunya
// jalan keluar adalah reboot RX, dan setiap reboot = satu lubang data di CSV.
const unsigned long WIFI_RETRY_MS = 15000UL;
unsigned long lastWifiRetryMs = 0;

// ══════════════════════════════════════════════════════════════════════════════
// DUA TASK: RADIO (radioTask) vs JARINGAN (loopTask)
// ══════════════════════════════════════════════════════════════════════════════
// Masalah yang diselesaikan: TX hanya menunggu ACK 1,5 dtk. Dulu RX menjalankan
// LoRa DAN jaringan di satu task, jadi setiap percobaan koneksi MQTT/Telegram
// yang memblokir beberapa detik (DNS lambat, TLS handshake, broker timeout)
// membuat radio TIDAK didengarkan → TX mencetak "ACK : TIMEOUT" dan dashboard
// "menunggu RX", padahal kedua board hidup.
//
// ══════════════════════════════════════════════════════════════════════════════
// STATUS KEHADEVARAN PERANGKAT (supaya dashboard TIDAK menebak)
// ═══════════════════════════════════════════════════════════════════════════════
// ladet/rx/online : RX publish "online" (retained) saat konek; broker publish
//                   "offline" otomatis via LWT kalau RX mati/listrik putus.
// ladet/tx/online : RX menandai "online" begitu ada paket LoRa masuk, dan
//                   "offline" kalau 30 detik sunyi (TX mati/di luar jangkauan).
const char* RX_ONLINE_TOPIC = "ladet/rx/online";
const char* TX_ONLINE_TOPIC = "ladet/tx/online";
bool txOnlinePublished = false;   // state "TX online" yang terakhir dipublish
unsigned long lastOnlinePublish = 0;  // heartbeat kehadiran RX (tiap 20 dtk)
Preferences prefs;                // penyimpanan NVS untuk SF aktif (tahan reset)

// ── Diagnostik & pemulihan diri ──────────────────────────────────────────────
// Watchdog bawaan ESP32 (Task WDT) hanya menangkap loop yang benar-benar
// membeku. Kegagalan yang justru sering terjadi di lapangan adalah "setengah
// hidup": WiFi stack nyangkut, atau MQTT tidak pernah bisa connect lagi.
// Tidak ada WDT yang menolong kondisi itu, jadi RX reboot sendiri setelah
// beberapa menit — lebih baik bolak-balik online daripada diam tanpa jejak.
const char* RX_STATUS_TOPIC = "ladet/rx/status";   // JSON diagnostik (non-retained)
uint16_t bootCount = 0;                            // jumlah boot (disimpan di NVS)
unsigned long lastWifiOkMs = 0;
unsigned long lastMqttOkMs = 0;
const unsigned long WIFI_RESTART_MS  = 30UL * 60UL * 1000UL; // 30 menit
const unsigned long MQTT_RESTART_MS  = 5UL * 60UL * 1000UL;  // 5 menit
const unsigned long HEALTH_INTERVAL_MS = 60000UL;            // lapor tiap 1 menit
unsigned long lastHealthMs = 0;

/** Nama penyebab reset terakhir — tampil di Serial & dikirim ke dashboard. */
const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT_RESET";
    case ESP_RST_SW:       return "SW_RESTART";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    default:               return "UNKNOWN";
  }
}

/**
 * Kirim diagnostik ke `ladet/rx/status`.
 * Dashboard saat ini mengabaikan topik ini (bukan `ladet/data`), jadi tidak
 * mengganggu alur data. Gunanya untuk memantau dari MQTT Explorer / mosquitto_sub:
 * apakah RX sering reboot, heap menipis, atau WiFi melemah.
 */
void publishHealth() {
  if (!mqttClient.connected()) return;
  String json = "{\"reset\":\"" + String(resetReasonName()) + "\"" +
                ",\"boot_count\":" + String(bootCount) +
                ",\"uptime_s\":" + String(millis() / 1000UL) +
                ",\"heap\":" + String(ESP.getFreeHeap()) +
                ",\"wifi_rssi\":" + String(WiFi.RSSI()) +
                ",\"sf\":" + String(activeSF) + "}";
  mqttClient.publish(RX_STATUS_TOPIC, json.c_str());
  Serial.println("📊 Health: " + json);
}

/**
 * Restart dengan rapi: MQTT di-disconnect lebih dulu supaya broker TIDAK
 * mem-publish LWT "offline". Tanpa ini, setiap reboot akan sempat membuat
 * dashboard menampilkan RX mati padahal hanya sedang memulai ulang.
 */
void safeRestart(const char* reason) {
  Serial.printf("♻️  Restart ESP32: %s\n", reason);
  Serial.flush();
  if (mqttClient.connected()) {
    mqttClient.disconnect();
    delay(300);
  }
  ESP.restart();
}

/**
 * Task Watchdog untuk loop().
 * DIPERLAKU KAN sebagai DARURAT SAJA: timeout 90 detik. Satu-satunya keadaan
 * yang boleh me-reset RX adalah loop yang benar-benar membeku (I2C macet,
 * driver radio hang) — bukan karena jaringan lambat. Gangguan WiFi/MQTT
 * TIDAK dihitung; itu ditangani reconnect tanpa reboot.
 * Dipanggil di AKHIR setup() supaya proses boot tidak dihitung.
 */
bool wdtArmed = false;
void setupLoopWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = 30000;   // 30 detik — cukup untuk satu percobaan
                                // jaringan terpanjang, tapi jauh lebih pendek
                                // dari 90 dtk: setiap detik loop membeku = satu
                                // paket TX hilang (interval TX ±2 dtk).
  cfg.idle_core_mask = 0;       // jangan ikut memantau idle task Arduino
  cfg.trigger_panic  = true;    // panic → cetak lokasi hang → reboot

  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err != ESP_OK) err = esp_task_wdt_init(&cfg);

  // loopTask bisa saja sudah terdaftar oleh core Arduino.
  esp_err_t addErr = esp_task_wdt_add(NULL);
  wdtArmed = (err == ESP_OK) && (addErr == ESP_OK || addErr == ESP_ERR_INVALID_ARG);
  Serial.printf("Watchdog loop  : %s (timeout 90 dtk — hanya kalau benar-benar macet)\n",
                wdtArmed ? "aktif" : "TIDAK aktif");
}

/* ══════════════════════════════════════════════════════════════════════════
   WAKTU — pengganti RTC
   ══════════════════════════════════════════════════════════════════════════
   TX tidak punya jam dinding (DS3231 dilepas). RX punya WiFi, jadi waktu
   absolut diambil dari NTP. Kalau internet mati, dipakai uptime — jujur dan
   tetap bisa dipakai untuk menghitung "berapa menit sebelum jaringan pulih". */

void setupClock() {
  // WIB = UTC+7. Non-blocking: kalau belum sinkron, nowStamp() memakai uptime.
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
}

String nowStamp() {
  time_t t = time(nullptr);
  if (t > 1700000000) {                 // sudah sinkron (tahun 2023+)
    struct tm tmBuf;
    localtime_r(&t, &tmBuf);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return String(buf);
  }
  return "uptime " + String(millis() / 1000UL) + " s";
}

#if LADET_PROFILE_THESIS
/* ══════════════════════════════════════════════════════════════════════════
   PROFIL SKRIPSI — AKTUATOR/LCD/TELEGRAM TIDAK DIPAKAI
   ══════════════════════════════════════════════════════════════════════════
   Semua fungsi dibuat kosong supaya handleLoRaPacket() dan setup() tidak
   perlu diubah. Tidak ada relay yang ditulis, tidak ada layar yang di-update,
   dan tidak ada koneksi TLS Telegram — loop tetap cepat menerima LoRa. */
int statusToLevel(const char* status) {
  String s = String(status);
  s.trim();
  s.toUpperCase();
  if (s.startsWith("BAHAYA"))  return 3;
  if (s.startsWith("WASPADA")) return 2;
  if (s.startsWith("HATI"))    return 1;
  if (s.startsWith("AMAN"))    return 0;
  return -1;
}
// Variabel yang tetap ditulis handleLoRaPacket() walau LCD tidak ada.
// Nilainya tidak ditampilkan, tapi membuat kode pengujian dan lapangan identik.
float lcdSat = 0.0, lcdDg = 0.0, lcdTilt = 0.0;
int   lcdRssi = 0, lcdLevel = -1;

/**
 * Relay/buzzer tetap terpasang di board, jadi PIN-nya dimatikan SEKALI di awal.
 * Kalau dibiarkan mengambang (input), modul relay aktif-LOW bisa menganggap
 * inputnya aktif dan semua relay menyala saat boot. Setelah ini tidak ada
 * penulisan pin lagi — itulah yang menghilangkan lonjakan arus pemicu brownout.
 */
void setupActuators() {
  const int OFF = RELAY_ACTIVE_LOW ? HIGH : LOW;
  pinMode(PIN_LED_YELLOW, OUTPUT); digitalWrite(PIN_LED_YELLOW, OFF);
  pinMode(PIN_LED_GREEN,  OUTPUT); digitalWrite(PIN_LED_GREEN,  OFF);
  pinMode(PIN_BUZZER,    OUTPUT); digitalWrite(PIN_BUZZER,    OFF);
  if (!ACTUATOR_RED_CONFLICT) {
    pinMode(PIN_LED_RED, OUTPUT); digitalWrite(PIN_LED_RED, OFF);
  }
}
void applyActuators(int) {}
void setupLCD() {}
void serviceLcd() {}
void serviceTelegram() {}
void alertIfNeeded(const struct SensorData&, int, int) {}

#else   // ── PROFIL IMPLEMENTASI LAPANGAN (fitur lengkap) ──────────────────────

/* ══════════════════════════════════════════════════════════════════════════
   AKTUATOR LOKAL
   ══════════════════════════════════════════════════════════════════════════ */

const char* LEVEL_NAME[4] = { "AMAN", "HATI-HATI", "WASPADA", "BAHAYA" };
int  lastActuatorLevel = -1;      // -1 = belum pernah menerima status
int  lastAlertLevel    = -1;
unsigned long lastAlertMs = 0;
String pendingAlert = "";
unsigned long pendingAlertAt = 0;
unsigned long lastTelegramAttemptMs = 0;
unsigned long telegramRetryMs = 15000UL;   // backoff: 15→30→60→120 dtk saat gagal

// ── LCD 20x4 ──
LiquidCrystal_I2C* lcd = nullptr;     // null = LCD tidak terpasang/tidak ketemu
uint8_t  lcdAddrFound = 0;
String   lcdCache[LCD_ROWS];          // isi terakhir per baris (hindari flicker)
float    lcdSat = 0.0;
float    lcdDg = 0.0;
float    lcdTilt = 0.0;
int      lcdRssi = 0;
int      lcdLevel = -1;               // -1 = belum pernah terima paket
unsigned long lastLcdMs = 0;

/** Tulis satu kanal relay sesuai polaritas modul (aktif LOW pada umumnya). */
void setRelay(int pin, bool on) {
#if RELAY_ACTIVE_LOW
  digitalWrite(pin, on ? LOW : HIGH);
#else
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

// Status terakhir tiap kanal: 0=hijau, 1=kuning, 2=buzzer, 3=merah. -1=belum.
int8_t relayState[4] = {-1, -1, -1, -1};

// Tulis relay HANYA kalau statusnya berubah, dengan jeda 25 ms antar kanal.
// Menyalakan beberapa relay serentak membuat lonjakan arus pada 5 V/12 V yang
// bisa men-drop 3,3 V sesaat → modul LoRa/ESP32 brownout (radio tuli/reboot).
// Relay yang tidak berubah juga tidak perlu ditulis ulang tiap paket.
void applyRelayChannel(uint8_t idx, int pin, bool on) {
  int8_t want = on ? 1 : 0;
  if (relayState[idx] == want) return;
  setRelay(pin, on);
  relayState[idx] = want;
  delay(25);   // beri waktu catu daya pulih sebelum kanal berikutnya
}

void setupActuators() {
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  // Semua OFF sejak awal supaya relay tidak "klik" saat boot.
  setRelay(PIN_LED_YELLOW, false);
  setRelay(PIN_LED_GREEN, false);
  setRelay(PIN_BUZZER, false);
  relayState[0] = 0;
  relayState[1] = 0;
  relayState[2] = 0;
  if (!ACTUATOR_RED_CONFLICT) {
    pinMode(PIN_LED_RED, OUTPUT);
    setRelay(PIN_LED_RED, false);
    relayState[3] = 0;
  }
}

/** "BAHAYA" → 3, "WASPADA" → 2, "HATI-HATI" → 1, "AMAN" → 0, lainnya → -1. */
int statusToLevel(const char* status) {
  String s = String(status);
  s.trim();
  s.toUpperCase();
  if (s.startsWith("BAHAYA"))  return 3;
  if (s.startsWith("WASPADA")) return 2;
  if (s.startsWith("HATI"))    return 1;
  if (s.startsWith("AMAN"))    return 0;
  return -1;    // status tidak dikenal → jangan ubah apa pun
}

void applyActuators(int level) {
  if (level < 0) return;
  applyRelayChannel(0, PIN_LED_GREEN,  level == 0);
  applyRelayChannel(1, PIN_LED_YELLOW, level == 1 || level == 2);
  applyRelayChannel(2, PIN_BUZZER,     level == 2);
  // Kalau pin merah bentrok dengan MOSI LoRa, JANGAN ditulis: menulis pin itu
  // akan merusak jalur SPI. Relay merah (dan buzzer BAHAYA yang satu kanal)
  // tidak aktif sampai wiring diperbaiki — lihat WIRING.md.
  if (!ACTUATOR_RED_CONFLICT) {
    applyRelayChannel(3, PIN_LED_RED, level == 3);
  } else if (level == 3 && relayState[3] != 0) {
    relayState[3] = 0;   // jangan spam log tiap paket
    Serial.println("🚨 BAHAYA, tapi relay MERAH nonaktif: GPIO pin merah = LoRa MOSI. Perbaiki wiring!");
  }
  if (level != lastActuatorLevel) {
    Serial.printf("🔔 AKTUATOR → %s | hijau=%d kuning=%d merah=%d buzzer=%d\n",
                  LEVEL_NAME[level],
                  level == 0,
                  (level == 1 || level == 2),
                  level == 3,
                  level == 2);
    lastActuatorLevel = level;
  }
}

/* ══════════════════════════════════════════════════════════════════════════
   LCD 20x4 I2C
   ══════════════════════════════════════════════════════════════════════════
   Baris 1: identitas + status WiFi
   Baris 2: level bahaya terakhir dari TX
   Baris 3: saturasi tanah + getaran
   Baris 4: kemiringan + RSSI

   Semua kegagalan LCD bersifat opsional: kalau layar tidak terpasang, tidak
   ada alamat yang menjawab, atau kabel I2C lepas, firmware tetap jalan dan
   relay/Telegram tidak terpengaruh. */

/** Cari alamat backpack PCF8574: 0x27/0x3F paling umum, lalu scan 0x08–0x77. */
uint8_t scanLcdAddress() {
  const uint8_t umum[] = { 0x27, 0x3F, 0x20, 0x26, 0x38, 0x3E };
  for (uint8_t i = 0; i < sizeof(umum); i++) {
    Wire.beginTransmission(umum[i]);
    if (Wire.endTransmission() == 0) return umum[i];
  }
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

/** Tulis satu baris lengkap 20 karakter; dilewati kalau isinya sama. */
void lcdLine(uint8_t row, const String& text) {
  if (!lcd) return;
  String t = text;
  if (t.length() > LCD_COLS) t = t.substring(0, LCD_COLS);
  while (t.length() < LCD_COLS) t += ' ';
  if (lcdCache[row] == t) return;
  lcdCache[row] = t;
  lcd->setCursor(0, row);
  lcd->print(t);
}

void setupLCD() {
  Wire.begin(LCD_SDA, LCD_SCL);
  lcdAddrFound = scanLcdAddress();
  if (lcdAddrFound == 0) {
    Serial.printf("LCD 20x4       : tidak ditemukan (SDA=%d SCL=%d) — dilewati\n",
                  LCD_SDA, LCD_SCL);
    return;
  }
  lcd = new LiquidCrystal_I2C(lcdAddrFound, LCD_COLS, LCD_ROWS);
  lcd->begin(LCD_COLS, LCD_ROWS);
  lcd->backlight();
  lcd->clear();
  for (uint8_t i = 0; i < LCD_ROWS; i++) lcdCache[i] = "";
  Serial.printf("LCD 20x4       : OK di alamat 0x%02X (SDA=%d SCL=%d)\n",
                lcdAddrFound, LCD_SDA, LCD_SCL);

  lcdLine(0, "LADET RX  WiFi:--");
  lcdLine(1, "Status: MENUNGGU TX");
  lcdLine(2, "S:--  G:--");
  lcdLine(3, "Menunggu paket LoRa");
}

/** Dipanggil tiap loop; hanya menulis ulang kalau isi baris berubah. */
void serviceLcd() {
  if (!lcd) return;
  if (millis() - lastLcdMs < 500UL) return;
  lastLcdMs = millis();

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  lcdLine(0, String("LADET RX  WiFi:") + (wifiOk ? "OK" : "--"));

  String status;
  if (lcdLevel < 0) {
    status = "MENUNGGU TX";
  } else if (lastLoRaRxMs == 0 || millis() - lastLoRaRxMs > 30000UL) {
    // Relay tetap memegang level terakhir (disengaja demi keselamatan), tapi
    // layar jujur bilang TX sudah tidak mengirim.
    status = "TX HILANG";
  } else {
    status = LEVEL_NAME[lcdLevel];
  }
  lcdLine(1, "Status: " + status);

  // Angka detik di baris 4 terus bertambah = bukti RX masih hidup. Kalau
  // angkanya berhenti padahal listrik nyala, program RX yang macet.
  String up = String(millis() / 1000UL) + "s";
  if (lcdLevel >= 0) {
    lcdLine(2, "S:" + String(lcdSat, 1) + "% G:" + String(lcdDg, 2) + "g");
    lcdLine(3, up + " " + String(lcdRssi) + "dBm T:" + String(lcdTilt, 1));
  } else {
    lcdLine(2, "S:--  G:--");
    lcdLine(3, up + " menunggu paket");
  }
}

/* ══════════════════════════════════════════════════════════════════════════
   TELEGRAM (dikirim dari RX)
   ══════════════════════════════════════════════════════════════════════════ */

String urlEncode(const String& in) {
  String out;
  char buf[4];
  for (size_t i = 0; i < in.length(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

bool telegramConfigured() {
  return strlen(TELEGRAM_BOT_TOKEN) > 10 && strlen(TELEGRAM_CHAT_ID) > 0;
}

/** Kirim satu pesan ke Telegram. Blocking dibatasi ketat; hanya saat level berubah. */
bool sendTelegram(const String& text) {
  if (!telegramConfigured()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();                 // sertifikat tidak diverifikasi — cukup untuk skripsi

  // ⚠️ PENTING: nilai ini dalam MILIDETIK (bukan detik). Dulu `setTimeout(5)`
  // berarti TCP connect hanya diberi 5 ms → Telegram selalu gagal konek.
  client.setConnectionTimeout(2000);    // TCP connect maks 2 dtk
  client.setTimeout(2000);              // baca/tulis maks 2 dtk
  // TLS handshake default 120 dtk — inilah yang bisa membekukan seluruh loop
  // (LoRa tidak didengarkan selama itu). Dibatasi 4 dtk.
  client.setHandshakeTimeout(4);

  Serial.printf("📨 Telegram: menghubungi api.telegram.org... (heap %u B)\n",
                (unsigned)ESP.getFreeHeap());
  unsigned long tConnect = millis();
  if (!client.connect("api.telegram.org", 443)) {
    Serial.printf("📵 Telegram: gagal konek api.telegram.org setelah %lu ms\n",
                  millis() - tConnect);
    return false;
  }

  String body = "chat_id=" + urlEncode(TELEGRAM_CHAT_ID) + "&text=" + urlEncode(text);
  client.print("POST /bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage HTTP/1.1\r\n");
  client.print("Host: api.telegram.org\r\n");
  client.print("Content-Type: application/x-www-form-urlencoded\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  // Cukup baca baris status ("HTTP/1.1 200 OK").
  String statusLine;
  unsigned long t0 = millis();
  while (client.connected() && millis() - t0 < 5000) {
    if (!client.available()) { delay(10); continue; }
    statusLine = client.readStringUntil('\n');
    break;
  }
  client.stop();

  bool ok = statusLine.indexOf(" 200") > 0;
  Serial.printf("📨 Telegram: %s (%lu ms, heap %u B)\n",
                ok ? "terkirim" : statusLine.c_str(),
                millis() - tConnect, (unsigned)ESP.getFreeHeap());
  return ok;
}

/** Simpan pesan; dikirim sekarang kalau ada internet, atau nanti saat pulih. */
void queueAlert(const String& text) {
  if (!telegramConfigured()) {
    Serial.println("📵 Telegram belum diisi: salin secrets.example.h → secrets.h "
                   "lalu isi TELEGRAM_BOT_TOKEN & TELEGRAM_CHAT_ID");
    return;
  }
  // Simpan teks langsung ke pendingAlert; serviceTelegram() yang akan kirim.
  // Tidak ada antrian antrian agar loop tidak tertunda.
  pendingAlert = text;
  pendingAlertAt = millis();
  Serial.println("📮 Notifikasi masuk (akan dikirim saat loop berikutnya)");
}

/** Dipanggil tiap loop: coba kirim notifikasi Telegram, jangan spam percobaan. */
void serviceTelegram() {
  if (pendingAlert.length() == 0) return;
  if (!telegramConfigured()) { pendingAlert = ""; return; }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - pendingAlertAt > 30UL * 60UL * 1000UL) {
    pendingAlert = "";
    Serial.println("🗑️ Notifikasi kedaluwarsa (>30 mnt) — dibuang");
    return;
  }
  if (millis() - lastTelegramAttemptMs < telegramRetryMs) return;
  lastTelegramAttemptMs = millis();
  if (sendTelegram(pendingAlert)) {
    pendingAlert = "";
    telegramRetryMs = 15000UL;        // sukses → irama normal
  } else if (telegramRetryMs < 120000UL) {
    telegramRetryMs *= 2;             // gagal → backoff, jangan makan waktu loop
    Serial.printf("⏳ Telegram gagal — coba lagi %lu dtk lagi\n", telegramRetryMs / 1000UL);
  }
}

/**
 * Susun pesan hanya saat level BERUBAH (atau pengingat tiap 5 menit untuk
 * WASPADA/BAHAYA). Tanpa ini Telegram akan dibanjiri pesan tiap 2 detik.
 */
void alertIfNeeded(const struct SensorData& d, int level, int rssi) {
  // Level turun ke AMAN = alarm selesai. WAJIB menyimpan nilainya, kalau tidak
  // kejadian berikutnya (AMAN → WASPADA dalam <5 menit) dianggap "bukan baru"
  // dan notifikasinya tidak terkirim.
  if (level <= 0) { lastAlertLevel = level; return; }
  bool isNew  = (level != lastAlertLevel);
  bool remind = (level >= 2) && (millis() - lastAlertMs > 300000UL);
  if (!isNew && !remind) return;
  lastAlertLevel = level;
  lastAlertMs = millis();

  String msg;
  msg += (level >= 3) ? "🚨 " : "⚠️ ";
  msg += "LADET — ";
  msg += LEVEL_NAME[level];
  msg += "\nDesa Klungkung, Sukorambi, Jember\n";
  msg += "Saturasi   : " + String(d.sat * 100.0, 1) + " %\n";
  msg += "Getaran    : " + String(d.dg, 3) + " Δg\n";
  msg += "Kemiringan : " + String(d.dth, 2) + "°\n";
  msg += "Waktu      : " + nowStamp() + "\n";
  msg += "RSSI       : " + String(rssi) + " dBm";
  queueAlert(msg);
}

#endif  // LADET_PROFILE_THESIS

void publishConfigStatus(const char* state, bool txConfirmed) {
  String json = "{\"sf\":" + String(activeSF) +
                ",\"distance\":" + String(activeDistance) +
                ",\"tx_confirmed\":" + String(txConfirmed ? "true" : "false") +
                ",\"state\":\"" + state + "\"}";
  Serial.println("📤 Config status: " + json);
  if (!mqttClient.connected()) return;
  mqttClient.publish(CONFIG_STATUS_TOPIC, json.c_str());
}

bool channelQuiet() {
  // Hanya paket DATA yang dihitung. BEACON TX (tiap 12 detik) HANYA bukti TX
  // hidup. Di mode CONTINUOUS, "sunyi" = celah antar dua paket data (~2 dtk).
  return (millis() - lastDataRxMs) > CONFIG_QUIET_MS;
}

// ── Baca register SX1278 langsung lewat SPI ─────────────────────────────────
// readRegister() di library LoRa bersifat private, jadi dibaca manual di sini.
// HANYA untuk diagnostik: membaca register tidak mengubah state radio dan
// aman dilakukan saat radio sedang dalam mode RX (polling, tanpa interrupt).
uint8_t loraRegRead(uint8_t reg) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return value;
}

/** Tulis register SX1278 (kebalikan loraRegRead). */
void loraRegWrite(uint8_t reg, uint8_t value) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg | 0x80);
  SPI.transfer(value);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
}

// Register SX127x yang dipakai (sama dengan yang dipakai library LoRa).
#define SX_OP_MODE     0x01   // bit7=LongRangeMode, bit2..0=mode
#define SX_IRQ_FLAGS   0x12   // bit3=TxDone, bit6=RxDone
#define SX_MODE_TX     0x83   // LongRangeMode | TX
#define SX_IRQ_TX_DONE 0x08   // bit TxDone

/**
 * Kirim isi FIFO, TAPI dengan batas waktu.
 *
 * Ini pengganti `LoRa.endPacket()` yang menunggu TxDone tanpa batas waktu
 * (lihat LoRa.cpp — `while ((readRegister(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) == 0)
 * yield();`). Kalau radio tidak pernah melaporkan TxDone, versi library membuat
 * loop() membeku sampai watchdog me-reset ESP32 (90 dtk → ±40 paket hilang).
 * Fungsi ini menyerah setelah maxWaitMs dan mengembalikan false, jadi loop()
 * tidak pernah macet dan pemanggilnya bisa memulihkan radio lebih dulu.
 *
 * Urutannya sama dengan library: pastikan FIFO sudah diisi (beginPacket+print),
 * bersihkan flag lama, masuk mode TX, tunggu TxDone.
 */
bool loraTransmitTimed(unsigned long maxWaitMs) {
  loraRegWrite(SX_IRQ_FLAGS, 0xFF);   // buang flag lama (TxDone/RxDone dari paket sebelumnya)
  loraRegWrite(SX_OP_MODE, SX_MODE_TX);
  unsigned long t0 = millis();
  while ((loraRegRead(SX_IRQ_FLAGS) & SX_IRQ_TX_DONE) == 0) {
    if (millis() - t0 > maxWaitMs) return false;
    if (wdtArmed) esp_task_wdt_reset();
    delay(1);
  }
  loraRegWrite(SX_IRQ_FLAGS, SX_IRQ_TX_DONE);  // clear TxDone
  return true;
}

// Init ulang radio dari nol (reset pin + SPI + parameter LoRa) TANPA
// me-restart ESP32. Dipakai kalau chip terbaca keluar dari mode LoRa atau
// tuli terlalu lama — supaya relay & uptime tetap jalan, tidak ikut reboot.
void recoverLoRa(const char* reason) {
  Serial.printf("♻️  RADIO: init ulang (%s) — OP_MODE=0x%02X VER=0x%02X\n",
                reason, loraRegRead(0x01), loraRegRead(0x42));
  lastRadioRecoverMs = millis();
  LoRa.end();          // sleep + SPI.end()
  delay(20);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);

  bool ok = false;
  for (int i = 1; i <= 3 && !ok; i++) {
    ok = LoRa.begin(LORA_FREQ);
    if (!ok) {
      Serial.printf("   LoRa.begin() percobaan %d/3 gagal\n", i);
      delay(100);
    }
  }
  if (!ok) {
    Serial.println("❌ RADIO: begin() tetap gagal — cek 3V3 modul LoRa & kabel SPI.");
    return;
  }

  LoRa.setSpreadingFactor(activeSF);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();
  LoRa.setTxPower(ACK_TX_POWER);   // ACK hemat arus → tidak memicu brownout
  LoRa.receive();
  radioBadChecks = 0;
  Serial.printf("   ✅ RADIO pulih — SF=%d, siap menerima (OP_MODE=0x%02X)\n",
                activeSF, loraRegRead(0x01));
}

// Dipanggil tiap loop. Cepat (2 register read tiap 2 dtk) dan tidak pernah
// memotong paket yang sedang datang: hanya bertindak kalau chip benar-benar
// tidak sehat atau sudah tuli jauh lebih lama dari interval kirim TX (2 dtk).
void superviseLoRa() {
  unsigned long now = millis();
  if (now - lastRadioCheckMs < RADIO_CHECK_MS) return;
  lastRadioCheckMs = now;

  uint8_t version   = loraRegRead(0x42);            // 0x12 = SX1278 dikenal
  uint8_t opMode    = loraRegRead(0x01);            // bit7 = LongRangeMode
  bool    present   = (version == 0x12);
  bool    longRange = (opMode & 0x80) != 0;

  if (!present || !longRange) {
    radioBadChecks++;
    Serial.printf("⚠️  RADIO tidak sehat: VER=0x%02X OP_MODE=0x%02X (mode LoRa=%s) cek %u/2\n",
                  version, opMode, longRange ? "ya" : "TIDAK",
                  (unsigned)radioBadChecks);
    if (radioBadChecks >= 2 && now - lastRadioRecoverMs > RADIO_COOLDOWN_MS) {
      recoverLoRa(!present ? "chip tidak merespons SPI" : "chip keluar dari mode LoRa");
    }
    return;
  }
  radioBadChecks = 0;

  // Belum pernah ada paket sama sekali (TX belum dinyalakan / terlalu jauh):
  // jangan re-init terus-menerus, cukup tunggu.
  if (!radioEverHeard) return;

  unsigned long silence = now - lastLoRaRxMs;
  // Ambang "tuli" HARUS mengikuti SF. Di SF12 satu paket butuh ±7–8 dtk, jadi
  // ambang 9 dtk akan menganggap radio tuli padahal TX normal — dan re-init
  // justru membuang paket yang sedang datang. SF9/SF12 memang lebih lambat.
  unsigned long deafMs = (activeSF >= 12) ? 30000UL : ((activeSF >= 9) ? 15000UL : 9000UL);
  if (silence < deafMs) return;

  // Jangan potong handshake config yang sedang menunggu ACK=CONF.
  if (pendingSF >= 0) return;

  // Setelah 3 percobaan beruntun tetap sunyi, jarangkan jadi 1 menit sekali
  // supaya log tidak banjir saat TX memang dimatikan di bangku uji.
  // Maksimum 15 dtk (dulu 60). Kalau radio benar-benar tuli, setiap menit yang
  // dihabiskan untuk menunggu = ±28 paket hilang — terlalu mahal. Pemulihan
  // radio murah (±100 ms), jadi lebih baik sering mencoba.
  unsigned long cooldown = (radioRecoverAttempts >= 3) ? 15000UL : RADIO_COOLDOWN_MS;
  if (now - lastRadioRecoverMs < cooldown) return;

  radioRecoverAttempts++;
  Serial.printf("⚠️  Tuli %lu dtk padahal TX tadinya aktif — coba pulihkan radio (%u)\n",
                silence / 1000UL, (unsigned)radioRecoverAttempts);
  recoverLoRa("tuli terlalu lama");
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("📩 MQTT config: %s = %s\n", topic, msg.c_str());

  String t = String(topic);
  if (t == "ladet/config/sf") {
    // Ganti SF dari web dinonaktifkan: SF dikontrol manual via #define LORA_SF.
    publishConfigStatus("rejected", false);
    return;
  } else if (t == "ladet/config/distance") {
    int d = msg.toInt();
    if (d <= 0) return;
    pendingDistance = d;
    publishConfigStatus("queued", false);
  }
}

// Dipanggil saat kanal sunyi: realisasikan perubahan jarak dari web jika ada
void processConfigQueue() {
  if (pendingDistance < 0) return;
  if (!channelQuiet()) return;

  activeDistance = pendingDistance;
  pendingDistance = -1;
  Serial.printf("⚙️ CONFIG: jarak aktif -> %d m (metadata lokal RX)\n", activeDistance);
  publishConfigStatus("applied", false);
}

// ══════════════════════════════════════════════════════════════════════════════
// PACKET LOSS & DELAY TRACKING
// ══════════════════════════════════════════════════════════════════════════════
int lastReceivedSEQ = -1;
unsigned long totalExpected = 0;
unsigned long totalReceived = 0;
bool firstPacketReceived = false;

// ─── Gap SEQ paket terakhir (0 = berurutan) ───
// Dikirim ke dashboard supaya CSV bisa menunjukkan di baris mana paket hilang.
int lastSeqGap = 0;

// ─── Loss tracking tahan reboot RX ───
// totalExpected/totalReceived/lastReceivedSEQ disimpan berkala ke NVS. Setelah
// RX reboot, hitungan TIDAK mulai dari 1 lagi: gap SEQ dari TX tetap dihitung,
// jadi packet loss tidak "hilang" hanya karena gateway sempat restart.
bool lossRestored = false;
unsigned long lastLossSaveMs = 0;
const unsigned long LOSS_SAVE_INTERVAL_MS = 30000UL;

// ─── Nomor terima RX (monoton naik, TIDAK pernah di-reset) ───
// Jumlah paket LoRa yang benar-benar diterima RX, dipakai dashboard sebagai
// kunci anti-duplikat. SENGAJA tidak di-reset saat pengujian baru terdeteksi:
// kalau di-reset, kunci akan kembali ke rx#1 dan paket-paket awal run kedua
// dianggap duplikat oleh dashboard (tabel & grafik berhenti di beberapa baris).
unsigned long rxCount = 0;

// ─── Deteksi SEQ TX MACET ───
// Firmware TX versi lama mengirim nilai SEQ yang sama terus (mis. selalu
// SEQ=1). Kalau itu terjadi, selisih SEQ tidak bisa dipakai untuk menghitung
// packet loss. RX menghitung berapa kali berturut-turut SEQ tidak maju; kalau
// sudah SEQ_STUCK_STREAK kali, tandai paket sebagai seq_stuck supaya dashboard
// bisa memberi peringatan "upload ulang firmware TX".
int sameSeqStreak = 0;
bool seqStuck = false;
const int SEQ_STUCK_STREAK = 3;

// ─── Simpan/pulihkan state loss di NVS (tahan reboot RX) ───
// Ditulis maksimal tiap 30 dtk supaya flash tidak cepat aus (paket tiap ~3 dtk
// = ~28.800 tulis/hari kalau tiap paket → flash rusak dalam hitungan hari).
void loadLossState() {
  prefs.begin("ladet", true);
  int32_t  lseq = prefs.getInt("lseq", -1);
  uint32_t lexp = prefs.getUInt("lexp", 0);
  uint32_t lrec = prefs.getUInt("lrec", 0);
  prefs.end();
  if (lseq >= 0 && lexp > 0 && lrec > 0 && lrec <= lexp) {
    lastReceivedSEQ = (int)lseq;
    totalExpected   = lexp;
    totalReceived   = lrec;
    lossRestored    = true;
    Serial.printf("💾 Loss dilanjutkan dari NVS: SEQ %ld, %lu/%lu\n",
                  (long)lseq, totalReceived, totalExpected);
  }
}

void saveLossState(bool force) {
  if (!force && millis() - lastLossSaveMs < LOSS_SAVE_INTERVAL_MS) return;
  if (lastReceivedSEQ < 0) return;
  lastLossSaveMs = millis();
  prefs.begin("ladet", false);
  prefs.putInt("lseq", (int32_t)lastReceivedSEQ);
  prefs.putUInt("lexp", (uint32_t)totalExpected);
  prefs.putUInt("lrec", (uint32_t)totalReceived);
  prefs.end();
}

// Pengujian baru (SEQ TX kembali ke 1 / mundur) = epoch baru → mulai dari nol
// dan hapus state lama supaya tidak tercampur dengan run sebelumnya.
void clearLossState() {
  prefs.begin("ladet", false);
  prefs.remove("lseq");
  prefs.remove("lexp");
  prefs.remove("lrec");
  prefs.end();
  lossRestored = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// DELAY MEASUREMENT — KALIBRASI OFFSET JAM TX <-> RX
// ═════════════════════════════════════════════════════════════════════════════
// millis() TX dan RX berjalan sendiri-sendiri (titik nol = waktu boot masing2).
// Jadi   rxTime - send_ms   =   delay_asli + offset_boot.
// Kalau offset_boot TIDAK dikurangi, yang terbaca = uptime RX yang naik terus
// ~1000 ms tiap detik (inilah penyebab "delay" membengkak sampai ratusan detik).
//
// Ada 3 cara mendapatkan offset_boot, dipakai berurutan sesuai ketersediaan:
//
//   "rtt"        → offset dari RTT yang dikirim TX  (ABSOLUT, paling akurat)
//                  butuh firmware TX yang mengirim field RTT=
//   "clock"      → offset hasil kalibrasi + refine RTT (ABSOLUT)
//   "clock-rel"  → offset = minimum rawDiff yang pernah terlihat (RELATIF)
//                  dipakai kalau TX belum mengirim RTT=; hasilnya delay
//                  relatif terhadap paket tercepat, TIDAK PERNAH membengkak
//   "invalid"    → di luar rentang wajar, dibuang dashboard
//
// Catatan: delay satu arah yang ABSOLUT secara teori tidak bisa dihitung dari
// selisih dua jam yang tidak tersinkron (2 variabel, 1 persamaan). Karena itu
// RTT dari TX adalah satu-satunya sumber absolut, dan "clock-rel" adalah
// jaring pengaman supaya angka delay tidak pernah salah total.

bool   clockOffsetValid    = false;
long   clockOffsetMs       = 0;
long   prevRawDiffMs       = -1;        // (rxTime-send_ms) paket sebelumnya
bool   clockOffsetRefined  = false;     // sudah diperbaiki pakai RTT?
long   rawDiffMin          = 0;         // running minimum rawDiff (fallback relatif)
bool   rawDiffMinValid     = false;
bool   rttSeen             = false;     // sudah pernah terima RTT= dari TX?
bool   rttMissingWarned    = false;     // sudah dicetak peringatan "TX belum kirim RTT="?

// Batas kewajaran delay (ms) — HARUS mengikuti SF.
// Satu paket data SF12 butuh ±7 dtk di udara, jadi RTT (data + ACK) ±8 dtk dan
// delay satu arah dari RTT/2 ±4 dtk. Dengan batas tetap 5 dtk seperti versi
// sebelumnya, SELURUH jalur RTT ditolak di SF12 → delay absolut tidak pernah
// aktif. Nilai ini gunanya hanya membuang angka yang jelas salah (mis. delay
// yang membengkak mengikuti uptime RX), bukan menilai mutu jaringan.
unsigned long delaySanityMaxMs() {
  if (activeSF >= 12) return 25000UL;
  if (activeSF >= 9)  return 12000UL;
  return 5000UL;
}
const char*         delaySource         = "unknown"; // rtt | clock | clock-rel | invalid

// ══════════════════════════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════════════════════════════

void setup() {
  // PERTAMA: amankan relay sebelum apa pun. Kalau pin dibiarkan mengambang
  // (high-Z) selama Serial/NVS/LoRa init, modul relay tertentu bisa menganggap
  // inputnya aktif dan SEMUA relay menyala saat boot. setupActuators() menulis
  // level OFF ke tiap kanal secepat mungkin.
  setupActuators();

  Serial.begin(115200);
  // Jangan menunggu USB host selamanya: pada board dengan USB native (S3/C3)
  // `while (!Serial)` menggantung kalau tidak ada Serial Monitor, sehingga alat
  // tidak pernah jalan saat dipasang di lapangan tanpa laptop.
  unsigned long serialWait = millis();
  while (!Serial && millis() - serialWait < 1500UL) delay(10);

  // Hitung boot (disimpan di NVS): kalau angkanya naik terus, ada yang
  // menyebabkan RX reboot berulang — mudah dicek tanpa membuka Serial.
  prefs.begin("ladet", false);
  bootCount = (uint16_t)(prefs.getUShort("boots", 0) + 1);
  prefs.putUShort("boots", bootCount);
  prefs.end();

  // Pulihkan hitungan packet loss dari NVS supaya reboot RX di tengah
  // pengujian TIDAK membuat counter kembali 1/1. Kalau NVS kosong/rusak,
  // fungsi ini tidak melakukan apa-apa (mulai dari nol seperti biasa).
  loadLossState();

  Serial.println();
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println("  LADET RX — Gateway MQTT Publisher");
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println();
  Serial.printf("Reset terakhir : %s\n", resetReasonName());
  // Kalau RX reboot sendiri di tengah pengujian, baris ini menyebut sebabnya
  // tanpa perlu membuka MQTT Explorer: BROWNOUT = catu daya, TASK_WDT = loop
  // membeku (mis. menunggu TxDone ACK), PANIC = kesalahan perangkat lunak.
  switch (esp_reset_reason()) {
    case ESP_RST_BROWNOUT:
      Serial.println("   → catu daya turun saat RX bekerja. Cek 3V3 modul LoRa & kabel,"
                     " tambahkan kapasitor 100 µF di rel 3V3.");
      break;
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
      Serial.println("   → loop membeku melebihi ambang watchdog (paling sering:"
                     " menunggu TxDone saat mengirim ACK).");
      break;
    case ESP_RST_PANIC:
      Serial.println("   → exception perangkat lunak. Kirim backtrace di atas untuk ditelusuri.");
      break;
    default: break;
  }
  Serial.printf("Boot ke-       : %u\n", bootCount);
  // Penanda versi: memastikan board benar-benar menjalankan sketch yang baru
  // di-upload (bukan firmware lama yang masih tersimpan).
  Serial.printf("Firmware build : %s %s\n", __DATE__, __TIME__);
  Serial.println();
  Serial.println("TEST CONFIGURATION:");
  Serial.printf("  Distance : %d m\n", TEST_DISTANCE_M);
  Serial.printf("  SF       : %d (HARUS sama dengan TX!)\n", LORA_SF);
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println();

#if LADET_PROFILE_THESIS
  Serial.println("Profil firmware: SKRIPSI (LoRa + MQTT saja)");
  Serial.println("                 LCD/relay/buzzer/Telegram NONAKTIF");
#else
  // Relay sudah diamankan di awal setup(); di sini hanya melaporkan statusnya.
  Serial.printf("Aktuator       : merah=%d kuning=%d hijau=%d buzzer=%d (%s)\n",
                PIN_LED_RED, PIN_LED_YELLOW, PIN_LED_GREEN, PIN_BUZZER,
                RELAY_ACTIVE_LOW ? "relay aktif LOW" : "relay aktif HIGH");
  if (ACTUATOR_RED_CONFLICT) {
    Serial.println("⚠️  PERINGATAN BENTROK PIN:");
    Serial.printf("    PIN_LED_RED (%d) dipakai jalur LoRa (MOSI=%d MISO=%d SCK=%d SS=%d).\n",
                  PIN_LED_RED, LORA_MOSI, LORA_MISO, LORA_SCK, LORA_SS);
    Serial.println("    → Kanal MERAH dinonaktifkan sampai wiring diperbaiki.");
    Serial.println("    → Pindahkan kabel input relay merah ke GPIO32, lalu ubah");
    Serial.println("      PIN_LED_RED menjadi 32. Detail: firmware/WIRING.md");
  }
#endif
  Serial.println();

  setupLCD();            // LCD 20x4 I2C (opsional; tidak menghambat relay)
  setupWiFi();
  setupClock();          // NTP — pengganti RTC untuk cap waktu
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  // WAJIB >= ukuran JSON (1536) + panjang topik + header MQTT (~26 byte).
  // Kalau buffer lebih kecil dari payload, PubSubClient menolak publish
  // (return false) TANPA error jelas → broker tidak pernah menerima ladet/data
  // dan dashboard terus "MENUNGGU RX" walau LoRa & WiFi sehat.
  mqttClient.setBufferSize(2048);
  mqttClient.setCallback(onMqttMessage);
  // CATATAN: jangan panggil espClient.setTimeout() di sini. Di ESP32 nilai itu
  // juga dipakai sebagai batas TCP connect (default 3000 ms). Kalau diperkecil
  // ke 2 dtk, MQTT gagal connect ke broker publik dan dashboard terus
  // "menunggu RX". Biarkan default.
  setupLoRa();

  // SF manual murni: selalu gunakan #define LORA_SF (hapus NVS lama jika ada)
  activeSF = LORA_SF;
  LoRa.setSpreadingFactor(activeSF);
  prefs.begin("ladet", false);
  prefs.remove("sf");
  prefs.end();
  Serial.printf("📡 LoRa SF aktif: %d (sesuai #define LORA_SF)\n", activeSF);
  
  Serial.println("✅ RX Gateway siap!");
  Serial.printf("📡 MQTT Topic: %s\n", MQTT_TOPIC);
  Serial.printf("📍 Test Distance: %d m\n", TEST_DISTANCE_M);
  setupLoopWatchdog();   // mulai pantau loop SETELAH boot selesai
  
#ifdef MQTT_TEST_MODE
  Serial.println();
  Serial.println("⚠️  MODE: MQTT TEST (no LoRa) — Publishing synthetic data every 5s");
  Serial.println("    To disable: comment out #define MQTT_TEST_MODE");
#endif
  
  Serial.println();
}

#ifdef MQTT_TEST_MODE
// ══════════════════════════════════════════════════════════════════════════════
// TEST 0 — MQTT TEST FUNCTION (synthetic data)
// ══════════════════════════════════════════════════════════════════════════════
void publishTestPacket() {
  SensorData testData;
  
  // Hardcoded test values for verification
  strcpy(testData.type, "DATA");
  testData.seq = 999;
  strcpy(testData.time, "12:34:56");
  testData.soil_raw = 2450;
  testData.soil_mv = 1225;
  testData.theta = 0.35;
  testData.theta_r = 0.10;
  testData.theta_s = 0.45;
  testData.sat = 0.4676;  // Target: 46.76%
  testData.g0 = 1.00;
  testData.g = 1.004;
  testData.dg = 0.004;    // Target: 0.004 Δg
  testData.tx = 0.5;
  testData.ty = -0.3;
  testData.tz = 358.2;
  testData.dtx = 0.1;
  testData.dty = 0.0;
  testData.dtz = 0.2;
  testData.dth = 0.6;     // Target: 0.6°
  strcpy(testData.status, "AMAN");  // Target: "aman"
  testData.sf = 7;        // Target: "SF7"
  
  int testRssi = -67;     // Target: -67 dBm
  float testSnr = 9.5;    // Target: 9.5 dB
  
  Serial.println();
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println("🧪 TEST 0 — Publishing synthetic packet");
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println("Expected website values:");
  Serial.println("  Kelembaban Tanah: 46.76 %");
  Serial.println("  Getaran:          0.004 Δg");
  Serial.println("  Kemiringan:       0.6 °");
  Serial.println("  Status:           aman (green card)");
  Serial.println("  SF:               SF7");
  Serial.println("  RSSI:             -67 dBm");
  Serial.println("  SNR:              9.5 dB");
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println();
  
  publishToMQTT(testData, testRssi, testSnr, millis(), 50, 0.0);
}
#endif

void loop() {
  if (wdtArmed) esp_task_wdt_reset();   // bukti loop masih berputar

  // ── Pemulihan diri TANPA reboot ──
  // Dulu RX reboot sendiri kalau WiFi 30 mnt / MQTT 5 mnt tidak tersambung.
  // Akibatnya tiap ±5 menit ada lubang data ±20 dtk (waktu boot + WiFi) dan
  // nomor paket melompat, seolah-olah ada kehilangan yang tidak tercatat.
  // Sekarang jaringan hanya dicoba ulang terus-menerus; ESP32 TIDAK PERNAH
  // di-reboot karena masalah jaringan. Reset hanya untuk kondisi darurat
  // (loop benar-benar membeku > 90 dtk, ditangani Task Watchdog).
  if (WiFi.status() == WL_CONNECTED) lastWifiOkMs = millis();
  if (mqttClient.connected()) {
    lastWifiOkMs = millis();
    lastMqttOkMs = millis();
  }
  static unsigned long lastNetWarnMs = 0;
  if (millis() - lastNetWarnMs > 600000UL) {   // peringatan maks 1x / 10 mnt
    if (millis() - lastWifiOkMs > WIFI_RESTART_MS) {
      lastNetWarnMs = millis();
      Serial.println("⚠️  WiFi putus > 30 mnt — tetap mencoba, TANPA reboot (sirine/LED dari LoRa tetap jalan)");
    } else if (WiFi.status() == WL_CONNECTED && millis() - lastMqttOkMs > MQTT_RESTART_MS) {
      lastNetWarnMs = millis();
      Serial.println("⚠️  MQTT tidak tersambung > 5 mnt — tetap mencoba, TANPA reboot");
    }
  }

  // Antrian notifikasi Telegram & LCD dijalankan SETELAH paket LoRa diproses
  // (lihat akhir loop) supaya TLS Telegram yang lambat tidak menunda ACK/alarm.

  // ── Detektor loop macet (diagnostik) ──
  // Kalau satu iterasi loop memakan > 1,5 dtk, hampir pasti ada panggilan
  // jaringan yang memblokir (DNS/TLS Telegram, MQTT, NTP). Baris ini membuat
  // penyebabnya terlihat di Serial, bukan cuma "tiba-tiba berhenti".
  static unsigned long lastLoopMs = 0;
  unsigned long nowLoop = millis();
  if (lastLoopMs != 0 && nowLoop - lastLoopMs > 1500UL) {
    Serial.printf("⚠️  Loop tertahan %lu ms (heap %u B, WiFi %s, MQTT %s)\n",
                  nowLoop - lastLoopMs, (unsigned)ESP.getFreeHeap(),
                  WiFi.status() == WL_CONNECTED ? "OK" : "putus",
                  mqttClient.connected() ? "OK" : "putus");
  }
  lastLoopMs = nowLoop;

  // ── Denyut "RX masih hidup" ──
  // Hanya dicetak saat TIDAK ada paket LoRa, supaya tidak mengganggu log run.
  // Gunanya: membedakan "TX berhenti mengirim" dari "RX mati/hang".
  // Kalau baris ini terus muncul → RX sehat, masalahnya di TX atau radio.
  // Kalau Serial benar-benar diam → RX hang/reset (atau kabel USB lepas).
  static unsigned long lastAliveMs = 0;
  if (millis() - lastAliveMs > 5000UL) {
    lastAliveMs = millis();
    // Ambang "sunyi" HARUS ikut SF. Di SF12 jeda antar paket ±10 dtk itu NORMAL
    // (airtime sendiri ±7 dtk), jadi ambang tetap 6 dtk akan mencetak denyut
    // "RX hidup" terus-menerus dan menenggelamkan baris log run yang penting.
    unsigned long silentLimit = (activeSF >= 12) ? 15000UL
                              : (activeSF >= 9)  ? 9000UL : 6000UL;
    bool silent = (lastLoRaRxMs == 0) || (millis() - lastLoRaRxMs > silentLimit);
    if (silent) {
      unsigned long silentSec = (millis() - lastLoRaRxMs) / 1000UL;
      Serial.printf("⏱️  RX hidup — %lu dtk tanpa paket LoRa (heap %u B, WiFi %s, MQTT %s, RADIO=0x%02X)\n",
                    silentSec, (unsigned)ESP.getFreeHeap(),
                    WiFi.status() == WL_CONNECTED ? "OK" : "putus",
                    mqttClient.connected() ? "OK" : "putus",
                    loraRegRead(0x01));
    }
  }

  serviceWiFi();   // WiFi pulih tanpa reboot

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    // Backoff: 5 → 10 → 20 → 40 → 60 dtk. Satu percobaan connect bisa
    // memblokir beberapa detik (DNS/TCP); mencobanya tiap 5 dtk terus-menerus
    // membuat loop hampir selalu tersangkut dan paket LoRa hilang beruntun.
    if (now - lastReconnectAttempt > mqttRetryMs) {
      lastReconnectAttempt = now;
      if (reconnectMQTT()) {
        mqttRetryMs = 5000UL;
      } else if (mqttRetryMs < 15000UL) {
        mqttRetryMs = (mqttRetryMs * 2UL > 15000UL) ? 15000UL : mqttRetryMs * 2UL;
      }
    }
  } else {
    mqttRetryMs = 5000UL;
    mqttClient.loop();
    // Kirim ulang paket yang sempat gagal saat WiFi/MQTT putus. Dibatasi per
    // putaran supaya loop() tidak memblokir penerimaan LoRa terlalu lama.
    flushPending();
    processConfigQueue();   // config remote: terapkan antrian saat kanal kosong
    // ── Heartbeat kehadiran (WAJIB) ──
    // Saat RX di-reset, broker baru sadar socket lama mati ~20-30 dtk kemudian
    // dan menulis will "offline" (retained) — bisa menimpa "online" yang baru
    // saja dipublish RX yang sudah hidup. Heartbeat tiap 20 dtk memastikan
    // broker kembali jujur dengan cepat.
    if (millis() - lastOnlinePublish > 20000UL) {
      lastOnlinePublish = millis();
      mqttClient.publish(RX_ONLINE_TOPIC, "online", true);
      mqttClient.publish(TX_ONLINE_TOPIC, txOnlinePublished ? "online" : "offline", true);
    }
    // TX dianggap hilang kalau 30 dtk tidak ada paket LoRa sama sekali
    if (txOnlinePublished && (millis() - lastLoRaRxMs) > 30000UL) {
      mqttClient.publish(TX_ONLINE_TOPIC, "offline", true);
      txOnlinePublished = false;
      Serial.println("📡 30 dtk tanpa paket LoRa → ladet/tx/online = offline");
    }
    // Diagnostik tiap 1 menit (reset terakhir, uptime, heap, RSSI).
    if (millis() - lastHealthMs > HEALTH_INTERVAL_MS) {
      lastHealthMs = millis();
      publishHealth();
    }
  }
  
#ifdef MQTT_TEST_MODE
  // TEST 0: Publish synthetic data every 5 seconds
  static unsigned long lastTestPublish = 0;
  if (millis() - lastTestPublish > 5000) {
    publishTestPacket();
    lastTestPublish = millis();
  }
#else
  // Normal mode: Check for LoRa packets
  superviseLoRa();   // jaga radio tetap sehat (deteksi chip "mati diam-diam")
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    handleLoRaPacket(packetSize);
  }
#endif

  // Baru setelah paket LoRa dilayani: antrian Telegram & layar. Kalau TLS
  // Telegram lambat, ACK paket berikutnya tetap terkirim tepat waktu.
  serviceTelegram();
  serviceLcd();

  // Beri makan watchdog SETELAH satu putaran loop selesai dengan benar.
  // Tanpa ini, task yang terdaftar di WDT bisa dianggap "macet" walau loop
  // sehat — dan itu memicu reboot di tengah pengujian skripsi.
  if (wdtArmed) esp_task_wdt_reset();
}

void setupWiFi() {
  Serial.print("🌐 Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  // Matikan WiFi power-save: RX harus selalu siaga menerima TCP/MQTT. Mode
  // hemat daya membuat kartu WiFi tidur di antara beacon → publish tertahan
  // dan koneksi MQTT lebih mudah putus (RSSI -79 dBm sudah lemah).
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("✅ WiFi terhubung!");
    Serial.print("   IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("❌ WiFi gagal terhubung!");
  }
}

/**
 * Kembalikan WiFi TANPA me-restart ESP32. Dipanggil tiap loop.
 * Reboot karena jaringan adalah penyebab lubang data yang paling merugikan:
 * satu reboot = ±20 dtk tidak mendengarkan + nomor boot berubah sehingga CSV
 * menandai paket-paket berikutnya sebagai "RX restart".
 */
void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetryMs < WIFI_RETRY_MS) return;
  lastWifiRetryMs = millis();
  Serial.println("🌐 WiFi putus — coba sambung ulang (tanpa reboot)");
  WiFi.disconnect(false);   // non-blocking, jangan tunggu
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void setupLoRa() {
  Serial.print("📡 Menginisialisasi LoRa...");
  
  // Initialize SPI with explicit pins
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  // Dulu: `while (1);` → RX diam total sampai listriknya dicabut, dan WDT
  // tidak menolong karena loop-nya tidak memberi makan apa pun tapi juga tidak
  // pernah "membeku" di dalam delay. Sekarang dicoba ulang lalu restart sendiri.
  bool loraOk = false;
  for (int i = 1; i <= 5 && !loraOk; i++) {
    loraOk = LoRa.begin(LORA_FREQ);
    if (!loraOk) {
      Serial.printf("\n   LoRa gagal (percobaan %d/5)", i);
      delay(500);
    }
  }
  if (!loraOk) {
    Serial.println("\n❌ LoRa tidak merespons 5x — restart otomatis.");
    Serial.println("   Cek wiring: SCK=18 MISO=19 MOSI=23 NSS=5 RST=14 DIO0=4, dan 3V3.");
    Serial.flush();
    delay(300);
    ESP.restart();
  }
  
  // LoRa parameters (sama dengan TX)
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();  // CRC enabled (sama dengan TX)
  LoRa.setTxPower(ACK_TX_POWER);  // ACK saja — 10 dBm cukup, arus lebih kecil
  
  Serial.println(" ✅");
  Serial.println("   LoRa Configuration:");
  Serial.printf("   Frequency : %.1f MHz\n", LORA_FREQ / 1E6);
  Serial.printf("   SF        : %d\n", LORA_SF);
  Serial.println("   BW        : 125 kHz");
  Serial.println("   CR        : 4/5");
  Serial.println("   CRC       : Enabled");
  Serial.println("   Sync Word : 0x12");
  Serial.println();
  Serial.println("   Wiring:");
  Serial.println("   SCK       : GPIO18");
  Serial.println("   MISO      : GPIO19");
  Serial.println("   MOSI      : GPIO23 (RX — beda dengan TX yang memakai 25)");
  Serial.println("   NSS       : GPIO5");
  Serial.println("   RST       : GPIO14");
  Serial.println("   DIO0      : GPIO4");
}

boolean reconnectMQTT() {
  String clientId = "LADET-RX-" + String(random(0xffff), HEX);
  
  // LWT (Last Will & Testament): kalau RX mati tanpa sempat disconnect,
  // broker otomatis mem-publish "offline" (retained) ke ladet/rx/online —
  // jadi dashboard tahu RX mati, tidak cuma melihat status browser↔broker.
  if (mqttClient.connect(clientId.c_str(), RX_ONLINE_TOPIC, 1, true, "offline")) {
    Serial.println("✅ MQTT terhubung!");
    // Config remote: berlangganan topik config dari web (retained → RX langsung
    // tahu config terakhir walau baru nyala/reconnect).
    mqttClient.subscribe("ladet/config/sf");
    mqttClient.subscribe("ladet/config/distance");
    // Umumkan RX online (retained → dashboard yang baru dibuka pun tahu)
    mqttClient.publish(RX_ONLINE_TOPIC, "online", true);
    // Umumkan state config RX saat ini ke dashboard
    publishConfigStatus("idle", true);
    // Diagnostik pertama setelah (re)connect: penyebab reset & jumlah boot.
    publishHealth();
    lastHealthMs = millis();
    return true;
  } else {
    Serial.print("❌ MQTT gagal, rc=");
    Serial.println(mqttClient.state());
    return false;
  }
}

void handleLoRaPacket(int packetSize) {
  String rawPacket = "";
  while (LoRa.available()) {
    rawPacket += (char)LoRa.read();
  }

  // Tangkap rxTime SEGERA setelah paket diterima (sebelum Serial.print)
  unsigned long rxTime = millis();
  lastLoRaRxMs = rxTime;   // config remote: penanda kanal sedang dipakai run
  lastDataRxMs = rxTime;   // PAKET DATA = run benar-benar sedang berjalan
  radioEverHeard = true;   // radio terbukti sehat — penjaga tidak perlu re-init
  radioRecoverAttempts = 0;
  if (!txOnlinePublished) {  // TX baru terlihat → laporkan ke dashboard (retained)
    txOnlinePublished = true;
  }

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  
  SensorData data = parsePacket(rawPacket);

  // ══════════════════════════════════════════════════════════════════════
  // KIRIM ACK SEGERA — untuk SEMUA paket, TERMASUK #0 (kalibrasi)!
  // RTT diukur oleh TX dari kirim packet sampai terima ACK.
  // ACK #0 khusus penting: RTT-nya disertakan TX di paket #1 sehingga RX
  // bisa mengunci offset ABSOLUT sejak paket pertama → delay #1 rill, bukan 0.
  //
  // ⚠️ JANGAN kembali memakai LoRa.endPacket() di sini. Itu fungsi library yang
  // menunggu TxDone TANPA batas waktu; kalau radio tersendat, loop() membeku dan
  // watchdog me-reset ESP32 setelah 30–90 dtk → puluhan paket hilang sekaligus
  // (di dashboard tampil sebagai "RX restart/offline"). loraTransmitTimed()
  // melakukan hal yang sama tetapi menyerah setelah ACK_TX_TIMEOUT_MS.
  // ══════════════════════════════════════════════════════════════════════
#if RX_SEND_ACK
  String ack = "ACK=" + String(data.seq);
  bool ackOk = false;
  LoRa.idle();                 // STANDBY sebelum mengisi FIFO
  if (LoRa.beginPacket()) {
    LoRa.print(ack);
    ackOk = loraTransmitTimed(ACK_TX_TIMEOUT_MS);
  }
  if (ackOk) {
    LoRa.idle();               // beri PA waktu turun
    delay(2);
    LoRa.receive();            // kembali mendengarkan
  } else {
    // Radio tidak menyelesaikan TX. Jarang terjadi, tapi kalau terjadi kita
    // TIDAK menunggu reboot: chip langsung di-init ulang supaya bisa menerima
    // paket TX berikutnya (yang datang ±2 dtk lagi).
    ackTimeouts++;
    Serial.printf(" ⚠️  ACK #%d tidak selesai (TxDone tidak datang, %lu×) — radio di-init ulang\n",
                  data.seq, ackTimeouts);
    recoverLoRa("ACK tidak selesai");
  }
#else
  LoRa.receive();              // listen-only (uji pembanding)
#endif

  // ─── PAKET #0 = KALIBRASI (bukan data) ───
  // TX mengirim SEQ=0 paling awal tiap run. Paket ini di-ACK tapi TIDAK
  // dipublish ke MQTT dan TIDAK dihitung dalam rx_count/loss — dashboard &
  // spreadsheet tetap berisi #1..#50 rill saja.
  //
  // Kunci agar delay #1 rill: rawDiff #0 (rxTime − send_ms) disimpan sebagai
  // prevRawDiffMs & minimum. Saat #1 datang membawa RTT#0 (RTT/2 ≈ delay
  // satu arah #0), Langkah 1 menghitung offset ABSOLUT = rawDiff#0 − RTT#0/2
  // → delay #1 = rawDiff#1 − offset = delay satu arah sesungguhnya.
  // Kalau ACK #0 hilang (TX tak punya RTT), #1 memakai offset relatif
  // rawDiff#0 (Langkah 2) — tetap bernilai nyata, bukan 0.
  if (data.seq == 0) {
    long rawDiff0 = (data.send_ms > 0) ? ((long)rxTime - (long)data.send_ms) : -1;
    clockOffsetValid    = false;
    clockOffsetRefined  = false;
    prevRawDiffMs       = rawDiff0;                     // ← dipakai Langkah 1 di paket #1
    rawDiffMinValid     = (rawDiff0 != -1L);            // -1 = field MS= tidak ada
    rawDiffMin          = (rawDiff0 != -1L) ? rawDiff0 : 0;
    rttSeen             = false;
    rttMissingWarned    = false;
    firstPacketReceived = false;
    lastReceivedSEQ     = -1;
    sameSeqStreak       = 0;
    seqStuck            = false;
    clearLossState();   // #0 = TX baru boot → run baru, buang state lama
    Serial.println();
    Serial.printf("🧭 PAKET #0 (KALIBRASI) — di-ACK (RTT utk paket #1), rawDiff=%ld ms. Tidak dipublish.\n", rawDiff0);
    return;
  }

  // Hitung paket yang benar-benar diterima (dipakai dashboard sebagai kunci
  // anti-duplikat & untuk peringatan SEQ macet).
  rxCount++;

  Serial.println();
  Serial.printf("═══════════════════════════════════════════════════════════\n");
  Serial.printf("📨 PAKET SEQ=%d — %d bytes | heap %u B\n", data.seq, packetSize, (unsigned)ESP.getFreeHeap());
  Serial.printf("═══════════════════════════════════════════════════════════\n");
  Serial.printf(" RSSI: %d dBm | SNR: %.2f dB | Waktu: %s\n", rssi, snr, nowStamp().c_str());
  // Detektor desinkron: field SF= di dalam paket ditulis TX sendiri dari
  // variabel activeSF-nya. Kalau berbeda dengan SF radio RX, berarti keduanya
  // tidak sinkron — seharusnya tidak pernah terjadi selama handshake config
  // berjalan benar. Alarm ini pembuktian fisik, bukan sekadar tulisan status.
  if (data.sf > 0 && data.sf != activeSF) {
    Serial.printf(" ⚠️ DESINKRON: TX melapor SF=%d, RX=%d — periksa config!\n", data.sf, activeSF);
  }
  Serial.printf(" Soil: θ=%.4f m³/m³ (Sat: %.1f%%) | g=%.4f (Δg=%.4f)\n",
                data.theta, data.sat * 100.0, data.g, data.dg);
  Serial.printf(" Tilt: X=%.2f° Y=%.2f° Z=%.2f° (Δθ=%.2f°)\n",
                data.tx, data.ty, data.tz, data.dth);
  Serial.printf(" Status: %s\n", data.status);

  // ─── Deteksi awal pengujian baru ───
  // TX di-reset untuk setiap skenario (SF7 → SF9 → SF12), sedangkan RX terus
  // menyala. Saat TX reboot, epoch millis()-nya berubah total sehingga offset
  // kalibrasi lama jadi tidak valid. Karena itu kalibrasi & counter loss
  // di-reset setiap kali pengujian baru terdeteksi.
  // Dua cara deteksi:
  //   a) SEQ kembali ke 1  → normal (paket pertama run baru diterima)
  //   b) SEQ menurun       → run baru tapi paket 1 hilang, jadi run baru
  //                          dideteksi mulai dari SEQ berikutnya yang masuk
  bool newRunDetected = (firstPacketReceived && data.seq == 1)
                     || (firstPacketReceived && data.seq < lastReceivedSEQ);
  if (newRunDetected) {
    clockOffsetValid    = false;
    clockOffsetRefined  = false;
    prevRawDiffMs       = -1;
    rawDiffMinValid     = false;
    rawDiffMin          = 0;
    rttSeen             = false;
    rttMissingWarned    = false;
    firstPacketReceived = false;
    lastReceivedSEQ     = -1;
    sameSeqStreak       = 0;
    seqStuck            = false;
    // Epoch baru: hapus state loss lama di NVS supaya run sebelumnya tidak
    // tercampur dengan hitungan pengujian yang baru.
    clearLossState();
    Serial.println("🔄 Pengujian baru terdeteksi — reset kalibrasi delay & loss.");
  }

  // ─── Packet Loss Tracking ───
  // rxTime sudah ditangkap di atas (segera setelah paket diterima)
  if (!firstPacketReceived) {
    if (lossRestored && lastReceivedSEQ >= 0 &&
        data.seq > lastReceivedSEQ) {
      // RX baru saja reboot di tengah run yang sama: lanjutkan hitungan dari
      // NVS. Gap SEQ dari TX tetap dihitung, jadi paket yang hilang selama RX
      // mati TIDAK dibuang dari laporan packet loss.
      unsigned long gap = data.seq - lastReceivedSEQ - 1;
      totalExpected += (1 + gap);
      totalReceived += 1;
      lastSeqGap     = (int)gap;
      Serial.printf("💾 Lanjut pasca-reboot: SEQ %d → %d (+%lu harapan, gap %lu)\n",
                    lastReceivedSEQ, data.seq, 1 + gap, gap);
      lastReceivedSEQ = data.seq;
    } else {
      // Run benar-benar baru (SEQ=1) atau SEQ mundur → epoch baru.
      if (lossRestored) clearLossState();
      lastReceivedSEQ = data.seq;
      totalExpected = 1;
      totalReceived = 1;
      lastSeqGap    = 0;
    }
    firstPacketReceived = true;
    lossRestored        = false;
    sameSeqStreak       = 0;
  } else {
    if (data.seq <= lastReceivedSEQ) {
      // Duplikat atau out-of-order, skip
      Serial.println("⚠️  Duplikat/out-of-order SEQ, skip loss tracking.");
      // SEQ tidak maju → hitung berapa kali berturut-turut (deteksi SEQ macet)
      sameSeqStreak++;
    } else if (data.seq == lastReceivedSEQ + 1) {
      // Normal: berurutan, tidak ada loss
      totalExpected += 1;
      totalReceived += 1;
      lastReceivedSEQ = data.seq;
      lastSeqGap      = 0;
      sameSeqStreak = 0;
    } else {
      // Gap terdeteksi: ada paket hilang
      unsigned long gap = data.seq - lastReceivedSEQ - 1;
      totalExpected += (1 + gap);  // 1 untuk paket ini + gap
      totalReceived += 1;
      lastSeqGap = (int)gap;
      Serial.printf("⚠️  GAP: SEQ %d → %d (%lu paket hilang!)\n",
                    lastReceivedSEQ, data.seq, gap);
      lastReceivedSEQ = data.seq;
      sameSeqStreak = 0;
    }
  }

  // Simpan berkala (throttle 30 dtk). Saat ada gap, paksa simpan supaya
  // informasi paket hilang tidak ikut hilang bila RX reboot sebelum 30 dtk.
  saveLossState(lastSeqGap > 0);

  // SEQ macet = nomor SEQ dari TX tidak pernah maju. Selama itu, packet loss
  // tidak bisa dihitung dari selisih SEQ (angka loss yang tampil hanya 0 %),
  // jadi paket ditandai supaya dashboard tidak menganggapnya data valid 100 %.
  seqStuck = (sameSeqStreak >= SEQ_STUCK_STREAK);
  if (seqStuck) {
    Serial.printf("⚠️  SEQ TX MACET (SEQ=%d berulang %d×) — packet loss tidak valid. "
                  "Upload firmware/ladet-tx/ladet-tx.ino versi terbaru.\n",
                  data.seq, sameSeqStreak);
  }

  // ══════════════════════════════════════════════════════════════════════
  // HITUNG DELAY  —  Delay = t_terima(RX) − t_kirim(TX)
  // ══════════════════════════════════════════════════════════════════════
  // millis() TX dan RX punya titik nol BERBEDA, jadi selisih mentah
  // (rxTime − send_ms) = delay_asli + offset_boot. offset_boot inilah yang
  // dulu terbaca sebagai "delay" dan naik ~1000 ms tiap detik (→ 181 s).
  //
  // Prioritas sumber delay:
  //   1. "clock"     offset dari RTT TX  → delay ABSOLUT (paling akurat)
  //   2. "clock-rel" offset = min(rawDiff) → delay RELATIF, jaring pengaman
  //                  (dipakai kalau TX belum mengirim field RTT=)
  //   3. "rtt"       RTT/2 langsung (kalau field MS= tidak ada sama sekali)
  //   4. "invalid"   di luar rentang wajar → dibuang oleh dashboard
  //
  // Semua jalur di atas DIJAMIN tidak pernah menghasilkan angka yang naik
  // terus mengikuti uptime RX.
  // ══════════════════════════════════════════════════════════════════════
  unsigned long delayMs = 0;
  delaySource = "unknown";

  // PENTING: RTT di paket ini adalah RTT paket SEBELUMNYA. TX baru tahu RTT
  // setelah ACK diterima, sedangkan paket ini sudah dikirim lebih dulu.
  // Batas atas memakai delaySanityMaxMs() yang SF-aware: di SF12 RTT ±8 dtk,
  // jadi batas tetap 5 dtk membuat RTT selalu dianggap tidak masuk akal.
  bool rttPlausible = (data.rtt_ms > 0.0 && data.rtt_ms < (float)delaySanityMaxMs());
  if (rttPlausible) rttSeen = true;

  // rawDiff = millis() RX − millis() TX pada paket yang sama.
  // Nilainya BOLEH NEGATIF dan itu normal: titik nol millis() kedua board
  // berbeda (waktu boot masing-masing), jadi tandanya hanya menunjukkan board
  // mana yang lebih dulu menyala. Versi lama mensyaratkan rawDiff >= 0, jadi
  // setiap kali RX menyala lebih dulu (TX baru di-upload untuk ganti SF),
  // kalibrasi TIDAK PERNAH terjadi dan SELURUH kolom delay berisi 0 ms —
  // persis yang terjadi pada skenario SF12. Yang penting bukan tandanya,
  // melainkan selisih antar paket. -1 = field MS= tidak ada di paket.
  long rawDiffMs = (data.send_ms > 0) ? ((long)rxTime - (long)data.send_ms) : -1;
  bool haveRawDiff = (rawDiffMs != -1L);

  // Running minimum rawDiff → jaring pengaman relatif (lihat langkah 2)
  if (haveRawDiff && (!rawDiffMinValid || rawDiffMs < rawDiffMin)) {
    rawDiffMin      = rawDiffMs;
    rawDiffMinValid = true;
  }

  // ── Langkah 1: offset ABSOLUT dari RTT paket sebelumnya ──
  // RTT/2 = delay satu arah paket tersebut (diukur jam TX sendiri, bebas
  // offset). Jadi offset_boot = rawDiff_paket_lalu − RTT/2. Setelah offset
  // diketahui, delay tiap paket = rawDiff − offset, persis t_terima − t_kirim.
  if (rttPlausible && prevRawDiffMs != -1L && !clockOffsetRefined) {
    long refined = prevRawDiffMs - (long)(data.rtt_ms / 2.0);
    Serial.printf(" 🔧 Offset dari RTT TX (ABSOLUT): %ld ms  → delay ABSOLUT aktif ✅\n", refined);
    clockOffsetMs      = refined;
    clockOffsetValid   = true;
    clockOffsetRefined = true;
  }

  // ── Langkah 2: jaring pengaman RELATIF ──
  // Kalau TX belum mengirim RTT=, offset = rawDiff terkecil yang pernah
  // terlihat. Hasilnya delay relatif terhadap paket tercepat: selalu >= 0,
  // dan TIDAK PERNAH membengkak mengikuti uptime RX.
  //
  // CATATAN: blok ini hampir selalu jalan di paket 1, karena pada paket 1 TX
  // memang belum mungkin mengirim RTT (RTT paket 1 baru diketahui setelah
  // ACK-nya masuk, yaitu saat paket 2 dikirim). Jadi di sini JANGAN menyimpulkan
  // "TX tidak mendukung RTT" — itu baru bisa dipastikan di paket ke-3
  // (lihat diagnostik otomatis di bagian tampilan).
  if (!clockOffsetValid && rawDiffMinValid) {
    clockOffsetMs    = rawDiffMin;
    clockOffsetValid = true;
    if (data.seq <= 2) {
      Serial.printf(" 🔧 Kalibrasi awal offset: %ld ms (delay relatif dulu, "
                    "menunggu RTT dari TX di paket berikutnya)\n", clockOffsetMs);
    } else {
      Serial.printf(" 🔧 Offset kalibrasi RELATIF (TX belum kirim RTT=): %ld ms\n", clockOffsetMs);
    }
  }

  if (haveRawDiff && clockOffsetValid) {
    long corrected = rawDiffMs - clockOffsetMs;
    if (corrected < 0) corrected = 0;
    delayMs     = (unsigned long)corrected;
    delaySource = clockOffsetRefined ? "clock" : "clock-rel";
  } else if (rttPlausible) {
    // Tidak ada field MS= (firmware TX sangat lama) → pakai RTT/2 apa adanya
    delayMs     = (unsigned long)(data.rtt_ms / 2.0);
    delaySource = "rtt";
  }

  // ── Sanity check ──
  // Ambang mengikuti SF (lihat delaySanityMaxMs()). Delay 25 dtk di SF12 tetap
  // mustahil, jadi angka yang membengkak mengikuti uptime RX tetap tertangkap.
  unsigned long sanityMax = delaySanityMaxMs();
  if (delayMs > sanityMax) {
    unsigned long rawDelayMs = delayMs;
    bool pakaiRtt = rttPlausible;
    if (pakaiRtt) {
      delayMs     = (unsigned long)(data.rtt_ms / 2.0);
      delaySource = "rtt";
    } else {
      delayMs     = 0;
      delaySource = "invalid";
    }
    Serial.printf(" ⚠️  delay %lu ms > sanity %lu ms (SF%d) → %s\n",
                  rawDelayMs, sanityMax, activeSF,
                  pakaiRtt ? "pakai RTT/2" : "ditandai invalid");
  }

  // Simpan rawDiff paket ini (dipakai langkah 1 di paket berikutnya)
  prevRawDiffMs = rawDiffMs;

  // Hitung packet loss %
  float packetLoss = 0.0;
  if (totalExpected > 1) {
    packetLoss = ((float)(totalExpected - totalReceived) / totalExpected) * 100.0;
  }

  // ── Tampilkan delay, loss & ACK ──
  Serial.printf(" ⏱️ Delay: %lu ms (sumber: %s)", delayMs, delaySource);
  if (data.rtt_ms > 0) {
    // RTT ini milik paket sebelumnya — dicetak sebagai cross-check saja
    Serial.printf(" | RTT paket sebelumnya: %.1f ms (RTT/2 = %.1f ms)",
                  data.rtt_ms, data.rtt_ms / 2.0);
    if (rttPlausible && delaySource == "clock") {
      Serial.printf(" | selisih vs clock-diff: %.1f ms",
                    (double)delayMs - (data.rtt_ms / 2.0));
    }
  }
#if RX_SEND_ACK
  Serial.printf(" 📤 ACK %s: \"%s\"\n", ackOk ? "terkirim" : "GAGAL", ack.c_str());
#else
  Serial.println(" 📤 ACK dimatikan (RX_SEND_ACK=0) — listen-only");
#endif
  Serial.println();

  // ── Diagnostik sekali jalan: apakah TX perlu di-upload ulang? ──
  // Kalau sampai paket ke-3 TX masih belum mengirim RTT=, berarti firmware TX
  // belum mendukungnya → delay yang terukur hanya RELATIF (bukan absolut).
  //
  // PENTING: RTT yang ADA tapi ditolak batas kewajaran bukan alasan yang sama.
  // Di SF12 RTT memang ±8 dtk, dan sebelum ambang dibuat SF-aware angka itu
  // selalu ditolak — pesan "TX belum kirim RTT" jadi menyesatkan. Sekarang dua
  // keadaan itu dipisahkan supaya penyebabnya jelas dari Serial saja.
  bool rttAdaTapiDitolak = (data.rtt_ms > 0.0 && !rttPlausible);
  static bool rttRejectedWarned = false;
  if (rttAdaTapiDitolak && !rttRejectedWarned) {
    rttRejectedWarned = true;
    Serial.printf(" ⚠️  RTT=%.1f ms ADA tapi ditolak (batas %lu ms, SF%d). "
                  "Delay jatuh ke relatif/invalid.\n",
                  data.rtt_ms, sanityMax, activeSF);
  }
  if (!rttSeen && !rttAdaTapiDitolak && !rttMissingWarned && data.seq >= 3) {
    rttMissingWarned = true;
    Serial.println(" ╔═══════════════════════════════════════════════════════╗");
    Serial.println(" ║ ⚠️  TX BELUM MENGIRIM FIELD RTT=                       ║");
    Serial.println(" ║   → Delay sekarang RELATIF (0 = paket tercepat).       ║");
    Serial.println(" ║   → Grafik tetap STABIL, tidak akan membengkak 181 s.  ║");
    Serial.println(" ║   → Untuk delay ABSOLUT: upload ladet-tx.ino ke TX.    ║");
    Serial.println(" ╚═══════════════════════════════════════════════════════╝");
  }

  Serial.printf(" 📦 Loss: %.1f%% (%lu/%lu) | 📤 MQTT: ", packetLoss, totalReceived, totalExpected);

  // ── AKTUATOR & NOTIFIKASI LOKAL ──
  // Dijalankan SEBELUM publishToMQTT dan tanpa syarat koneksi jaringan:
  // inilah inti "tetap berguna saat internet mati". Keputusan level tetap
  // milik TX (field STATUS=), RX hanya mengeksekusi relay.
  {
    int level = statusToLevel(data.status);
    applyActuators(level);
    alertIfNeeded(data, level, rssi);
    lcdLevel = level;
    lcdSat   = data.sat * 100.0;
    lcdDg    = data.dg;
    lcdTilt  = data.dth;
    lcdRssi  = rssi;
  }

  // ── KIRIM DATA KE MQTT ──
  // INI yang membuat dashboard menerima `ladet/data`. Tanpa baris ini RX
  // tetap menerima LoRa & mencetak Serial, tetapi broker tidak pernah
  // menerima apa pun → dashboard terus "MENUNGGU RX".
  publishToMQTT(data, rssi, snr, rxTime, delayMs, packetLoss);
  Serial.printf("═══════════════════════════════════════════════════════════\n");
}

SensorData parsePacket(String raw) {
  SensorData data;
  strcpy(data.type, "DATA");
  data.seq = 0;
  data.soil_raw = 0;
  data.soil_mv = 0;
  data.theta = 0.0;
  data.theta_r = 0.0;
  data.theta_s = 0.0;
  data.sat = 0.0;
  data.g0 = 0.0;
  data.g = 0.0;
  data.dg = 0.0;
  data.tx = 0.0;
  data.ty = 0.0;
  data.tz = 0.0;
  data.dtx = 0.0;
  data.dty = 0.0;
  data.dtz = 0.0;
  data.dth = 0.0;
  data.send_ms = 0;
  data.rtt_ms = -1.0;  // default: tidak ada RTT
  strcpy(data.status, "AMAN");
  data.sf = 9;
  data.theta0 = 0.0;
  data.thetam = 0.0;
  data.has_thetam = false;
  
  int startIdx = 0;
  int commaIdx = 0;
  
  while ((commaIdx = raw.indexOf(',', startIdx)) != -1) {
    String token = raw.substring(startIdx, commaIdx);
    parseKeyValue(token, data);
    startIdx = commaIdx + 1;
  }
  String lastToken = raw.substring(startIdx);
  parseKeyValue(lastToken, data);
  
  return data;
}

void parseKeyValue(String token, SensorData &data) {
  token.trim();
  if (token.length() == 0) return;
  
  int eqIdx = token.indexOf('=');
  if (eqIdx == -1) return;
  
  String key = token.substring(0, eqIdx);
  String value = token.substring(eqIdx + 1);
  key.trim();
  value.trim();
  
  if (key == "TYPE") value.toCharArray(data.type, sizeof(data.type));
  else if (key == "SEQ") data.seq = value.toInt();
  // Field TIME= dari firmware TX lama diabaikan: waktu absolut memakai NTP RX.
  else if (key == "SOIL_RAW") data.soil_raw = value.toInt();
  else if (key == "SOIL_MV") data.soil_mv = value.toInt();
  else if (key == "THETA") data.theta = value.toFloat();
  else if (key == "THETAR") data.theta_r = value.toFloat();
  else if (key == "THETAS") data.theta_s = value.toFloat();
  else if (key == "SAT") data.sat = value.toFloat();
  else if (key == "G0") data.g0 = value.toFloat();
  else if (key == "G") data.g = value.toFloat();
  else if (key == "DG") data.dg = value.toFloat();
  else if (key == "TX") data.tx = value.toFloat();
  else if (key == "TY") data.ty = value.toFloat();
  else if (key == "TZ") data.tz = value.toFloat();
  else if (key == "DTX") data.dtx = value.toFloat();
  else if (key == "DTY") data.dty = value.toFloat();
  else if (key == "DTZ") data.dtz = value.toFloat();
  else if (key == "DTH") data.dth = value.toFloat();
  else if (key == "THETA0") data.theta0 = value.toFloat();
  else if (key == "THETAM") { data.thetam = value.toFloat(); data.has_thetam = true; }
  else if (key == "MS") data.send_ms = strtoul(value.c_str(), NULL, 10);
  else if (key == "RTT") data.rtt_ms = value.toFloat();
  else if (key == "STATUS") value.toCharArray(data.status, sizeof(data.status));
  else if (key == "SF") data.sf = value.toInt();

  // Paket slim (hemat byte): THETAM tidak dikirim karena IDENTIK dengan TZ.
  // Sudut terukur = TZ — perintah ini jalan SETELAH parsing TZ, jadi hanya
  // dipakai kalau THETAM= memang tidak ada di paket.
  if (key == "DTH" && !data.has_thetam) data.thetam = data.tz;
}

void publishToMQTT(SensorData data, int rssi, float snr, unsigned long rxTime, unsigned long delayMs, float packetLoss) {
  // JANGAN return saat MQTT offline. Payload tetap dibangun (cap waktu & rx_ms
  // diambil pada saat paket diterima), lalu masuk antrean RAM di bagian bawah
  // fungsi ini dan dikirim ulang setelah koneksi pulih.
  
  // 1024 → 1536: payload bertambah rx_count, rx_ms, seq_stuck. Kalau dokumen
  // JSON kurang dari yang dibutuhkan, ArduinoJson memotong hasilnya diam-diam
  // dan dashboard gagal JSON.parse (data tidak muncul sama sekali).
  StaticJsonDocument<1536> doc;

  doc["type"] = data.type;
  doc["packet_id"] = data.seq;
  // Cap waktu absolut dari NTP (pengganti RTC). Kalau internet mati, isinya
  // "uptime N s" — bukan jam palsu 1 Jan 2000.
  doc["timestamp"] = nowStamp();
  
  JsonObject sensor = doc.createNestedObject("sensor");
  sensor["soil_saturation"] = data.sat * 100.0;
  sensor["theta_measured"] = data.theta;            // kelembapan terukur (m3/m3) — utk CSV skripsi
  sensor["theta_r"] = data.theta_r;                 // titik kering θr (m3/m3) saat paket ini
  sensor["theta_s"] = data.theta_s;                 // titik jenuh θs (m3/m3) saat paket ini
  sensor["g_initial"] = data.g0;                    // getaran acuan (baseline)
  sensor["g"] = data.g;                             // getaran terukur
  sensor["delta_g"] = data.dg;                      // delta getaran (acuan status)
  sensor["tilt_angle"] = data.dth;                  // delta sudut (acuan status — yang dipakai dashboard)
  sensor["tilt_angle_measured"] = data.thetam;      // sudut terukur (BNO Z)
  sensor["tilt_angle_reference"] = data.theta0;     // sudut acuan (baseline)
  sensor["tilt_x"] = data.tx;
  sensor["tilt_y"] = data.ty;
  sensor["tilt_z"] = data.tz;
  sensor["delta_tilt_x"] = data.dtx;
  sensor["delta_tilt_y"] = data.dty;
  sensor["delta_tilt_z"] = data.dtz;

  String statusLower = String(data.status);
  statusLower.toLowerCase();
  doc["status"] = statusLower;
  
  JsonObject radio = doc.createNestedObject("radio");
  radio["sf"] = "SF" + String(data.sf);
  radio["rssi"] = rssi;
  radio["snr"] = snr;
  
  JsonObject test = doc.createNestedObject("test");
  test["distance_m"] = activeDistance;
  test["delay_ms"] = delayMs;
  test["delay_source"] = delaySource;
  test["packet_loss"] = packetLoss;
  test["total_expected"] = totalExpected;
  test["total_received"] = totalReceived;
  // Nomor terima RX (monoton naik) → kunci anti-duplikat di dashboard, tetap
  // valid walau SEQ dari TX macet/diulang.
  test["rx_count"] = rxCount;
  test["rx_ms"] = millis();
  // RTT paket sebelumnya (diukur jam TX sendiri via micros() — 1 jam, akurat,
  // tidak bergantung RTC yang belum diset) → dashboard/CSV bisa menampilkan
  // delay versi RTT. -1 = TX belum mengirim field RTT= (paket awal).
  if (data.rtt_ms >= 0.0) test["rtt_ms"] = data.rtt_ms;
  // Waktu kirim versi TX = (waktu RX terima, jam RX) − delay terukur.
  // BUKAN RTC mentah (rtc.now() di TX belum diset → 1/1/2000). Format ISO
  // supaya Excel tidak salah tebak (tidak jadi "1-Jan" seperti "terima/kirim").
  {
    unsigned long txMs = (rxTime >= delayMs) ? (rxTime - delayMs) : 0;
    unsigned long s = txMs / 1000UL;
    char txBuf[32];
    snprintf(txBuf, sizeof(txBuf), "%02lu:%02lu:%02lu.%03lu",
             (s / 3600UL) % 24, (s / 60UL) % 60, s % 60, txMs % 1000UL);
    sensor["tx_time"] = txBuf;
  }
  // true = SEQ TX tidak maju → packet loss dari selisih SEQ tidak bisa dipakai.
  test["seq_stuck"] = seqStuck;
  // ── Diagnostik gateway (untuk CSV skripsi) ──
  // rx_boot      : nomor boot RX. Kalau berubah di tengah run → RX reboot.
  // rx_uptime_s  : umur RX saat paket ini. Kecil = baru nyala/reboot.
  // seq_gap      : jumlah paket TX yang hilang tepat sebelum paket ini
  //                (0 = berurutan). Bikin loss kelihatan baris per baris.
  test["rx_boot"] = bootCount;
  test["rx_uptime_s"] = millis() / 1000UL;
  test["seq_gap"] = lastSeqGap;
  // Penyebab reboot RX terakhir (BROWNOUT / TASK_WDT / PANIC / POWERON) dan
  // berapa kali ACK gagal selesai. Dipakai CSV Bab 4 untuk memisahkan "paket
  // hilang karena radio" dari "paket hilang karena gateway restart".
  test["rx_reset"] = resetReasonName();
  test["rx_ack_timeouts"] = ackTimeouts;
  
  char jsonBuffer[1536];
  size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

  // Deteksi pemotongan JSON (buffer kurang) supaya tidak dikirim payload rusak.
  if (len >= sizeof(jsonBuffer)) {
    Serial.println("❌ JSON terpotong (buffer kurang) — payload TIDAK dikirim.");
    return;
  }

  boolean success = mqttClient.connected() && mqttClient.publish(MQTT_TOPIC, jsonBuffer);
  
  if (success) {
    Serial.printf("✅ MQTT sent (%d bytes)\n", len);
  } else {
    // Bukan dibuang: simpan ke antrean RAM, nanti dikirim ulang otomatis saat
    // WiFi/MQTT pulih (lihat flushPending()). Ini yang membuat pengujian 50
    // paket tetap lengkap walau jaringan sempat putus.
    enqueuePending(jsonBuffer);
    Serial.printf("💾 MQTT %s — paket disimpan, antrean %u (payload %d B, buffer 2048, state=%d)\n",
                  mqttClient.connected() ? "publish gagal" : "offline",
                  (unsigned)pendingCount, len, mqttClient.state());
  }
}
