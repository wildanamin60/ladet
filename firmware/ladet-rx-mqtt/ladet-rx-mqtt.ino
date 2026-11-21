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
#include <WiFiClientSecure.h>   // Telegram (HTTPS)
#include <time.h>               // NTP — pengganti RTC
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LoRa.h>
#include <Preferences.h>   // simpan SF aktif di flash (NVS) — tahan reset
#include <esp_system.h>    // esp_reset_reason() — penyebab reboot terakhir

// ══════════════════════════════════════════════════════════════════════════════
// KONFIGURASI — SESUAIKAN DENGAN SETUP ANDA
// ══════════════════════════════════════════════════════════════════════════════

// WiFi
// ⚠️ WAJIB DIISI sebelum upload — kalau masih "YOUR_WIFI_SSID" RX tidak akan
//    pernah connect ke MQTT (Serial Monitor akan mencetak "....." terus-menerus).
//    Perhatikan: harus WiFi 2.4 GHz (ESP32 tidak support 5 GHz).
const char* WIFI_SSID = "Alieeen3";  // Ganti dengan SSID WiFi
const char* WIFI_PASS = "bukanwildan";  // Ganti dengan password WiFi

// MQTT Broker
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;  // Native MQTT (bukan 8884 WebSocket)
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

// Test Configuration — UBAH SESUAI SKENARIO PENGUJIAN
const int TEST_DISTANCE_M = 100;  // 100m, 300m, 700m, atau 1000m (sesuai revisi sempro)
const int TOTAL_PACKETS   = 50;   // Harus sama dengan TX

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

// Modul relay umum (optocoupler) aktif LOW: IN=LOW → kontak menutup.
// Kalau relay Anda justru aktif HIGH, ubah angka ini menjadi 0.
//
// ⚠️ GEJALA: SEMUA relay nyala begitu board diberi daya (tanpa TX, tanpa data)
//    Berarti pin OFF sedang dibaca modul sebagai ON. Dua penyebab:
//    a) modul AKTIF HIGH  → ubah angka ini menjadi 0
//    b) modul 5 V aktif LOW tapi sinyal 3.3 V tidak cukup mematikannya
//       → VCC logika modul ke 3.3 V, JD-VCC ke 5 V (lepas jumper), GND tetap satu
//    Cara memastikan: upload `firmware/tools/relay-test/relay-test.ino`.
#define RELAY_ACTIVE_LOW 1

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
int pendingDistance = -1;
unsigned long lastLoRaRxMs = 0;        // millis() paket LoRa TERAKHIR (apa pun: data/BEACON) — bukti TX hidup
unsigned long lastDataRxMs = 0;         // millis() paket DATA terakhir — PENENTU run sedang berjalan atau tidak
const unsigned long CONFIG_QUIET_MS   = 15000UL; // run selesai = sunyi 15 dtk
const unsigned long CONF_WAIT_MS      = 1500UL;  // timeout tunggu ACK=CONF
const int           CONF_MAX_ATTEMPTS = 10;
const char* CONFIG_STATUS_TOPIC = "ladet/config/status";
const char* CMD_TOPIC = "ladet/config/cmd";   // perintah web (non-retained): "RUN"
String pendingCommand = "";                  // ENGINEER, RUN, atau MONITOR

// ── Status kehadiran perangkat (supaya dashboard TIDAK menebak) ──
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

