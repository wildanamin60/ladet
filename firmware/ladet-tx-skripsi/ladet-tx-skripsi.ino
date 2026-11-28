/**
 * LADET TX — mode skripsi (LADET_PROFILE_THESIS = 1)
 *
 * Mengirim data sensor ke RX lewat LoRa 433 MHz. Tidak ada WiFi/MQTT di sini:
 * RX yang menjadi gateway MQTT. Protokol paket mengikuti ladet-rx-skripsi.ino:
 *
 *   TYPE=DATA,SEQ=n,THETA=..,SAT=..,G0=..,G=..,DG=..,TX=..,TY=..,TZ=..,
 *   DTX=..,DTY=..,DTZ=..,DTH=..,THETA0=..,MS=<millis TX>,[RTT=..],STATUS=..,SF=n
 *
 * RX membalas "ACK=<seq>" segera setelah paket diterima. RTT hasil ACK dikirim
 * di paket BERIKUTNYA sebagai RTT= (dipakai RX menghitung delay absolut).
 * SEQ=0 adalah paket kalibrasi: di-ACK RX tetapi tidak dipublish ke MQTT.
 *
 * RX juga bisa mengirim TYPE=CONF,SF=n (dari web via MQTT) dan TX wajib
 * membalas "ACK=CONF" di SF lama sebelum pindah SF.
 *
 * Hardware : ESP32 Dev Module + LoRa SX1278 + ADXL345 + BNO055 + SEN0308
 * Wiring   : LoRa SCK=18 MISO=19 MOSI=25 SS=5 RST=14 DIO0=4
 *            I2C SDA=21 SCL=22 | Soil=34
 *
 * Ganti LORA_SF (7 / 9 / 12) lalu upload ulang untuk tiap skenario pengujian.
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

#define LADET_PROFILE_THESIS 1

#define SDA_PIN       21
#define SCL_PIN       22
#define SOIL_PIN      34

#define LORA_SCK      18
#define LORA_MISO     19
#define LORA_MOSI     25
#define LORA_SS       5
#define LORA_RST      14
#define LORA_DIO0     4

#define LORA_FREQUENCY    433E6
#define LORA_BW           125E3
#define LORA_CR           5
#define LORA_SF           7      // 7 / 9 / 12 sesuai skenario
#define LORA_SYNC_WORD    0x12
#define LORA_TX_POWER     4      // AMS1117 tidak kuat di atas ini (brownout)

#define PACKET_INTERVAL   2000UL // jeda antar paket (SF7). SF9/SF12 otomatis
                                 // lebih lambat karena airtime lebih panjang.
// ACK Window
// ACK Window — jendela tunggu ACK SETELAH paket data selesai dikirim.
// Harus lebih panjang dari airtime ACK ("ACK=<seq>" ≈ 6 byte) pada SF tersebut,
// kalau tidak RTT tidak pernah terukur dan delay kembali RELATIF:
//   SF7  : airtime ACK ±30 ms   → 500 ms cukup
//   SF12 : airtime ACK ±1,1 dtk → 1400 ms terlalu mepet (RX baru mulai mengirim
//          ACK setelah paket data ±7 dtk selesai diterima), jadi dinaikkan.
#define ACK_WINDOW_SF7    500UL
#define ACK_WINDOW_SF12   3200UL
#define WDT_TIMEOUT_MS    25000

// Batas waktu tunggu TxDone setelah mengirim paket data.
// WAJIB: LoRa.endPacket() dari library menunggu flag TxDone TANPA batas waktu
// (LoRa.cpp: `while ((readRegister(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) == 0) yield();`).
// Kalau radio tidak pernah melaporkan TxDone, TX membeku dan watchdog me-reset
// ESP32 di tengah pengujian. 12 dtk cukup: paket ±200 B di SF12 makan ±6,3 dtk.
#define TX_DONE_TIMEOUT_MS 12000UL

#define SOIL_THRESHOLD          0.60f
#define DELTA_G_THRESHOLD       0.50f
#define DELTA_THETA_THRESHOLD   5.0f
#define TILT_EMA_ALPHA          0.15f
#define TILT_WARMUP_MS          5000UL

const float SOIL_ADC_DRY = 2826.0f;
const float SOIL_ADC_WET = 18.0f;
const float THETA_R      = 0.05f;
const float THETA_S      = 0.45f;

Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified(12345);
Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28);

float gBaseline = 1.0f;
float pitchBaseline = 0.0f;
float rollBaseline = 0.0f;
float tiltBaseline = 0.0f;

float pitchFiltered = 0.0f;
float rollFiltered = 0.0f;
float tiltFiltered = 0.0f;
bool  tiltFilterReady = false;
unsigned long tiltWarmupUntilMs = 0;

bool wdtArmed = false;
int  activeSF = LORA_SF;

int  nextSeq = 0;
int  lastSentSeq = -1;
unsigned long lastSentMicros = 0;
float lastRttMs = -1.0f;

struct SensorData {
  float theta;
  float sat;
  float g0;
  float g;
  float dg;
  float tiltX;
  float tiltY;
  float tiltZ;
  float dTiltX;
  float dTiltY;
  float dTiltZ;
  float dTheta;
  float theta0;
  float thetaM;
  const char* status;
};

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
    default:               return "UNKNOWN";
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
  Serial.printf("Watchdog        : %s (%d detik)\n", wdtArmed ? "aktif" : "nonaktif", WDT_TIMEOUT_MS / 1000);
}

void bootRequire(const char* name, bool (*probe)(), int tries) {
  for (int i = 1; i <= tries; i++) {
    if (probe()) return;
    Serial.printf("   %s gagal (%d/%d)\n", name, i, tries);
    delay(400);
  }
  Serial.printf("ERROR: %s tidak merespons -> restart otomatis\n", name);
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

void loraRegWrite(uint8_t reg, uint8_t value) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg | 0x80);
  SPI.transfer(value);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
}

// Register SX127x (nilai sama dengan yang dipakai library LoRa).
#define SX_OP_MODE     0x01
#define SX_IRQ_FLAGS   0x12
#define SX_MODE_TX     0x83   // LongRangeMode | TX
#define SX_IRQ_TX_DONE 0x08

/**
 * Kirim isi FIFO dengan batas waktu — pengganti LoRa.endPacket().
 * Library-nya menunggu TxDone tanpa batas waktu; satu TxDone yang tidak datang
 * membuat loop() membeku dan watchdog me-reset TX di tengah run. Di sini
 * pemanggilan menyerah setelah maxWaitMs sehingga TX bisa memulihkan radio.
 */
