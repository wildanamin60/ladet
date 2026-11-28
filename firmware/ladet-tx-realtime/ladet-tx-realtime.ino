/**
 * LADET TX - Realtime Landslide Sensor Transmitter (v4.0 REALTIME)
 *
 * Dirancang khusus untuk implementasi lapangan realtime yang ringan & andal:
 * - LoRa SF7 permanen (433 MHz, BW 125 kHz, CR 4/5, TX Power 4 dBm)
 * - Tanpa SEQ, tanpa paket kalibrasi #0, tanpa tunggu ACK (non-blocking)
 * - Payload ringkas (~55 byte) → airtime sangat singkat, minim risiko brownout
 * - Task Watchdog 15 detik + auto-recovery radio SX1278 jika chip macet
 *
 * Hardware : ESP32 + LoRa SX1278 + ADXL345 + BNO055 + Soil (SEN0308)
 * Wiring   : LoRa SCK=18, MISO=19, MOSI=25, SS=5, RST=14, DIO0=4
 *            I2C SDA=21, SCL=22 | Soil=34
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <math.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_BNO055.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// =====================================================
// PIN CONFIGURATION
// =====================================================
#define SDA_PIN       21
#define SCL_PIN       22
#define SOIL_PIN      34

#define LORA_SCK      18
#define LORA_MISO     19
#define LORA_MOSI     25
#define LORA_SS       5
#define LORA_RST      14
#define LORA_DIO0     4

// =====================================================
// LORA CONFIGURATION (SF7 PERMANEN UNTUK REALTIME)
// =====================================================
#define LORA_FREQUENCY    433E6
#define LORA_BW           125E3
#define LORA_CR           5
#define LORA_SF           7
#define LORA_SYNC_WORD    0x12
// 4 dBm terbukti stabil pada regulator AMS1117 tanpa brownout
#define LORA_TX_POWER     4

#define PACKET_INTERVAL   2000UL   // kirim setiap 2 detik
#define WDT_TIMEOUT_MS    15000    // watchdog 15 detik → reboot cepat jika hang

// Telemetri baterai (opsional). Aktifkan setelah pembagi tegangan dipasang:
//   Baterai+ ── R1 100k ──┬── GPIO35
//                         └── R2 100k ── GND
// Rasio 2.0 karena tegangan dibagi dua. Tanpa pembagi pin melayang → angka acak.
#define BATTERY_MONITOR   0
#define BATTERY_PIN       35
#define BATTERY_RATIO     2.0f

// =====================================================
// SENSOR THRESHOLD (TUPOKSI)
// =====================================================
#define SOIL_THRESHOLD          0.60f  // > 60%   → HATI-HATI
#define DELTA_G_THRESHOLD       0.50f  // > 0.50g → WASPADA
#define DELTA_THETA_THRESHOLD   5.0f   // > 5.0°  → BAHAYA
#define TILT_EMA_ALPHA          0.15f  // perataan pitch/roll; makin kecil makin halus
#define TILT_WARMUP_MS          15000UL // abaikan kemiringan saat sensor baru menyala

// =====================================================
// SOIL CALIBRATION
// =====================================================
// Hasil ukur probe di lokasi (menu 1 kalibrasi): kering 2826, terendam 18
const float SOIL_ADC_DRY = 2826.0f;
const float SOIL_ADC_WET = 18.0f;
const float THETA_R      = 0.05f;
const float THETA_S      = 0.45f;

// =====================================================
// OBJECTS & BASELINE
// =====================================================
Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified(12345);
Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28);

float gBaseline = 1.0f;
float pitchBaseline = 0.0f;
float rollBaseline  = 0.0f;
float pitchFiltered = 0.0f;
float rollFiltered  = 0.0f;
bool  tiltFilterReady = false;
unsigned long tiltWarmupUntilMs = 0;
bool wdtArmed = false;

struct SensorData {
  float saturation;
  float deltaG;
  float deltaTheta;
  const char* status;
};

// =====================================================
// HELPERS
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
  Serial.printf("Watchdog TX     : %s (%d detik)\n", wdtArmed ? "aktif" : "nonaktif", WDT_TIMEOUT_MS / 1000);
}

void bootRequire(const char* name, bool (*probe)(), int tries) {
  for (int i = 1; i <= tries; i++) {
    if (probe()) return;
    Serial.printf("   %s gagal (%d/%d)\n", name, i, tries);
    delay(400);
  }
  Serial.printf("ERROR: %s tidak merespons → restart otomatis\n", name);
  Serial.flush();
  delay(200);
  ESP.restart();
}

uint8_t loraRegRead(uint8_t reg) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t val = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return val;
}

bool initLoRaRadio() {
  LoRa.end();
  delay(20);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 1; i <= 3; i++) {
    if (LoRa.begin(LORA_FREQUENCY)) {
      LoRa.setSpreadingFactor(LORA_SF);
      LoRa.setSignalBandwidth(LORA_BW);
      LoRa.setCodingRate4(LORA_CR);
      LoRa.setTxPower(LORA_TX_POWER);
      LoRa.setSyncWord(LORA_SYNC_WORD);
      LoRa.enableCrc();
      LoRa.idle();
      return true;
    }
    delay(100);
  }
  return false;
}

void ensureRadioHealthy() {
  uint8_t ver = loraRegRead(0x42);
  uint8_t op  = loraRegRead(0x01);
  if (ver != 0x12 || (op & 0x80) == 0) {
    Serial.printf("♻️  TX RADIO tidak sehat (VER=0x%02X OP=0x%02X) → re-init\n", ver, op);
    if (initLoRaRadio()) {
      Serial.println("   ✅ TX RADIO pulih (SF7)");
    } else {
      Serial.println("❌ TX RADIO gagal pulih → restart ESP32");
      Serial.flush();
      delay(200);
      ESP.restart();
    }
  }
}

// =====================================================
// SENSOR & BASELINE
// =====================================================
float calculateSaturation(int raw) {
  float ratio = (SOIL_ADC_DRY - raw) / (SOIL_ADC_DRY - SOIL_ADC_WET);
  ratio = constrain(ratio, 0.0f, 1.0f);
  float theta = THETA_R + ratio * (THETA_S - THETA_R);
  return constrain((theta - THETA_R) / (THETA_S - THETA_R), 0.0f, 1.0f);
}

// Sudut dari vektor gravitasi BNO055. Tanpa magnetometer: tidak terpengaruh
// logam/antena di sekitar alat dan tidak drift. false = sensor tidak terbaca.
bool readTilt(float& pitch, float& roll) {
  imu::Vector<3> a = bno.getVector(Adafruit_BNO055::VECTOR_ACCELEROMETER);
  if (sqrt(a.x() * a.x() + a.y() * a.y() + a.z() * a.z()) < 0.5f) return false;
  pitch = atan2(-a.x(), sqrt(a.y() * a.y() + a.z() * a.z())) * 180.0f / PI;
  roll  = atan2(a.y(), a.z()) * 180.0f / PI;
  return true;
}

void takeBaseline() {
  Serial.println("Mengkalibrasi baseline sensor (3 detik)...");
  delay(3000);

  const int samples = 20;
  float gSum = 0.0f;
  float pSum = 0.0f, rSum = 0.0f;
  int tiltOk = 0;

  for (int i = 0; i < samples; i++) {
    sensors_event_t ev;
    adxl.getEvent(&ev);
    float mag = sqrt(ev.acceleration.x * ev.acceleration.x +
                     ev.acceleration.y * ev.acceleration.y +
                     ev.acceleration.z * ev.acceleration.z);
    gSum += mag / 9.80665f;

    float p, r;
    if (readTilt(p, r)) { pSum += p; rSum += r; tiltOk++; }
    delay(50);
  }

  if (tiltOk == 0) {
    Serial.println("ERROR: BNO055 tidak mengirim data sudut → restart");
    Serial.flush();
    delay(200);
    ESP.restart();
  }

  gBaseline = gSum / samples;
  pitchBaseline = pSum / tiltOk;
  rollBaseline  = rSum / tiltOk;

  // Filter mulai dari posisi baseline supaya paket pertama tidak melompat.
  pitchFiltered = pitchBaseline;
  rollFiltered  = rollBaseline;
  tiltFilterReady = true;

  Serial.printf("Baseline terkunci: G0=%.3f g | pitch=%.2f roll=%.2f derajat\n",
                gBaseline, pitchBaseline, rollBaseline);
}

SensorData readSensors() {
  SensorData data;

  int soilRaw = analogRead(SOIL_PIN);
  data.saturation = calculateSaturation(soilRaw);

  sensors_event_t adxlEv;
  adxl.getEvent(&adxlEv);
  float mag = sqrt(adxlEv.acceleration.x * adxlEv.acceleration.x +
                   adxlEv.acceleration.y * adxlEv.acceleration.y +
                   adxlEv.acceleration.z * adxlEv.acceleration.z);
  float gNow = mag / 9.80665f;
  data.deltaG = fabs(gNow - gBaseline);
  if (isnan(data.deltaG)) data.deltaG = 0.0f;

  // Baca gagal (I2C putus / sensor reset) → pertahankan nilai filter terakhir,
  // supaya tidak muncul lompatan ke 0° yang terbaca sebagai BAHAYA palsu.
  float pRaw, rRaw;
  if (readTilt(pRaw, rRaw)) {
    if (!tiltFilterReady) {
      pitchFiltered = pRaw;
      rollFiltered  = rRaw;
      tiltFilterReady = true;
    } else {
      pitchFiltered += TILT_EMA_ALPHA * (pRaw - pitchFiltered);
      rollFiltered  += TILT_EMA_ALPHA * (rRaw - rollFiltered);
    }
  }

  float dp = pitchFiltered - pitchBaseline;
  float dr = rollFiltered - rollBaseline;
  data.deltaTheta = sqrt(dp * dp + dr * dr);
  if (isnan(data.deltaTheta)) data.deltaTheta = 0.0f;

  // Sensor baru menyala: orientasi BNO055 belum stabil, jangan menilai dulu.
  if (millis() < tiltWarmupUntilMs) {
    data.deltaTheta = 0.0f;
  }

  if (data.deltaTheta > DELTA_THETA_THRESHOLD) {
    data.status = "BAHAYA";
  } else if (data.deltaG > DELTA_G_THRESHOLD) {
    data.status = "WASPADA";
  } else if (data.saturation > SOIL_THRESHOLD) {
    data.status = "HATI-HATI";
  } else {
    data.status = "AMAN";
  }

  return data;
}

#if BATTERY_MONITOR
float readBatteryVolts() {
  uint32_t total = 0;
  for (int i = 0; i < 8; i++) {
    total += analogReadMilliVolts(BATTERY_PIN);
    delay(2);
  }
  return (total / 8.0f) / 1000.0f * BATTERY_RATIO;
}
#endif

bool sendRealtimePacket(const SensorData& d) {
  char pkt[112];
#if BATTERY_MONITOR
  snprintf(pkt, sizeof(pkt), "SAT=%.3f,DG=%.3f,DTH=%.2f,VBAT=%.2f,STATUS=%s",
           d.saturation, d.deltaG, d.deltaTheta, readBatteryVolts(), d.status);
#else
  snprintf(pkt, sizeof(pkt), "SAT=%.3f,DG=%.3f,DTH=%.2f,STATUS=%s",
           d.saturation, d.deltaG, d.deltaTheta, d.status);
#endif

  LoRa.idle();
  if (!LoRa.beginPacket()) {
    Serial.println("❌ LoRa beginPacket gagal");
    return false;
  }
  LoRa.print(pkt);
  int ok = LoRa.endPacket();

  Serial.printf("📤 TX [SF7 %dB]: %s → %s\n",
                (int)strlen(pkt), pkt, ok == 1 ? "OK" : "GAGAL");
  return ok == 1;
}

// =====================================================
// SETUP & LOOP
// =====================================================
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1200UL) delay(10);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   LADET TX v4.0 — REALTIME (SF7, NO-SEQ, WDT 15s)");
  Serial.println("==================================================");
  Serial.printf("Reset terakhir  : %s\n", resetReasonName());

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);
#if BATTERY_MONITOR
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
#endif

  bootRequire("ADXL345", []() { return adxl.begin(); }, 5);
  adxl.setRange(ADXL345_RANGE_16_G);
  Serial.println("ADXL345         : OK");

  bootRequire("BNO055", []() { return bno.begin(); }, 5);
  delay(800);
  bno.setExtCrystalUse(true);
  Serial.println("BNO055          : OK");

  if (!initLoRaRadio()) {
    Serial.println("❌ LoRa SX1278 tidak merespons → restart");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  Serial.println("LoRa SX1278     : OK (433 MHz, SF7, 4 dBm)");

  takeBaseline();
  tiltWarmupUntilMs = millis() + TILT_WARMUP_MS;
  Serial.printf("Pemanasan kemiringan %lu detik (DTH diabaikan sementara)\n",
                TILT_WARMUP_MS / 1000);
#if BATTERY_MONITOR
  Serial.printf("Baterai         : %.2f V (pembagi %.1fx)\n",
                readBatteryVolts(), BATTERY_RATIO);
#endif

  setupWatchdog();
  Serial.println("▶️  Streaming realtime dimulai...");
}

void loop() {
  if (wdtArmed) esp_task_wdt_reset();

  ensureRadioHealthy();

  SensorData d = readSensors();
  if (!sendRealtimePacket(d)) {
    initLoRaRadio();
  }

  unsigned long waitStart = millis();
  while (millis() - waitStart < PACKET_INTERVAL) {
    if (wdtArmed) esp_task_wdt_reset();
    delay(50);
  }
}