/** Tulis satu kanal relay sesuai polaritas modul (aktif LOW pada umumnya). */
void setRelay(int pin, bool on) {
#if RELAY_ACTIVE_LOW
  digitalWrite(pin, on ? LOW : HIGH);
#else
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

void setupActuators() {
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  // Semua OFF sejak awal supaya relay tidak "klik" saat boot.
  setRelay(PIN_LED_YELLOW, false);
  setRelay(PIN_LED_GREEN, false);
  setRelay(PIN_BUZZER, false);
  if (!ACTUATOR_RED_CONFLICT) {
    pinMode(PIN_LED_RED, OUTPUT);
    setRelay(PIN_LED_RED, false);
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
  setRelay(PIN_LED_GREEN,  level == 0);
  setRelay(PIN_LED_YELLOW, level == 1 || level == 2);
  setRelay(PIN_BUZZER,     level == 2);
  // Kalau pin merah bentrok dengan MOSI LoRa, JANGAN ditulis: menulis pin itu
  // akan merusak jalur SPI. Relay merah (dan buzzer BAHAYA yang satu kanal)
  // tidak aktif sampai wiring diperbaiki — lihat WIRING.md.
  if (!ACTUATOR_RED_CONFLICT) {
    setRelay(PIN_LED_RED, level == 3);
  } else if (level == 3) {
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

/** Kirim satu pesan ke Telegram. Blocking maks ~5 dtk; hanya saat level berubah. */
bool sendTelegram(const String& text) {
  if (!telegramConfigured()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();          // sertifikat tidak diverifikasi — cukup untuk skripsi
  client.setTimeout(5);          // detik
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("📵 Telegram: gagal konek api.telegram.org");
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
  Serial.printf("📨 Telegram: %s\n", ok ? "terkirim" : statusLine.c_str());
  return ok;
}

/** Simpan pesan; dikirim sekarang kalau ada internet, atau nanti saat pulih. */
void queueAlert(const String& text) {
  pendingAlert = text;
  pendingAlertAt = millis();
  Serial.println("📮 Notifikasi masuk antrian (dikirim saat internet tersedia)");
}

/** Dipanggil tiap loop: coba kirim antrian, jangan spam percobaan. */
void serviceTelegram() {
  if (pendingAlert.length() == 0) return;
  if (!telegramConfigured()) { pendingAlert = ""; return; }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - pendingAlertAt > 30UL * 60UL * 1000UL) {
    pendingAlert = "";
    Serial.println("🗑️ Notifikasi kedaluwarsa (>30 mnt) — dibuang");
    return;
  }
  if (millis() - lastTelegramAttemptMs < 15000UL) return;   // coba tiap 15 dtk
  lastTelegramAttemptMs = millis();
  if (sendTelegram(pendingAlert)) pendingAlert = "";
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


void publishConfigStatus(const char* state, bool txConfirmed) {
  if (!mqttClient.connected()) return;
  String json = "{\"sf\":" + String(activeSF) +
                ",\"distance\":" + String(activeDistance) +
                ",\"tx_confirmed\":" + String(txConfirmed ? "true" : "false") +
                ",\"state\":\"" + state + "\"}";
  mqttClient.publish(CONFIG_STATUS_TOPIC, json.c_str());
  Serial.println("📤 Config status: " + json);
}

bool channelQuiet() {
  // PENTING: hanya paket DATA yang membuat kanal "sibuk". BEACON TX (tiap 12
  // detik) HANYA bukti TX hidup — kalau ikut dihitung, kanal tak pernah kosong
  // dan perintah RUN akan SELALU ditolak ("run_busy selamanya").
  return (millis() - lastDataRxMs) > CONFIG_QUIET_MS;
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("📩 MQTT config: %s = %s\n", topic, msg.c_str());

  String t = String(topic);
  if (t == "ladet/config/sf") {
    int sf = msg.toInt();                 // terima "SF9" maupun "9"
    if (sf == 0 && msg.startsWith("SF")) sf = msg.substring(2).toInt();
    if (sf != 7 && sf != 9 && sf != 12) {
      publishConfigStatus("rejected", false);
      return;
    }
    if (sf == activeSF) {                 // sudah sama — tidak ada yg dikirim ke TX
      publishConfigStatus("applied", false);
      return;
    }
    pendingSF = sf;
    publishConfigStatus("queued", false);
  } else if (t == "ladet/config/distance") {
    int d = msg.toInt();
    if (d <= 0) return;
    pendingDistance = d;
    publishConfigStatus("queued", false);
  } else if (t == CMD_TOPIC) {
    // Perintah mulai run dari web (non-retained, jadi tidak akan terpantik
    // ulang saat RX reconnect).
    if (msg == "ENGINEER" || msg == "RUN" || msg == "MONITOR") {
      pendingCommand = msg;
      publishConfigStatus("run_queued", false);
    }
  }
}

// Kirim perintah mulai run ke TX (TYPE=RUN → ACK=RUN) sampai di-ACK.
// Tipe dibedakan dari TYPE=CONF supaya TX firmware lama (yang hanya kenal
// TYPE=CONF) tidak menjawab → tidak ada lagi "run_started" palsu.
// maxAttempts = jumlah percobaan per SF. true = TX konfirmasi.
bool sendDeviceCommand(const String& command, int maxAttempts) {
  Serial.printf("▶️ CONFIG: kirim perintah %s ke TX...\n", command.c_str());
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    LoRa.idle();
    LoRa.beginPacket();
    LoRa.print("TYPE=" + command + ",SF=" + String(activeSF));
    LoRa.endPacket();
    LoRa.receive();

    unsigned long t0 = millis();
    while (millis() - t0 < CONF_WAIT_MS) {
      int size = LoRa.parsePacket();
      if (size > 0) {
        String in;
        while (LoRa.available()) in += (char)LoRa.read();
        if (in.startsWith("ACK=" + command)) {
          Serial.printf("   ✅ TX konfirmasi perintah %s\n", command.c_str());
          lastLoRaRxMs = millis();   // TX terbukti hidup & mendengarkan
          return true;
        }
      }
      mqttClient.loop();
      delay(5);
    }
    Serial.printf("   ⏳ percobaan %d/%d tanpa ACK...\n", attempt, maxAttempts);
  }
  return false;
}

// Kirim TYPE=CONF ke TX (MASIH di SF lama) sampai di-ACK, maks 10 percobaan.
// true = TX konfirmasi → SF baru aman diterapkan RX.
bool handshakeNewSF(int newSF) {
  Serial.printf("⚙️ CONFIG: handshake SF -> %d (kirim TYPE=CONF di SF %d)...\n", newSF, activeSF);
  for (int attempt = 1; attempt <= CONF_MAX_ATTEMPTS; attempt++) {
    // Tiba-tiba ada paket data (run baru mulai)? Batalkan — jangan ganggu run.
    if (!channelQuiet()) {
      Serial.println("   ✋ Paket data terdeteksi — handshake dibatalkan.");
      return false;
    }

    LoRa.idle();
    LoRa.beginPacket();
    LoRa.print("TYPE=CONF,SF=" + String(newSF) + ",DIST=" + String(activeDistance));
    LoRa.endPacket();
    LoRa.receive();

    unsigned long t0 = millis();
    while (millis() - t0 < CONF_WAIT_MS) {
      int size = LoRa.parsePacket();
      if (size > 0) {
        String in;
        while (LoRa.available()) in += (char)LoRa.read();
        if (in.startsWith("ACK=CONF")) {
          LoRa.setSpreadingFactor(newSF);
          Serial.printf("   ✅ TX konfirmasi → SF RX aktif: %d\n", newSF);
          return true;
        }
      }
      mqttClient.loop();   // jaga koneksi MQTT selama menunggu
      delay(5);
    }
    Serial.printf("   ⏳ percobaan %d/%d tanpa ACK...\n", attempt, CONF_MAX_ATTEMPTS);
  }
  return false;
}

// Dipanggil dari loop(): realisasikan config yang diantri kalau kanal kosong.
void processConfigQueue() {
  // ── Perintah mulai run (dari web) ──
  if (pendingCommand.length() > 0) {
    if (!mqttClient.connected()) return;
    String command = pendingCommand;
    // ENGINEER/MONITOR adalah perintah mode dan harus bisa memotong loop
    // monitoring yang sedang berjalan. RUN tetap menunggu kanal sunyi.
    if (command == "RUN" && !channelQuiet()) {
      pendingCommand = "";
      publishConfigStatus("run_busy", false);
      return;
    }
    pendingCommand = "";
    bool ok = sendDeviceCommand(command, CONF_MAX_ATTEMPTS);
    bool resynced = false;

    // ── AUTO-RESYNC ──
    // Kalau TX tidak menjawab di SF kita, TX mungkin berada di SF lain
    // (mis. NVS TX berbeda setelah re-flash). Coba SF 7/9/12 satu per satu;
    // begitu TX menjawab, RX PINDAH ke SF itu + simpan ke NVS → tersambung lagi.
    if (!ok) {
      const int candidates[3] = {7, 9, 12};
      for (int i = 0; i < 3 && !ok; i++) {
        if (candidates[i] == activeSF) continue;
        Serial.printf("🔍 AUTO-RESYNC: TX belum menjawab di SF %d — coba SF %d...\n",
                      activeSF, candidates[i]);
        int prevSF = activeSF;
        activeSF = candidates[i];
        LoRa.setSpreadingFactor(activeSF);
        ok = sendDeviceCommand(command, 3);
        if (ok) {
          resynced = true;
          prefs.begin("ladet", false);
          prefs.putUChar("sf", (unsigned char)activeSF);
          prefs.end();
          Serial.printf("   ✅ Tersambung kembali di SF %d (disimpan ke NVS)\n", activeSF);
        } else {
          activeSF = prevSF;
          LoRa.setSpreadingFactor(activeSF);
        }
      }
    }

    publishConfigStatus(ok ? (resynced ? "run_resynced" : "run_started") : "run_unconfirmed", ok);
    return;
  }

  if (pendingSF < 0 && pendingDistance < 0) return;
  if (!channelQuiet() || !mqttClient.connected()) return;

  if (pendingDistance >= 0) {
    activeDistance = pendingDistance;
    pendingDistance = -1;
    Serial.printf("⚙️ CONFIG: jarak aktif -> %d m (metadata lokal RX)\n", activeDistance);
  }

  if (pendingSF >= 0) {
    if (handshakeNewSF(pendingSF)) {
      activeSF = pendingSF;
      pendingSF = -1;
      // Simpan ke NVS: RX yang di-reset tetap di SF ini (sinkron dgn TX)
      prefs.begin("ladet", false);
      prefs.putUChar("sf", (unsigned char)activeSF);
      prefs.end();
      publishConfigStatus("applied", true);    // TX benar-benar konfirmasi via LoRa
    } else {
      pendingSF = -1;
      publishConfigStatus("tx_unconfirmed", false);  // SF TIDAK diubah; jarak (jika ada) tetap tercatat
    }
    return;
  }

  // Hanya jarak yang berubah: TX tidak terlibat → JANGAN klaim tx_confirmed.
  publishConfigStatus("applied", false);
}

// ══════════════════════════════════════════════════════════════════════════════
// PACKET LOSS & DELAY TRACKING
// ══════════════════════════════════════════════════════════════════════════════
int lastReceivedSEQ = -1;
unsigned long totalExpected = 0;
unsigned long totalReceived = 0;
bool firstPacketReceived = false;

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

const unsigned long DELAY_SANITY_MAX_MS = 5000UL;   // > 5 s = tidak valid
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

  Serial.println();
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println("  LADET RX — Gateway MQTT Publisher");
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println();
  Serial.printf("Reset terakhir : %s\n", resetReasonName());
  Serial.printf("Boot ke-       : %u\n", bootCount);
  Serial.println();
  Serial.println("TEST CONFIGURATION:");
  Serial.printf("  Distance : %d m\n", TEST_DISTANCE_M);
  Serial.printf("  SF       : %d (HARUS sama dengan TX!)\n", LORA_SF);
  Serial.println("═══════════════════════════════════════════════════════════");
  Serial.println();

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
  Serial.println();

  setupWiFi();
  setupClock();          // NTP — pengganti RTC untuk cap waktu
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(1024);
  mqttClient.setCallback(onMqttMessage);
  setupLoRa();

  // Pulihkan SF terakhir dari NVS: supaya setelah reset RX tetap sinkron
  // dengan TX yang sudah dikonfigurasi lewat web.
  prefs.begin("ladet", true);
  int savedSF = prefs.getUChar("sf", 0);
  prefs.end();
  if ((savedSF == 7 || savedSF == 9 || savedSF == 12) && savedSF != activeSF) {
    activeSF = savedSF;
    LoRa.setSpreadingFactor(activeSF);
    Serial.printf("🔄 SF dipulihkan dari NVS: %d\n", activeSF);
  }
  
  Serial.println("✅ RX Gateway siap!");
  Serial.printf("📡 MQTT Topic: %s\n", MQTT_TOPIC);
  Serial.printf("📍 Test Distance: %d m\n", TEST_DISTANCE_M);
  
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
  // ── Pemulihan diri (kasus "setengah hidup") ──
  if (WiFi.status() == WL_CONNECTED) lastWifiOkMs = millis();
  if (mqttClient.connected()) {
    lastWifiOkMs = millis();
    lastMqttOkMs = millis();
  }
  if (millis() - lastWifiOkMs > WIFI_RESTART_MS) {
    safeRestart("WiFi tidak tersambung > 30 menit");
  }
  if (WiFi.status() == WL_CONNECTED && millis() - lastMqttOkMs > MQTT_RESTART_MS) {
    // Hanya saat WiFi ADA tapi broker tidak terjangkau: reboot memaksa DNS/TCP
    // handshake baru. Kalau WiFi-nya sendiri mati (internet desa putus), RX
    // TIDAK di-reboot — sirine/LED dari LoRa harus tetap hidup berjam-jam.
    safeRestart("MQTT tidak tersambung > 5 menit");
  }

  // Antrian notifikasi Telegram: dicoba kirim terus saat internet tersedia.
  serviceTelegram();

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if (reconnectMQTT()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    mqttClient.loop();
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
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    handleLoRaPacket(packetSize);
  }
#endif
}

void setupWiFi() {
  Serial.print("🌐 Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
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
    mqttClient.subscribe(CMD_TOPIC);   // perintah mulai run dari web
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

  // ─── TYPE=BEACON: TX mengumumkan "saya hidup, SF saya X" ───
  // Kalau SF TX beda dengan RX, RX langsung PINDAH ke SF TX (auto-sync) —
  // tanpa menunggu perintah web. Setelah itu data & handshake berjalan di SF baru.
  if (rawPacket.startsWith("TYPE=BEACON")) {
    int txSF = -1;
    int sfIdx = rawPacket.indexOf("SF=");
    if (sfIdx >= 0) txSF = rawPacket.substring(sfIdx + 3).toInt();
    lastLoRaRxMs = millis();   // bukti TX hidup — TAPI bukan tanda "run berjalan"
    if (txSF == 7 || txSF == 9 || txSF == 12) {
      if (txSF != activeSF) {
        activeSF = txSF;
        LoRa.setSpreadingFactor(activeSF);
        prefs.begin("ladet", false);
        prefs.putUChar("sf", (unsigned char)activeSF);
        prefs.end();
        Serial.printf("🔄 BEACON: TX di SF %d → RX pindah ke SF %d (NVS)\n", txSF, activeSF);
        publishConfigStatus("applied", true);
      }
    }
    if (!txOnlinePublished) {
      if (mqttClient.connected()) mqttClient.publish(TX_ONLINE_TOPIC, "online", true);
      txOnlinePublished = true;
    }
    Serial.printf("📻 BEACON dari TX: SF %d (RX SF %d)\n", txSF, activeSF);
    LoRa.receive();
    return;
  }

  // Tangkap rxTime SEGERA setelah paket diterima (sebelum Serial.print)
  unsigned long rxTime = millis();
  lastLoRaRxMs = rxTime;   // config remote: penanda kanal sedang dipakai run
  lastDataRxMs = rxTime;   // PAKET DATA = run benar-benar sedang berjalan
  if (!txOnlinePublished) {  // TX baru terlihat → laporkan ke dashboard (retained)
    if (mqttClient.connected()) mqttClient.publish(TX_ONLINE_TOPIC, "online", true);
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
  // ══════════════════════════════════════════════════════════════════════
  String ack = "ACK=" + String(data.seq);
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(ack);
  LoRa.endPacket();
  LoRa.receive();

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
    rawDiffMinValid     = (rawDiff0 >= 0);
    rawDiffMin          = (rawDiff0 >= 0) ? rawDiff0 : 0;
    rttSeen             = false;
    rttMissingWarned    = false;
    firstPacketReceived = false;
    lastReceivedSEQ     = -1;
    sameSeqStreak       = 0;
    seqStuck            = false;
    Serial.println();
    Serial.printf("🧭 PAKET #0 (KALIBRASI) — di-ACK (RTT utk paket #1), rawDiff=%ld ms. Tidak dipublish.\n", rawDiff0);
    return;
  }

  // Hitung paket yang benar-benar diterima (dipakai dashboard sebagai kunci
  // anti-duplikat & untuk peringatan SEQ macet).
  rxCount++;

  Serial.println();
  Serial.printf("═══════════════════════════════════════════════════════════\n");
  Serial.printf("📨 PAKET %d/%d (SEQ=%d) — %d bytes\n", data.seq, TOTAL_PACKETS, data.seq, packetSize);
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
    Serial.println("🔄 Pengujian baru terdeteksi — reset kalibrasi delay & loss.");
  }

  // ─── Packet Loss Tracking ───
  // rxTime sudah ditangkap di atas (segera setelah paket diterima)
  if (!firstPacketReceived) {
    lastReceivedSEQ = data.seq;
    totalExpected = 1;
    totalReceived = 1;
    firstPacketReceived = true;
    sameSeqStreak = 0;
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
      sameSeqStreak = 0;
    } else {
      // Gap terdeteksi: ada paket hilang
      unsigned long gap = data.seq - lastReceivedSEQ - 1;
      totalExpected += (1 + gap);  // 1 untuk paket ini + gap
      totalReceived += 1;
      Serial.printf("⚠️  GAP: SEQ %d → %d (%lu paket hilang!)\n",
                    lastReceivedSEQ, data.seq, gap);
      lastReceivedSEQ = data.seq;
      sameSeqStreak = 0;
    }
  }

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
  bool rttPlausible = (data.rtt_ms > 0.0 && data.rtt_ms < (float)DELAY_SANITY_MAX_MS);
  if (rttPlausible) rttSeen = true;

  long rawDiffMs = (data.send_ms > 0) ? ((long)rxTime - (long)data.send_ms) : -1;

  // Running minimum rawDiff → jaring pengaman relatif (lihat langkah 2)
  if (rawDiffMs >= 0 && (!rawDiffMinValid || rawDiffMs < rawDiffMin)) {
    rawDiffMin      = rawDiffMs;
    rawDiffMinValid = true;
  }

  // ── Langkah 1: offset ABSOLUT dari RTT paket sebelumnya ──
  // RTT/2 = delay satu arah paket tersebut (diukur jam TX sendiri, bebas
  // offset). Jadi offset_boot = rawDiff_paket_lalu − RTT/2. Setelah offset
  // diketahui, delay tiap paket = rawDiff − offset, persis t_terima − t_kirim.
  if (rttPlausible && prevRawDiffMs >= 0 && !clockOffsetRefined) {
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

  if (rawDiffMs >= 0 && clockOffsetValid) {
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
  // delay > 5 s mustahil untuk LoRa (airtime SF12 maksimum ≈ 2 s).
  if (delayMs > DELAY_SANITY_MAX_MS) {
    unsigned long rawDelayMs = delayMs;
    bool pakaiRtt = rttPlausible;
    if (pakaiRtt) {
      delayMs     = (unsigned long)(data.rtt_ms / 2.0);
      delaySource = "rtt";
    } else {
      delayMs     = 0;
      delaySource = "invalid";
    }
    Serial.printf(" ⚠️  delay %lu ms > sanity %lu ms → %s\n",
                  rawDelayMs, DELAY_SANITY_MAX_MS,
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
  Serial.printf(" 📤 ACK terkirim: \"%s\"\n", ack.c_str());
  Serial.println();

  // ── Diagnostik sekali jalan: apakah TX perlu di-upload ulang? ──
  // Kalau sampai paket ke-3 TX masih belum mengirim RTT=, berarti firmware TX
  // belum mendukungnya → delay yang terukur hanya RELATIF (bukan absolut).
  if (!rttSeen && !rttMissingWarned && data.seq >= 3) {
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
  }

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
  if (!mqttClient.connected()) {
    Serial.printf("⚠️ MQTT offline! ");
    return;
  }
  
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
  
  char jsonBuffer[1536];
  size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

  // Deteksi pemotongan JSON (buffer kurang) supaya tidak dikirim payload rusak.
  if (len >= sizeof(jsonBuffer)) {
    Serial.println("❌ JSON terpotong (buffer kurang) — payload TIDAK dikirim.");
    return;
  }

  boolean success = mqttClient.publish(MQTT_TOPIC, jsonBuffer);
  
  if (success) {
    Serial.printf("✅ MQTT sent (%d bytes)\n", len);
  } else {
    Serial.printf("❌ MQTT FAILED (%d bytes)\n", len);
  }
}