bool loraTransmitTimed(unsigned long maxWaitMs) {
  loraRegWrite(SX_IRQ_FLAGS, 0xFF);   // buang flag lama
  loraRegWrite(SX_OP_MODE, SX_MODE_TX);
  unsigned long t0 = millis();
  while ((loraRegRead(SX_IRQ_FLAGS) & SX_IRQ_TX_DONE) == 0) {
    if (millis() - t0 > maxWaitMs) return false;
    if (wdtArmed) esp_task_wdt_reset();
    delay(1);
  }
  loraRegWrite(SX_IRQ_FLAGS, SX_IRQ_TX_DONE);
  return true;
}

bool initLoRaRadio() {
  LoRa.end();
  delay(20);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 1; i <= 3; i++) {
    if (LoRa.begin(LORA_FREQUENCY)) {
      LoRa.setSpreadingFactor(activeSF);
      LoRa.setSignalBandwidth(LORA_BW);
      LoRa.setCodingRate4(LORA_CR);
      LoRa.setTxPower(LORA_TX_POWER);
      LoRa.setSyncWord(LORA_SYNC_WORD);
      LoRa.enableCrc();
      LoRa.receive();
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
    Serial.printf("TX RADIO tidak sehat (VER=0x%02X OP=0x%02X) -> re-init\n", ver, op);
    if (initLoRaRadio()) {
      Serial.printf("   radio pulih (SF%d)\n", activeSF);
    } else {
      Serial.println("radio gagal pulih -> restart ESP32");
      Serial.flush();
      delay(200);
      ESP.restart();
    }
  }
}

float calculateSaturation(int raw) {
  float ratio = (SOIL_ADC_DRY - raw) / (SOIL_ADC_DRY - SOIL_ADC_WET);
  ratio = constrain(ratio, 0.0f, 1.0f);
  float theta = THETA_R + ratio * (THETA_S - THETA_R);
  return constrain((theta - THETA_R) / (THETA_S - THETA_R), 0.0f, 1.0f);
}

// pitch/roll dari vektor gravitasi; fromVertical = sudut total terhadap sumbu Z
// (dipakai sebagai sudut terukur THETAM/TZ).
bool readTilt(float& pitch, float& roll, float& fromVertical) {
  imu::Vector<3> a = bno.getVector(Adafruit_BNO055::VECTOR_ACCELEROMETER);
  float mag = sqrt(a.x() * a.x() + a.y() * a.y() + a.z() * a.z());
  if (mag < 0.5f) return false;

  pitch = atan2(-a.x(), sqrt(a.y() * a.y() + a.z() * a.z())) * 180.0f / PI;
  roll  = atan2(a.y(), a.z()) * 180.0f / PI;

  float cosT = constrain(a.z() / mag, -1.0f, 1.0f);
  fromVertical = acos(cosT) * 180.0f / PI;
  return true;
}

