/**
 * LADET RX — Realtime Landslide Gateway (v4.0 REALTIME)
 *
 * Dirancang khusus untuk implementasi lapangan realtime yang ringan, cepat pulih,
 * dan tahan gangguan:
 * - LoRa SF7 permanen, listen-only (TIDAK pernah switch ke TX untuk kirim ACK
 *   sehingga tidak pernah mengalami lonjakan arus PA / radio tuli setelah paket #1)
 * - Tanpa SEQ, tanpa loss tracking, tanpa NVS write berulang, tanpa antrean RAM berat
 * - Task Watchdog 15 detik + auto-recovery radio SX1278 setiap 1 detik
 * - Aktuator lokal (LED + Buzzer) & LCD 20x4 tetap berjalan meski WiFi/MQTT mati
 * - Notifikasi Telegram 24 jam otomatis saat level berubah (+ pengingat 5 menit)
 *
 * Hardware : ESP32 + LoRa SX1278 + Relay 4CH + LCD 20x4 I2C (opsional)
 * Wiring   : LoRa SCK=18, MISO=19, MOSI=23, SS=5, RST=14, DIO0=4
 *            Relay Hijau=27, Kuning=26, Merah+Buzzer=25, BuzzerWaspada=33
 *            LCD SDA=21, SCL=22
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LoRa.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// =====================================================
// KONFIGURASI JARINGAN & TELEGRAM
// =====================================================
const char* WIFI_SSID   = "Shell Ecomarathon";
const char* WIFI_PASS   = "nopassword";
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;

const char* MQTT_DATA_TOPIC = "ladet/data";
const char* RX_ONLINE_TOPIC = "ladet/rx/online";
const char* TX_ONLINE_TOPIC = "ladet/tx/online";

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef TELEGRAM_BOT_TOKEN
#define TELEGRAM_BOT_TOKEN ""
#endif
#ifndef TELEGRAM_CHAT_ID
#define TELEGRAM_CHAT_ID   ""
#endif
// Database riwayat peringatan (opsional). Kosong = dilewati.
#ifndef SUPABASE_URL
#define SUPABASE_URL       ""
#endif
#ifndef SUPABASE_ANON_KEY
#define SUPABASE_ANON_KEY  ""
#endif
// Update firmware lewat WiFi (hostname ladet-rx di jaringan yang sama).
#ifndef OTA_PASSWORD
#define OTA_PASSWORD       "ladet-ota"
#endif

// =====================================================
// PIN & RADIO (SF7 PERMANEN)
// =====================================================
#define LORA_SCK        18
#define LORA_MISO       19
#define LORA_MOSI       23   // RX pakai GPIO23 (GPIO25 dipakai relay merah)
#define LORA_SS         5
#define LORA_RST        14
#define LORA_DIO0       4

#define LORA_FREQ       433E6
#define LORA_BW         125E3
#define LORA_CR         5
#define LORA_SF         7
#define LORA_SYNC_WORD  0x12

#define PIN_LED_RED     25   // merah + buzzer BAHAYA
#define PIN_LED_YELLOW  26
#define PIN_LED_GREEN   27
#define PIN_BUZZER      33   // buzzer WASPADA
#define RELAY_ACTIVE_LOW 0   // 0 = aktif HIGH, 1 = aktif LOW

#define LCD_SDA         21
#define LCD_SCL         22
#define LCD_COLS        20
#define LCD_ROWS        4

// =====================================================
// TIMING & WATCHDOG CEPAT
// =====================================================
#define WDT_TIMEOUT_MS      15000    // reboot otomatis jika loop hang > 15 detik
#define RADIO_CHECK_MS      1000UL   // cek register SX1278 tiap 1 detik
#define RADIO_DEAF_MS       6000UL   // re-init jika sunyi > 6 detik saat TX aktif
#define RADIO_COOLDOWN_MS   5000UL
#define TX_OFFLINE_MS       15000UL  // TX dianggap offline jika > 15 detik sunyi
#define HEARTBEAT_DB_MS     86400000UL  // heartbeat ke database tiap 24 jam

// =====================================================
// GLOBAL STATE
// =====================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);

bool wdtArmed = false;
unsigned long lastMqttAttemptMs   = 0;
unsigned long mqttBackoffMs       = 3000UL;
unsigned long lastWifiCheckMs     = 0;
unsigned long lastOnlinePublishMs = 0;

unsigned long lastLoRaRxMs        = 0;
unsigned long lastHeartbeatMs     = 0;
unsigned long lastHeartbeatDbMs   = 0;
unsigned long heartbeatDbNextMs   = 60000UL;  // percobaan pertama 1 menit setelah boot
unsigned long lastRadioCheckMs    = 0;
unsigned long lastRadioRecoverMs  = 0;
bool radioEverHeard               = false;
uint8_t radioRecoverAttempts      = 0;
bool txOnlinePublished            = false;

// Aktuator & Alert
const char* LEVEL_NAME[4] = { "AMAN", "HATI-HATI", "WASPADA", "BAHAYA" };
int8_t relayState[4]      = { -1, -1, -1, -1 };
int    lastActuatorLevel  = -1;
int    lastAlertLevel     = -1;
unsigned long lastAlertMs = 0;
String pendingAlert       = "";
int    pendingAlertLevel  = 0;
unsigned long pendingAlertAtMs     = 0;
unsigned long lastTelegramAttemptMs = 0;
unsigned long telegramRetryMs       = 10000UL;

// Peringatan terakhir, dikirim retained ke broker supaya dashboard langsung
// menampilkan kejadian walau baru dibuka. Riwayat lengkap ada di Supabase.
#define ALERT_LOG_MAX 1
const char* ALERT_LOG_TOPIC = "ladet/alert/log";

struct AlertLogEntry {
  char    time[20];
  uint8_t level;
  float   sat;
  float   dg;
  float   dth;
};

AlertLogEntry alertLog[ALERT_LOG_MAX];
uint8_t alertLogCount = 0;

// LCD 20x4
LiquidCrystal_I2C* lcd = nullptr;
String lcdCache[LCD_ROWS];
float  lcdSat  = 0.0f;
float  lcdDg   = 0.0f;
float  lcdTilt = 0.0f;
int    lcdLevel = -1;
unsigned long lastLcdMs = 0;

struct SensorData {
  float sat;
  float dg;
  float dth;
  float vbat;
  int   rssi;
  float snr;
  char  status[16];
  bool  valid;
};

// =====================================================
// DIAGNOSTIK & WATCHDOG
// =====================================================
const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_RESET";
    case ESP_RST_SW:        return "SW_RESTART";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "UNKNOWN";
  }
}

void setupWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;

  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err != ESP_OK) err = esp_task_wdt_init(&cfg);
  esp_err_t addErr = esp_task_wdt_add(NULL);
  wdtArmed = (err == ESP_OK) && (addErr == ESP_OK || addErr == ESP_ERR_INVALID_ARG);
  Serial.printf("Watchdog RX     : %s (%d detik)\n", wdtArmed ? "aktif" : "nonaktif", WDT_TIMEOUT_MS / 1000);
}

void setupClock() {
  configTime(7 * 3600, 0, "id.pool.ntp.org", "pool.ntp.org", "time.google.com");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("🕐 Sinkronisasi NTP");
    unsigned long t0 = millis();
    while (time(nullptr) <= 1700000000 && millis() - t0 < 4000UL) {
      delay(100);
    }
    Serial.println(time(nullptr) > 1700000000 ? " OK" : " timeout (lanjut sinkron di latar)");
  }
}

String nowStamp() {
  time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tmBuf;
    localtime_r(&t, &tmBuf);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return String(buf);
  }
  return "uptime " + String(millis() / 1000UL) + " s";
}

// =====================================================
// AKTUATOR LOKAL (RELAY)
// =====================================================
void setRelay(int pin, bool on) {
#if RELAY_ACTIVE_LOW
  digitalWrite(pin, on ? LOW : HIGH);
#else
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

void applyRelayChannel(uint8_t idx, int pin, bool on) {
  int8_t want = on ? 1 : 0;
  if (relayState[idx] == want) return;
  setRelay(pin, on);
  relayState[idx] = want;
  delay(25);
}

void setupActuators() {
  pinMode(PIN_LED_GREEN,  OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED,    OUTPUT);
  pinMode(PIN_BUZZER,     OUTPUT);
  setRelay(PIN_LED_GREEN,  false);
  setRelay(PIN_LED_YELLOW, false);
  setRelay(PIN_LED_RED,    false);
  setRelay(PIN_BUZZER,     false);
  for (int i = 0; i < 4; i++) relayState[i] = 0;
}

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

void applyActuators(int level) {
  if (level < 0) return;
  applyRelayChannel(0, PIN_LED_GREEN,  level == 0);
  applyRelayChannel(1, PIN_LED_YELLOW, level == 1 || level == 2);
  applyRelayChannel(2, PIN_BUZZER,     level == 2);  // WASPADA; BAHAYA punya buzzer sendiri di kanal merah
  applyRelayChannel(3, PIN_LED_RED,    level == 3);

  if (level != lastActuatorLevel) {
    Serial.printf("🔔 AKTUATOR → %s\n", LEVEL_NAME[level]);
    lastActuatorLevel = level;
  }
}

// =====================================================
// LCD 20x4 I2C (OPSIONAL)
// =====================================================
uint8_t scanLcdAddress() {
  const uint8_t common[] = { 0x27, 0x3F, 0x20, 0x26, 0x38, 0x3E };
  for (uint8_t i = 0; i < sizeof(common); i++) {
    Wire.beginTransmission(common[i]);
    if (Wire.endTransmission() == 0) return common[i];
  }
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

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
  uint8_t addr = scanLcdAddress();
  if (addr == 0) {
    Serial.println("LCD 20x4        : tidak ditemukan (dilewati)");
    return;
  }
  lcd = new LiquidCrystal_I2C(addr, LCD_COLS, LCD_ROWS);
  lcd->begin(LCD_COLS, LCD_ROWS);
  lcd->backlight();
  lcd->clear();
  for (uint8_t i = 0; i < LCD_ROWS; i++) lcdCache[i] = "";
  Serial.printf("LCD 20x4        : OK (0x%02X)\n", addr);
  lcdLine(0, "LADET SIGAP CARE");
  lcdLine(1, "STATUS: MENUNGGU TX");
  lcdLine(2, "Kelembapan: --%");
  lcdLine(3, "Kemiringan: --");
}

const char* kondisiLerengLabel(float dg, float dth) {
  if (dth >= 5.0f)  return "Bahaya";
  if (dth >= 1.5f)  return "Miring";
  if (dg >= 0.50f)  return "Waspada";
  if (dg >= 0.15f)  return "Bergetar";
  return "Aman";
}

void serviceLcd() {
  if (!lcd || millis() - lastLcdMs < 500UL) return;
  lastLcdMs = millis();

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  lcdLine(0, wifiOk ? "LADET SIGAP CARE" : "LADET  [WIFI HILANG]");

  String status;
  if (lcdLevel < 0) {
    status = "MENUNGGU TX";
  } else if (lastLoRaRxMs == 0 || millis() - lastLoRaRxMs > TX_OFFLINE_MS) {
    status = "TX HILANG";
  } else {
    status = LEVEL_NAME[lcdLevel];
  }
  lcdLine(1, "STATUS: " + status);

  if (lcdLevel >= 0) {
    lcdLine(2, "Kelembapan: " + String(lcdSat, 1) + "%");
    lcdLine(3, String("Kemiringan: ") + kondisiLerengLabel(lcdDg, lcdTilt));
  } else {
    lcdLine(2, "Kelembapan: --%");
    lcdLine(3, "Kemiringan: --");
  }
}

// =====================================================
// TELEGRAM (24/7 DARI RX)
// =====================================================
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

bool sendTelegram(const String& text) {
  if (!telegramConfigured() || WiFi.status() != WL_CONNECTED) return false;

  // Statis: objek TLS dipakai ulang. Membuat-buang tiap notifikasi memecah
  // heap dan membuat RX crash setelah berhari-hari menyala.
  static WiFiClientSecure client;
  client.setInsecure();
  client.setConnectionTimeout(2000);
  client.setTimeout(2000);
  client.setHandshakeTimeout(3);

  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("📵 Telegram: gagal konek");
    return false;
  }

  String body = "chat_id=" + urlEncode(TELEGRAM_CHAT_ID) + "&text=" + urlEncode(text);
  client.print("POST /bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage HTTP/1.1\r\n");
  client.print("Host: api.telegram.org\r\n");
  client.print("Content-Type: application/x-www-form-urlencoded\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  String statusLine;
  unsigned long t0 = millis();
  while (client.connected() && millis() - t0 < 2500) {
    if (wdtArmed) esp_task_wdt_reset();
    if (!client.available()) { delay(10); continue; }
    statusLine = client.readStringUntil('\n');
    break;
  }
  client.stop();

  bool ok = statusLine.indexOf(" 200") > 0;
  Serial.printf("📨 Telegram: %s\n", ok ? "terkirim" : statusLine.c_str());
  return ok;
}

String formatWaktuWIB() {
  time_t t = time(nullptr);
  if (t > 1700000000) {
    static const char* BULAN[12] = {
      "Januari", "Februari", "Maret", "April", "Mei", "Juni",
      "Juli", "Agustus", "September", "Oktober", "November", "Desember"
    };
    struct tm tmBuf;
    localtime_r(&t, &tmBuf);
    char buf[64];
    snprintf(buf, sizeof(buf), "%d %s %d\n%02d:%02d:%02d WIB",
             tmBuf.tm_mday,
             BULAN[tmBuf.tm_mon % 12],
             tmBuf.tm_year + 1900,
             tmBuf.tm_hour,
             tmBuf.tm_min,
             tmBuf.tm_sec);
    return String(buf);
  }
  return "uptime " + String(millis() / 1000UL) + " detik";
}

bool supabaseConfigured() {
  return strlen(SUPABASE_URL) > 10 && strlen(SUPABASE_ANON_KEY) > 20;
}

// Kirim satu baris JSON ke tabel Supabase (dipakai peringatan & heartbeat).
bool supabasePost(const char* path, const char* body) {
  if (!supabaseConfigured() || WiFi.status() != WL_CONNECTED) return false;

  String host = String(SUPABASE_URL);
  int scheme = host.indexOf("://");
  if (scheme > 0) host = host.substring(scheme + 3);
  if (host.endsWith("/")) host.remove(host.length() - 1);

  static WiFiClientSecure client;
  client.setInsecure();
  client.setConnectionTimeout(2500);
  client.setTimeout(2500);
  client.setHandshakeTimeout(3);

  if (!client.connect(host.c_str(), 443)) {
    Serial.println("🗄️  Supabase: gagal konek");
    return false;
  }

  client.printf("POST %s HTTP/1.1\r\n", path);
  client.printf("Host: %s\r\n", host.c_str());
  client.print("apikey: " SUPABASE_ANON_KEY "\r\n");
  client.print("Authorization: Bearer " SUPABASE_ANON_KEY "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Prefer: return=minimal\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)strlen(body));
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  String statusLine;
  unsigned long t0 = millis();
  while (client.connected() && millis() - t0 < 2500) {
    if (wdtArmed) esp_task_wdt_reset();
    if (!client.available()) { delay(10); continue; }
    statusLine = client.readStringUntil('\n');
    break;
  }
  client.stop();

  bool ok = statusLine.indexOf(" 201") > 0 || statusLine.indexOf(" 200") > 0;
  Serial.printf("🗄️  Supabase %s: %s\n", path, ok ? "tersimpan" : statusLine.c_str());
  return ok;
}

bool pushAlertSupabase(int level, const SensorData& d) {
  char body[176];
  int len = snprintf(body, sizeof(body),
                     "{\"level\":%d,\"level_name\":\"%s\",\"soil_saturation\":%.2f,"
                     "\"delta_g\":%.3f,\"tilt_angle\":%.2f}",
                     level, LEVEL_NAME[level], d.sat * 100.0f, d.dg, d.dth);
  if (len <= 0 || len >= (int)sizeof(body)) return false;
  return supabasePost("/rest/v1/ladet_alerts", body);
}

// Denyut harian: bukti perangkat masih hidup, sekaligus menjaga project
// Supabase free tier agar tidak di-pause karena 7 hari tanpa aktivitas.
bool pushHeartbeatSupabase() {
  char body[192];
  unsigned long ageS = lastLoRaRxMs ? (millis() - lastLoRaRxMs) / 1000UL : 0;
  int len = snprintf(body, sizeof(body),
                     "{\"uptime_s\":%lu,\"free_heap\":%lu,\"reset_reason\":\"%s\","
                     "\"wifi_rssi\":%d,\"last_packet_age_s\":%lu}",
                     millis() / 1000UL, (unsigned long)ESP.getFreeHeap(),
                     resetReasonName(), (int)WiFi.RSSI(), ageS);
  if (len <= 0 || len >= (int)sizeof(body)) return false;
  return supabasePost("/rest/v1/ladet_heartbeat", body);
}

void pushAlertLog(int level, const SensorData& d) {
  if (level <= 0) return;
  if (alertLogCount >= ALERT_LOG_MAX) {
    for (uint8_t i = 1; i < ALERT_LOG_MAX; i++) alertLog[i - 1] = alertLog[i];
    alertLogCount = ALERT_LOG_MAX - 1;
  }
  AlertLogEntry& e = alertLog[alertLogCount++];
  snprintf(e.time, sizeof(e.time), "%s", nowStamp().c_str());
  e.level = (uint8_t)level;
  e.sat   = d.sat * 100.0f;
  e.dg    = d.dg;
  e.dth   = d.dth;
}

void publishAlertLog() {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < alertLogCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["t"] = alertLog[i].time;
    o["l"] = alertLog[i].level;
    o["s"] = alertLog[i].sat;
    o["g"] = alertLog[i].dg;
    o["d"] = alertLog[i].dth;
  }
  char buf[256];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len > 0 && len < sizeof(buf)) {
    mqttClient.publish(ALERT_LOG_TOPIC, buf, true);
  }
}

void alertIfNeeded(const SensorData& d, int level) {
  if (level <= 0) {
    lastAlertLevel = level;
    return;
  }
  bool isNew  = (level != lastAlertLevel);
  bool remind = (level >= 2) && (millis() - lastAlertMs > 300000UL);
  if (!isNew && !remind) return;

  // Peringatan yang belum terkirim tidak boleh ditimpa level yang lebih ringan.
  if (pendingAlert.length() > 0 && level < pendingAlertLevel) return;

  lastAlertLevel = level;
  lastAlertMs = millis();
  pendingAlertLevel = level;

  pushAlertLog(level, d);
  publishAlertLog();
  pushAlertSupabase(level, d);

  if (!telegramConfigured()) return;

  static const char* LEVEL_EMOJI[4]  = { "🟢", "🟡", "🟠", "🔴" };
  static const char* LEVEL_ACTION[4] = {
    "Kondisi tanah normal. Tidak ada tindakan diperlukan.",
    "Kelembapan tanah mulai tinggi. Pantau kondisi lereng secara berkala.",
    "Terdeteksi getaran tanah. Siapkan jalur evakuasi dan pantau terus menerus.",
    "SEGERA EVAKUASI! Kemiringan tanah melebihi batas aman. Tinggalkan area sekarang!"
  };

  String msg;
  msg.reserve(360);
  msg += String(LEVEL_EMOJI[level]) + " PERINGATAN LADET — " + String(LEVEL_NAME[level]) + "\n";
  msg += String(level == 0 ? "✅ " : "⚠️ ") + String(LEVEL_ACTION[level]) + "\n\n";
  msg += "📍 Lokasi: Desa Klungkung, Sukorambi, Jember\n";
  msg += "🕐 Waktu: {{WAKTU}}\n\n";
  msg += "📊 Data Sensor:\n";
  msg += "• Kelembapan Tanah  : " + String(d.sat * 100.0f, 1) + "%\n";
  msg += "• Getaran (Δg)      : " + String(d.dg, 3) + "\n";
  msg += "• Sudut Kemiringan  : " + String(d.dth, 2) + "°\n\n";
  msg += "━━━━━━━━━━━━━━━━━━━\n";
  msg += "📡 Landslide Early Detector Tool (LADET)";

  pendingAlert = msg;
  pendingAlertAtMs = millis();
}

void serviceTelegram() {
  if (pendingAlert.length() == 0) return;
  if (!telegramConfigured()) { pendingAlert = ""; return; }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - pendingAlertAtMs > 30UL * 60UL * 1000UL) {
    pendingAlert = "";
    return;
  }
  // Tunggu sebentar jika NTP baru mulai sinkron setelah WiFi connect (maks 5 detik)
  if (time(nullptr) <= 1700000000 && millis() - pendingAlertAtMs < 5000UL) {
    return;
  }
  if (millis() - lastTelegramAttemptMs < telegramRetryMs) return;
  lastTelegramAttemptMs = millis();

  String msgToSend = pendingAlert;
  msgToSend.replace("{{WAKTU}}", formatWaktuWIB());

  if (sendTelegram(msgToSend)) {
    pendingAlert = "";
    telegramRetryMs = 10000UL;
  } else if (telegramRetryMs < 120000UL) {
    telegramRetryMs *= 2;
  }
}

// =====================================================
// LORA RADIO + AUTO-RECOVERY CEPAT
// =====================================================
uint8_t loraRegRead(uint8_t reg) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return value;
}

bool initLoRaRadio() {
  LoRa.end();
  delay(20);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 1; i <= 3; i++) {
    if (LoRa.begin(LORA_FREQ)) {
      LoRa.setSpreadingFactor(LORA_SF);
      LoRa.setSignalBandwidth(LORA_BW);
      LoRa.setCodingRate4(LORA_CR);
      LoRa.setSyncWord(LORA_SYNC_WORD);
      LoRa.enableCrc();
      LoRa.receive();
      return true;
    }
    delay(100);
  }
  return false;
}

void recoverLoRa(const char* reason) {
  Serial.printf("♻️  RX RADIO re-init (%s)\n", reason);
  lastRadioRecoverMs = millis();
  if (initLoRaRadio()) {
    Serial.printf("   ✅ RX RADIO pulih (SF7, OP=0x%02X)\n", loraRegRead(0x01));
  } else {
    Serial.println("❌ RX RADIO gagal pulih — cek kabel SPI / 3V3");
  }
}

void superviseLoRa() {
  unsigned long now = millis();
  if (now - lastRadioCheckMs < RADIO_CHECK_MS) return;
  lastRadioCheckMs = now;

  uint8_t ver = loraRegRead(0x42);
  uint8_t op  = loraRegRead(0x01);
  if (ver != 0x12 || (op & 0x80) == 0) {
    if (now - lastRadioRecoverMs >= RADIO_COOLDOWN_MS) {
      recoverLoRa(ver != 0x12 ? "SPI tidak merespons" : "keluar mode LoRa");
    }
    return;
  }

  if (!radioEverHeard) return;
  if (now - lastLoRaRxMs < RADIO_DEAF_MS) return;

  unsigned long cooldown = (radioRecoverAttempts >= 3) ? 30000UL : RADIO_COOLDOWN_MS;
  if (now - lastRadioRecoverMs < cooldown) return;

  radioRecoverAttempts++;
  recoverLoRa("tuli > 6 dtk");
}

// =====================================================
// PARSING & MQTT REALTIME
// =====================================================
SensorData parsePacket(const String& raw, int rssi, float snr) {
  SensorData d = {};
  d.rssi = rssi;
  d.snr  = snr;
  strcpy(d.status, "AMAN");

  int start = 0;
  while (start < (int)raw.length()) {
    int comma = raw.indexOf(',', start);
    String token = (comma < 0) ? raw.substring(start) : raw.substring(start, comma);
    start = (comma < 0) ? raw.length() : comma + 1;

    int eq = token.indexOf('=');
    if (eq < 0) continue;
    String key = token.substring(0, eq);
    String val = token.substring(eq + 1);
    key.trim();
    val.trim();

    if (key == "SAT")         d.sat = val.toFloat();
    else if (key == "DG")     d.dg  = val.toFloat();
    else if (key == "DTH")    d.dth = val.toFloat();
    else if (key == "VBAT")   d.vbat = val.toFloat();
    else if (key == "STATUS") { val.toCharArray(d.status, sizeof(d.status)); d.valid = true; }
  }
  return d;
}

void publishRealtimeMQTT(const SensorData& d) {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["type"]      = "DATA";
  doc["timestamp"] = nowStamp();

  String st = String(d.status);
  st.toLowerCase();
  doc["status"] = st;

  JsonObject sensor = doc["sensor"].to<JsonObject>();
  sensor["soil_saturation"] = d.sat * 100.0f;
  sensor["delta_g"]         = d.dg;
  sensor["tilt_angle"]      = d.dth;
  if (d.vbat > 0.0f) sensor["battery_v"] = d.vbat;

  JsonObject radio = doc["radio"].to<JsonObject>();
  radio["sf"]   = "SF7";
  radio["rssi"] = d.rssi;
  radio["snr"]  = d.snr;

  char buf[384];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len > 0 && len < sizeof(buf)) {
    bool ok = mqttClient.publish(MQTT_DATA_TOPIC, buf);
    Serial.printf("📤 MQTT (%u B): %s\n", (unsigned)len, ok ? "OK" : "GAGAL");
  }
}

void handleLoRaPacket(int packetSize) {
  String raw;
  raw.reserve(packetSize + 1);
  while (LoRa.available()) raw += (char)LoRa.read();

  lastLoRaRxMs = millis();
  radioEverHeard = true;
  radioRecoverAttempts = 0;

  if (!txOnlinePublished) {
    txOnlinePublished = true;
    if (mqttClient.connected()) {
      mqttClient.publish(TX_ONLINE_TOPIC, "online", true);
    }
  }

  int rssi  = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  SensorData d = parsePacket(raw, rssi, snr);

  Serial.printf("📨 RX [%d dBm | SNR %.1f]: Sat=%.1f%% dG=%.3f dTh=%.2f° → %s\n",
                rssi, snr, d.sat * 100.0f, d.dg, d.dth, d.status);
  if (d.vbat > 0.0f) Serial.printf("🔋 Baterai TX: %.2f V\n", d.vbat);

  // Paket terpotong (CRC bisa lolos pada payload pendek) tidak boleh
  // ditampilkan sebagai AMAN. Buang, tunggu paket berikutnya 2 detik lagi.
  if (!d.valid) {
    Serial.printf("⚠️  Paket tanpa STATUS dibuang: %s\n", raw.c_str());
    return;
  }

  int level = statusToLevel(d.status);
  applyActuators(level);
  alertIfNeeded(d, level);

  lcdLevel = level;
  lcdSat   = d.sat * 100.0f;
  lcdDg    = d.dg;
  lcdTilt  = d.dth;

  publishRealtimeMQTT(d);
}

// =====================================================
// UPDATE VIA WIFI (OTA)
// =====================================================
void setupOTA() {
  ArduinoOTA.setHostname("ladet-rx");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    // Menulis flash menahan loop jauh lebih lama dari 15 detik, jadi task
    // ini harus dilepas dari watchdog lebih dulu supaya tidak reboot sendiri.
    if (wdtArmed) {
      esp_task_wdt_delete(NULL);
      wdtArmed = false;
    }
    Serial.println("⬆️  OTA mulai — jangan matikan daya");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n✅ OTA selesai, perangkat restart");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("❌ OTA gagal (kode %u)\n", (unsigned)e);
  });
  ArduinoOTA.begin();
  Serial.println("OTA             : siap (hostname ladet-rx)");
}

// =====================================================
// WIFI & MQTT
// =====================================================
void setupWiFi() {
  Serial.printf("🌐 Menghubungkan WiFi: %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" OK (%s)\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(" belum terhubung (lanjut offline, auto-retry di loop)");
  }
}

void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiCheckMs < 10000UL) return;
  lastWifiCheckMs = millis();
  Serial.println("🌐 WiFi putus → reconnect...");
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

bool reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String clientId = "LADET-RX-" + String(random(0xffff), HEX);
  if (mqttClient.connect(clientId.c_str(), RX_ONLINE_TOPIC, 1, true, "offline")) {
    Serial.println("✅ MQTT terhubung");
    mqttClient.publish(RX_ONLINE_TOPIC, "online", true);
    mqttClient.publish(TX_ONLINE_TOPIC, txOnlinePublished ? "online" : "offline", true);
    publishAlertLog();   // retained: riwayat tetap ada setelah broker/browser baru konek
    return true;
  }
  Serial.printf("❌ MQTT gagal (rc=%d)\n", mqttClient.state());
  return false;
}

// =====================================================
// SETUP & LOOP
// =====================================================
void setup() {
  setupActuators();

  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1200UL) delay(10);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   LADET RX v4.0 — REALTIME (SF7, LISTEN-ONLY)");
  Serial.println("==================================================");
  Serial.printf("Reset terakhir  : %s\n", resetReasonName());

  setupLCD();
  setupWiFi();
  setupClock();
  setupOTA();

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(1024);
  // Kirim Telegram bisa memblokir loop sampai ~5 detik; keepalive default 15
  // detik membuat broker memutus RX saat sedang mengirim notifikasi.
  mqttClient.setKeepAlive(30);

  if (!initLoRaRadio()) {
    Serial.println("❌ LoRa SX1278 gagal init → restart otomatis");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  Serial.println("LoRa SX1278     : OK (433 MHz, SF7, Listen-Only)");

  setupWatchdog();
  Serial.println("✅ RX Realtime Gateway siap!");
}

void loop() {
  if (wdtArmed) esp_task_wdt_reset();

  serviceWiFi();
  ArduinoOTA.handle();

  if (!mqttClient.connected()) {
    if (WiFi.status() == WL_CONNECTED && millis() - lastMqttAttemptMs > mqttBackoffMs) {
      lastMqttAttemptMs = millis();
      if (reconnectMQTT()) {
        mqttBackoffMs = 3000UL;
      } else if (mqttBackoffMs < 15000UL) {
        mqttBackoffMs = min(mqttBackoffMs * 2UL, 15000UL);
      }
    }
  } else {
    mqttClient.loop();

    if (millis() - lastOnlinePublishMs > 20000UL) {
      lastOnlinePublishMs = millis();
      mqttClient.publish(RX_ONLINE_TOPIC, "online", true);
      mqttClient.publish(TX_ONLINE_TOPIC, txOnlinePublished ? "online" : "offline", true);
    }

    if (txOnlinePublished && (millis() - lastLoRaRxMs > TX_OFFLINE_MS)) {
      txOnlinePublished = false;
      mqttClient.publish(TX_ONLINE_TOPIC, "offline", true);
      Serial.println("📡 TX offline (>15 dtk tanpa paket)");
    }
  }

  superviseLoRa();
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    handleLoRaPacket(packetSize);
  }

  serviceTelegram();
  serviceLcd();

  // Denyut hidup tiap 5 menit: bahan uji 24 jam. Heap yang terus turun = bocor.
  if (millis() - lastHeartbeatMs > 300000UL) {
    lastHeartbeatMs = millis();
    unsigned long ageS = lastLoRaRxMs ? (millis() - lastLoRaRxMs) / 1000UL : 0;
    Serial.printf("⏱️  RX hidup %lu dtk | heap %lu B | WiFi %s | MQTT %s | paket terakhir %lu dtk lalu\n",
                  millis() / 1000UL, (unsigned long)ESP.getFreeHeap(),
                  WiFi.status() == WL_CONNECTED ? "OK" : "PUTUS",
                  mqttClient.connected() ? "OK" : "PUTUS", ageS);
  }

  // Heartbeat harian ke database: jejak perangkat hidup + penjaga agar project
  // Supabase free tier tidak di-pause karena seminggu tanpa aktivitas.
  // Gagal (mis. internet putus) → coba lagi 10 menit, bukan menunggu sehari.
  if (millis() - lastHeartbeatDbMs >= heartbeatDbNextMs) {
    lastHeartbeatDbMs = millis();
    heartbeatDbNextMs = pushHeartbeatSupabase() ? HEARTBEAT_DB_MS : 600000UL;
  }

  if (wdtArmed) esp_task_wdt_reset();
}