void takeBaseline() {
  Serial.println("Kalibrasi baseline sensor (3 detik)...");
  delay(3000);

  const int samples = 20;
  float gSum = 0.0f, pSum = 0.0f, rSum = 0.0f, tSum = 0.0f;
  int tiltOk = 0;

  for (int i = 0; i < samples; i++) {
    sensors_event_t ev;
    adxl.getEvent(&ev);
    float mag = sqrt(ev.acceleration.x * ev.acceleration.x +
                     ev.acceleration.y * ev.acceleration.y +
                     ev.acceleration.z * ev.acceleration.z);
    gSum += mag / 9.80665f;

    float p, r, t;
    if (readTilt(p, r, t)) { pSum += p; rSum += r; tSum += t; tiltOk++; }
    delay(50);
  }

  if (tiltOk == 0) {
    Serial.println("ERROR: BNO055 tidak mengirim data sudut -> restart");
    Serial.flush();
    delay(200);
    ESP.restart();
  }

  gBaseline     = gSum / samples;
  pitchBaseline = pSum / tiltOk;
  rollBaseline  = rSum / tiltOk;
  tiltBaseline  = tSum / tiltOk;

  pitchFiltered   = pitchBaseline;
  rollFiltered    = rollBaseline;
  tiltFiltered    = tiltBaseline;
  tiltFilterReady = true;

  Serial.printf("Baseline        : G0=%.3f g | pitch=%.2f roll=%.2f sudut=%.2f\n",
                gBaseline, pitchBaseline, rollBaseline, tiltBaseline);
}

SensorData readSensors() {
  SensorData d;

  int soilRaw = analogRead(SOIL_PIN);
  d.sat   = calculateSaturation(soilRaw);
  d.theta = THETA_R + d.sat * (THETA_S - THETA_R);

  sensors_event_t ev;
  adxl.getEvent(&ev);
  float mag = sqrt(ev.acceleration.x * ev.acceleration.x +
                   ev.acceleration.y * ev.acceleration.y +
                   ev.acceleration.z * ev.acceleration.z);
  float gNow = mag / 9.80665f;
  d.g  = gNow;
  d.g0 = gBaseline;
  d.dg = fabs(gNow - gBaseline);
  if (isnan(d.dg)) d.dg = 0.0f;

  float pRaw, rRaw, tRaw;
  if (readTilt(pRaw, rRaw, tRaw)) {
    if (!tiltFilterReady) {
      pitchFiltered = pRaw;
      rollFiltered  = rRaw;
      tiltFiltered  = tRaw;
      tiltFilterReady = true;
    } else {
      pitchFiltered += TILT_EMA_ALPHA * (pRaw - pitchFiltered);
      rollFiltered  += TILT_EMA_ALPHA * (rRaw - rollFiltered);
      tiltFiltered  += TILT_EMA_ALPHA * (tRaw - tiltFiltered);
    }
  }

  d.tiltX = pitchFiltered;
  d.tiltY = rollFiltered;
  d.tiltZ = tiltFiltered;

  d.dTiltX = pitchFiltered - pitchBaseline;
  d.dTiltY = rollFiltered - rollBaseline;
  d.dTiltZ = tiltFiltered - tiltBaseline;
  d.dTheta = sqrt(d.dTiltX * d.dTiltX + d.dTiltY * d.dTiltY);
  if (isnan(d.dTheta)) d.dTheta = 0.0f;

  if (millis() < tiltWarmupUntilMs) d.dTheta = 0.0f;

  d.theta0 = tiltBaseline;
  d.thetaM = tiltFiltered;

  if (d.dTheta > DELTA_THETA_THRESHOLD) {
    d.status = "BAHAYA";
  } else if (d.dg > DELTA_G_THRESHOLD) {
    d.status = "WASPADA";
  } else if (d.sat > SOIL_THRESHOLD) {
    d.status = "HATI-HATI";
  } else {
    d.status = "AMAN";
  }

  return d;
}

unsigned long ackWindowMs() {
  return (activeSF == 12) ? ACK_WINDOW_SF12 : ACK_WINDOW_SF7;
}

void handleIncoming(String in) {
  in.trim();
  if (in.length() == 0) return;

  if (in.startsWith("ACK=")) {
    int seq = in.substring(4).toInt();
    if (seq == lastSentSeq && lastSentMicros > 0) {
      lastRttMs = (micros() - lastSentMicros) / 1000.0f;
    }
    return;
  }
}

void radioListen(unsigned long windowMs) {
  LoRa.receive();
  unsigned long start = millis();
  while (millis() - start < windowMs) {
    if (wdtArmed) esp_task_wdt_reset();
    int size = LoRa.parsePacket();
    if (size > 0) {
      String in = "";
      while (LoRa.available()) in += (char)LoRa.read();
      handleIncoming(in);
    }
    delay(5);
  }
}

bool sendPacket(const SensorData& d, int packetSeq) {
  char pkt[240];

  int len = snprintf(pkt, sizeof(pkt),
    "TYPE=DATA,SEQ=%d,THETA=%.3f,THETAR=%.3f,THETAS=%.3f,SAT=%.3f,"
    "G0=%.3f,G=%.3f,DG=%.3f,"
    "TX=%.2f,TY=%.2f,TZ=%.2f,DTX=%.2f,DTY=%.2f,DTZ=%.2f,DTH=%.2f,"
    "THETA0=%.2f,MS=%lu,",
    packetSeq, d.theta, THETA_R, THETA_S, d.sat,
    d.g0, d.g, d.dg,
    d.tiltX, d.tiltY, d.tiltZ,
    d.dTiltX, d.dTiltY, d.dTiltZ, d.dTheta,
    d.theta0, millis());

  if (len < 0 || len >= (int)sizeof(pkt)) {
    Serial.println("payload terpotong, paket tidak dikirim");
    return false;
  }

  if (lastRttMs >= 0.0f) {
    len += snprintf(pkt + len, sizeof(pkt) - len, "RTT=%.1f,", lastRttMs);
  }
  len += snprintf(pkt + len, sizeof(pkt) - len, "STATUS=%s,SF=%d", d.status, activeSF);

  if (len < 0 || len >= (int)sizeof(pkt)) {
    Serial.println("payload terpotong, paket tidak dikirim");
    return false;
  }

  LoRa.idle();
  if (!LoRa.beginPacket()) {
    Serial.println("LoRa beginPacket gagal");
    return false;
  }
  LoRa.print(pkt);

  lastSentSeq    = packetSeq;
  lastSentMicros = micros();
  // BUKAN LoRa.endPacket(): fungsi library itu menunggu TxDone TANPA batas
  // waktu, jadi satu TxDone yang tidak pernah datang = TX membeku lalu di-reset
  // watchdog (ledakan paket hilang di CSV).
  bool ok = loraTransmitTimed(TX_DONE_TIMEOUT_MS);

  Serial.printf("TX SEQ=%d SF%d %dB | sat=%.1f%% dg=%.3f dth=%.2f %s -> %s\n",
                packetSeq, activeSF, len, d.sat * 100.0f, d.dg, d.dTheta, d.status,
                ok ? "OK" : "GAGAL/TIMEOUT");
  return ok;
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1200UL) delay(10);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   LADET TX - SKRIPSI (LoRa -> RX gateway MQTT)");
  Serial.println("==================================================");
  Serial.printf("Reset terakhir  : %s\n", resetReasonName());
  Serial.printf("SF awal         : %d\n", activeSF);

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  bootRequire("ADXL345", []() { return adxl.begin(); }, 5);
  adxl.setRange(ADXL345_RANGE_16_G);
  Serial.println("ADXL345         : OK");

  bootRequire("BNO055", []() { return bno.begin(); }, 5);
  delay(800);
  bno.setExtCrystalUse(true);
  Serial.println("BNO055          : OK");

  if (!initLoRaRadio()) {
    Serial.println("LoRa SX1278 tidak merespons -> restart");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  Serial.printf("LoRa SX1278     : OK (433 MHz, SF%d, %d dBm)\n", activeSF, LORA_TX_POWER);

  takeBaseline();
  tiltWarmupUntilMs = millis() + TILT_WARMUP_MS;

  setupWatchdog();
  Serial.println("TX THESIS siap - SEQ=0 (kalibrasi), lalu SEQ=1,2,3,...");
}

void loop() {
  unsigned long start = millis();
  if (wdtArmed) esp_task_wdt_reset();

  ensureRadioHealthy();

  SensorData d = readSensors();
  if (!sendPacket(d, nextSeq)) initLoRaRadio();

  nextSeq = (nextSeq >= 60000) ? 1 : nextSeq + 1;

  radioListen(ackWindowMs());

  while (millis() - start < PACKET_INTERVAL) {
    radioListen(50);
  }
}
