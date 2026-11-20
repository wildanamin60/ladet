/**
 * LADET TX - Sensor Transmitter (v3.0 FINAL)
 *
 * Firmware asli LADET yang sudah teruji.
 * Satu tambahan: MS=millis() untuk delay_ms measurement di RX.
 *
 * Hardware: ESP32 + LoRa SX1278 + ADXL345 + BNO055 + Soil
 * (DS3231/RTC TIDAK dipakai — waktu absolut diisi RX, lihat RX firmware)
 * Wiring LoRa : SCK=18, MISO=19, MOSI=25, SS=5, RST=14, DIO0=4
 * Wiring I2C  : SDA=21, SCL=22
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <math.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_BNO055.h>
#include <Preferences.h>   // simpan SF aktif di flash (NVS) — tahan reset
#include <esp_system.h>    // esp_reset_reason() — penyebab reboot terakhir

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
// LORA CONFIGURATION
// =====================================================

#define LORA_FREQUENCY    433E6
#define LORA_BW           125E3
#define LORA_CR           5
// ⚠️ JANGAN NAIKKAN ke 14 dBm: pada hardware LADET ini lonjakan arus TX
// menyebabkan BROWNOUT (regulator AMS1117 tidak kuat) → TX reboot saat
// mengirim paket. 4 dBm terbukti stabil. Nilai ini HARUS sama di semua
// skenario S1–S12 (catat di metodologi).
#define LORA_TX_POWER     4
#define LORA_SF           7

// =====================================================
// RESEARCH CONFIGURATION
// =====================================================

// TOTAL_PACKETS = 50 paket DATA (#1..#50). Setiap run juga mengirim 1 paket
// KALIBRASI #0 lebih dulu (total 51 transmisi) — RX memakai #0 hanya untuk
// mengunci kalibrasi delay, tidak dipublish/dihitung sebagai data.
#define TOTAL_PACKETS     50
#define PACKET_INTERVAL   2000UL

// =====================================================
// SENSOR THRESHOLD
// =====================================================

#define SOIL_THRESHOLD          0.60
#define DELTA_G_THRESHOLD       0.50
#define DELTA_THETA_THRESHOLD   5.0

// =====================================================
// SOIL CALIBRATION
// =====================================================

float SOIL_ADC_DRY = 3000.0;
float SOIL_ADC_WET = 500.0;
float THETA_R = 0.05;
float THETA_S = 0.45;

// =====================================================
// OBJECT
// =====================================================

Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified(12345);
Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28);

// =====================================================
// BASELINE
// =====================================================

float gBaseline = 0.0;
float thetaXBaseline = 0.0;
float thetaYBaseline = 0.0;
float thetaZBaseline = 0.0;
imu::Quaternion qBaseline;

// =====================================================
// PACKET CONTROL
// =====================================================

// ── DUA MODE PENGIRIMAN ──
//   MONITOR (default, untuk WARGA/pengabdian): TX mengirim paket rutin tiap
//     MONITOR_INTERVAL_MS (default 2000 ms, bisa diubah dari web). Paket ini
//     TIDAK di-ACK, TIDAK masuk tracking loss skripsi — murni pantauan realtime.
//   BURST (untuk SKRIPSI/engineer): perintah RUN dari web → TX mengirim
//     #0 kalibrasi + #1..#50 dengan ACK & tracking loss seperti sebelumnya.
//   Selama dashboard engineer TIDAK dibuka, TX selalu dalam mode MONITOR.
uint16_t packetSequence = 0;   // #0 = kalibrasi (tidak dihitung), data rill = #1..#50
bool waitingAck = false;
unsigned long ackStartMicros = 0;
float lastRTT = -1.0;
int activeSF = LORA_SF;   // SF radio aktif — bisa diganti via config remote dari web
Preferences prefs;        // penyimpanan NVS untuk SF aktif (tahan reset)
bool runRequested = false; // perintah RUN dari web (via RX) — run dimulai saat ditekan
bool engineerMode = false;  // true = hentikan auto-loop sampai RUN/MONITOR
bool monitorEnabled = true;
bool runActive = true;
bool controlCommandInterrupted = false; // command mode diterima saat TX menunggu/menjeda
// ── Interval MONITOR (ms) — bisa diubah dari web (panel Skenario / Pengaturan).
// Default 2000 ms = sama seperti interval burst skripsi.
unsigned long monitorIntervalMs = 2000UL;
unsigned long lastMonitorMs = 0;   // millis() paket MONITOR terakhir
uint16_t monitorSeq = 0;           // nomor urut MONITOR (independen dari SEQ burst)

// =====================================================
// SENSOR DATA STRUCTURE
// =====================================================

struct SensorData {
  int soilRaw;
  int soilmV;
  float thetaMeasured;
  float thetaR;
  float thetaS;
  float saturation;
  float gInitial;
  float gMeasured;
  float deltaG;
  float adxlX;
  float adxlY;
  float adxlZ;
  float thetaX;
  float thetaY;
  float thetaZ;
  float deltaThetaX;
  float deltaThetaY;
  float deltaThetaZ;
  float deltaTheta;
  float bnoAccelX;
  float bnoAccelY;
  float bnoAccelZ;
  String status;
};

// =====================================================
// NORMALIZE ANGLE
// =====================================================

float normalizeAngle(float angle) {
  while (angle > 180.0) angle -= 360.0;
  while (angle < -180.0) angle += 360.0;
  return angle;
}

// =====================================================
// ANGLE DIFFERENCE
// =====================================================

float angleDifference(float current, float baseline) {
  return normalizeAngle(current - baseline);
}

// =====================================================
// SOIL THETA
// =====================================================

float calculateTheta(int raw) {
  float ratio = (SOIL_ADC_DRY - raw) / (SOIL_ADC_DRY - SOIL_ADC_WET);
  ratio = constrain(ratio, 0.0, 1.0);
  float theta = THETA_R + ratio * (THETA_S - THETA_R);
  return theta;
}

// =====================================================
// SOIL SATURATION
// =====================================================

float calculateSaturation(float theta) {
  if (THETA_S <= THETA_R) return 0.0;
  float S = (theta - THETA_R) / (THETA_S - THETA_R);
  return constrain(S, 0.0, 1.0);
}

// =====================================================
// READ SENSOR
// =====================================================

SensorData readSensors() {
  SensorData data;

  // SOIL
  data.soilRaw = analogRead(SOIL_PIN);
  data.soilmV = analogReadMilliVolts(SOIL_PIN);
  data.thetaMeasured = calculateTheta(data.soilRaw);
  data.thetaR = THETA_R;
  data.thetaS = THETA_S;
  data.saturation = calculateSaturation(data.thetaMeasured);

  // ADXL345
  sensors_event_t adxlEvent;
  adxl.getEvent(&adxlEvent);
  data.adxlX = adxlEvent.acceleration.x;
  data.adxlY = adxlEvent.acceleration.y;
  data.adxlZ = adxlEvent.acceleration.z;

  float magnitude = sqrt(
    data.adxlX * data.adxlX +
    data.adxlY * data.adxlY +
    data.adxlZ * data.adxlZ
  );
  data.gMeasured = magnitude / 9.80665;
  data.gInitial = gBaseline;
  data.deltaG = fabs(data.gMeasured - data.gInitial);

  // BNO055 EULER
  sensors_event_t orientation;
  bno.getEvent(&orientation, Adafruit_BNO055::VECTOR_EULER);
  data.thetaX = orientation.orientation.x;
  data.thetaY = orientation.orientation.y;
  data.thetaZ = orientation.orientation.z;

  data.deltaThetaX = angleDifference(data.thetaX, thetaXBaseline);
  data.deltaThetaY = angleDifference(data.thetaY, thetaYBaseline);
  data.deltaThetaZ = angleDifference(data.thetaZ, thetaZBaseline);

  // BNO ACCELEROMETER
  imu::Vector<3> bnoAccel = bno.getVector(Adafruit_BNO055::VECTOR_ACCELEROMETER);
  data.bnoAccelX = bnoAccel.x();
  data.bnoAccelY = bnoAccel.y();
  data.bnoAccelZ = bnoAccel.z();

  // TOTAL ORIENTATION CHANGE (QUATERNION)
  imu::Quaternion qCurrent = bno.getQuat();
  float dot = qBaseline.w() * qCurrent.w() +
              qBaseline.x() * qCurrent.x() +
              qBaseline.y() * qCurrent.y() +
              qBaseline.z() * qCurrent.z();
  dot = fabs(dot);
  dot = constrain(dot, 0.0, 1.0);
  data.deltaTheta = 2.0 * acos(dot) * 180.0 / PI;

  // STATUS
  bool soilWarning = data.saturation > SOIL_THRESHOLD;
  bool vibrationWarning = data.deltaG > DELTA_G_THRESHOLD;
  bool tiltWarning = data.deltaTheta > DELTA_THETA_THRESHOLD;

  if (tiltWarning) {
    data.status = "BAHAYA";
  } else if (vibrationWarning) {
    data.status = "WASPADA";
  } else if (soilWarning) {
    data.status = "HATI-HATI";
  } else {
    data.status = "AMAN";
  }

  return data;
}

// =====================================================
// PRINT SENSOR DATA
// =====================================================

void printSensorData(SensorData data) {
  Serial.println();
  Serial.println("==================================================");
  Serial.println("              DATA SENSOR LADET");
  Serial.println("==================================================");

  Serial.println();
  Serial.println("[SOIL MOISTURE]");
  Serial.print("ADC Raw           : "); Serial.println(data.soilRaw);
  Serial.print("Voltage           : "); Serial.print(data.soilmV); Serial.println(" mV");
  Serial.print("Theta terukur     : "); Serial.print(data.thetaMeasured, 4); Serial.println(" m3/m3");
  Serial.print("Theta r           : "); Serial.println(data.thetaR, 4);
  Serial.print("Theta s           : "); Serial.println(data.thetaS, 4);
  Serial.print("Saturasi S        : "); Serial.print(data.saturation * 100.0, 2); Serial.println(" %");

  Serial.println();
  Serial.println("[ADXL345]");
  Serial.print("G awal            : "); Serial.print(data.gInitial, 4); Serial.println(" g");
  Serial.print("G terukur         : "); Serial.print(data.gMeasured, 4); Serial.println(" g");
  Serial.print("Delta G           : "); Serial.print(data.deltaG, 4); Serial.println(" g");
  Serial.print("Accel X           : "); Serial.print(data.adxlX, 3); Serial.println(" m/s2");
  Serial.print("Accel Y           : "); Serial.print(data.adxlY, 3); Serial.println(" m/s2");
  Serial.print("Accel Z           : "); Serial.print(data.adxlZ, 3); Serial.println(" m/s2");

  Serial.println();
  Serial.println("[BNO055]");
  Serial.print("Theta X           : "); Serial.print(data.thetaX, 2); Serial.println(" deg");
  Serial.print("Theta Y           : "); Serial.print(data.thetaY, 2); Serial.println(" deg");
  Serial.print("Theta Z           : "); Serial.print(data.thetaZ, 2); Serial.println(" deg");
  Serial.print("Delta Theta X     : "); Serial.print(data.deltaThetaX, 2); Serial.println(" deg");
  Serial.print("Delta Theta Y     : "); Serial.print(data.deltaThetaY, 2); Serial.println(" deg");
  Serial.print("Delta Theta Z     : "); Serial.print(data.deltaThetaZ, 2); Serial.println(" deg");
  Serial.print("Delta Theta Total : "); Serial.print(data.deltaTheta, 2); Serial.println(" deg");

  Serial.println();
  Serial.print("STATUS            : "); Serial.println(data.status);
  Serial.println("==================================================");
}

// =====================================================
// BUILD PACKET
// Satu-satunya tambahan dari firmware asli:
// field MS=millis() untuk delay_ms measurement di RX
// =====================================================

String buildPacket(SensorData data) {
  String packet;
  packet.reserve(260);

  // PENTING: payload LoRa MAKSIMAL 255 byte (batas radio SX127x).
  // buildPacket WAJIB menjaga total < 255 byte dalam kondisi terburuk
  // (SEQ 2 digit + field RTT + STATUS terpanjang + MS 10 digit).
  // Karena itu: tanpa SOIL_RAW/SOIL_MV (tidak dipakai dashboard/CSV),
  // presisi desimal dipangkas ke yang bermakna, dan THETAM tidak dikirim
  // (nilainya IDENTIK dengan TZ — RX memakai TZ sebagai sudut terukur).
  packet += "TYPE=DATA";
  packet += ",SEQ=";          packet += packetSequence;
  // Tidak ada field TIME: TX tidak punya RTC. Waktu absolut diisi RX (NTP saat
  // online, uptime saat offline) — menghemat ~24 byte airtime LoRa.
  packet += ",THETA=";        packet += String(data.thetaMeasured, 3);
  packet += ",THETAR=";       packet += String(data.thetaR, 3);
  packet += ",THETAS=";       packet += String(data.thetaS, 3);
  packet += ",SAT=";          packet += String(data.saturation, 3);
  packet += ",G0=";           packet += String(data.gInitial, 3);
  packet += ",G=";            packet += String(data.gMeasured, 3);
  packet += ",DG=";           packet += String(data.deltaG, 3);
  packet += ",TX=";           packet += String(data.thetaX, 1);
  packet += ",TY=";           packet += String(data.thetaY, 1);
  packet += ",TZ=";           packet += String(data.thetaZ, 1);
  packet += ",DTX=";          packet += String(data.deltaThetaX, 1);
  packet += ",DTY=";          packet += String(data.deltaThetaY, 1);
  packet += ",DTZ=";          packet += String(data.deltaThetaZ, 1);
  packet += ",DTH=";          packet += String(data.deltaTheta, 1);
  // Sudut acuan (baseline) — sudut terukur = TZ (THETAM dihilangkan: duplikat).
  packet += ",THETA0=";       packet += String(thetaZBaseline, 1);

  // === SATU-SATUNYA TAMBAHAN: MS timestamp ===
  packet += ",MS=";           packet += millis();

  // RTT dari paket sebelumnya (diukur di TX via micros(), akurat 1 jam)
  // Packet 1: belum ada RTT. Packet N: memuat RTT paket N-1.
  if (lastRTT >= 0) {
    packet += ",RTT=";        packet += String(lastRTT, 2);
  }

  packet += ",STATUS=";       packet += data.status;
  packet += ",SF=";           packet += activeSF;

  return packet;
}

// =====================================================
// SEND DATA
// =====================================================

bool sendData(SensorData data) {
  String packet = buildPacket(data);

  Serial.println();
  Serial.println("---------------- LORA TX ----------------");
  Serial.print("SEQ      : "); Serial.println(packetSequence);
  Serial.print("SF       : "); Serial.println(activeSF);
  Serial.print("Payload size : "); Serial.print(packet.length()); Serial.println(" byte");
  Serial.print("Payload  : "); Serial.println(packet);

  if (packet.length() > 255) {
    Serial.println("ERROR: Payload > 255 byte!");
    return false;
  }

  LoRa.idle();

  if (!LoRa.beginPacket()) {
    Serial.println("ERROR: LoRa beginPacket gagal");
    return false;
  }

  LoRa.print(packet);
  int result = LoRa.endPacket();

  if (result == 1) {
    Serial.println("TX STATUS: BERHASIL");
    return true;
  } else {
    Serial.println("TX STATUS: GAGAL");
    return false;
  }
}

// =====================================================
// CHECK ACK
// =====================================================

// Forward declarations: command handling is shared by the ACK-wait path and
// the normal idle/config path below.
int extractInt(const String& s, const String& key);
void applyConfig(int newSF);

// Proses command kontrol yang bisa tiba kapan saja, termasuk saat TX sedang
// menunggu ACK paket data. Sebelumnya checkAck() membaca paket TYPE=RUN/
// TYPE=ENGINEER sebagai "response" biasa lalu membuangnya, sehingga RX terus
// retry walaupun TX sebenarnya sedang menerima command.
void handleConfigMessage(const String& incoming) {
  if (!incoming.startsWith("TYPE=CONF") && !incoming.startsWith("TYPE=RUN") &&
      !incoming.startsWith("TYPE=ENGINEER") && !incoming.startsWith("TYPE=MONITOR")) return;

  if (incoming.startsWith("TYPE=RUN") || incoming.startsWith("TYPE=ENGINEER") || incoming.startsWith("TYPE=MONITOR")) {
    controlCommandInterrupted = true;
    int comma = incoming.indexOf(',');
    String command = comma >= 0 ? incoming.substring(5, comma) : incoming.substring(5);
    LoRa.idle();
    LoRa.beginPacket();
    LoRa.print("ACK=" + command);
    LoRa.endPacket();
    LoRa.receive();
    if (command == "ENGINEER") {
      engineerMode = true; monitorEnabled = false; runActive = false;
      packetSequence = TOTAL_PACKETS + 1;
      Serial.println("🔒 MODE ENGINEER — auto-loop TX dihentikan.");
    } else if (command == "RUN") {
      engineerMode = true; monitorEnabled = false; runActive = true;
      packetSequence = 0; lastRTT = -1.0; runRequested = true;
      Serial.println("▶️ RUN Engineer diterima — sequence di-reset ke #0, data dimulai #1.");
    } else if (command == "MONITOR") {
      engineerMode = false; monitorEnabled = true; runActive = true;
      packetSequence = 0; lastRTT = -1.0; runRequested = false;
      Serial.println("📡 MONITOR diterima — auto-loop warga diaktifkan.");
    }
    return;
  }

  // ACK CONF dikirim di SF lama sebelum TX menerapkan konfigurasi baru.
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print("ACK=CONF");
  LoRa.endPacket();
  LoRa.receive();
  applyConfig(extractInt(incoming, "SF="));
}

void checkAck() {
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;

  String incoming;
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }

  if (incoming.startsWith("TYPE=")) {
    handleConfigMessage(incoming);
    // Command kontrol sudah dijawab. Jangan menunggu ACK data lama yang
    // mungkin memang tidak akan datang karena TX berpindah mode.
    waitingAck = false;
    return;
  }

  Serial.print("RX RESPONSE       : ");
  Serial.println(incoming);

  if (incoming.startsWith("ACK=")) {
    int ackSeq = incoming.substring(4).toInt();
    if (ackSeq == packetSequence) {
      unsigned long elapsed = micros() - ackStartMicros;
      lastRTT = elapsed / 1000.0;
      waitingAck = false;
      Serial.print("RTT ACK           : ");
      Serial.print(lastRTT, 3);
      Serial.println(" ms");
    }
  }

  LoRa.receive();
}

// =====================================================
// CONFIG REMOTE (RX -> TX via LoRa)
// Web -> MQTT -> RX, lalu RX meneruskan "TYPE=CONF,SF=n"
// ke TX MASIH di SF lama. TX membalas "ACK=CONF" di SF
// lama, baru menerapkan SF baru. Urutan ini menjamin TX &
// RX tidak pernah berada di SF yang berbeda.
// =====================================================

int extractInt(const String& s, const String& key) {
  int idx = s.indexOf(key);
  if (idx < 0) return -1;
  return s.substring(idx + key.length()).toInt();
}

void applyConfig(int newSF) {
  if (newSF != 7 && newSF != 9 && newSF != 12) return;
  int oldSF = activeSF;
  // SF 7/9/12 selalu valid untuk SX1278 (guard di atas), jadi setSpreadingFactor
  // tidak mungkin gagal. Catatan: di LoRa lib 0.8.0 fungsi ini return void.
  LoRa.setSpreadingFactor(newSF);
  activeSF = newSF;
  // Simpan ke NVS: TX yang di-reset tetap di SF ini (tetap sinkron dgn RX)
  prefs.begin("ladet", false);
  prefs.putUChar("sf", (unsigned char)newSF);
  prefs.end();
  Serial.printf("⚙️ CONFIG: SF %d -> %d TER-APPLY ke radio (ACK CONF terkirim di SF %d)\n", oldSF, newSF, oldSF);
}

// Dipanggil terus-menerus saat TX idle (boot window & setelah run selesai)
void checkConfig() {
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;

  String incoming;
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }
  Serial.print("📡 CONFIG diterima: ");
  Serial.println(incoming);

  handleConfigMessage(incoming);
}

// Heartbeat Serial + beacon radio: membuktikan TX hidup & mendengarkan,
// sekaligus memberitahu RX SF aktifnya (RX bisa menyelaraskan diri otomatis
// tanpa menunggu perintah web).
void listenBeat(const char* ctx) {
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat >= 10000UL) {
    lastBeat = millis();
    Serial.printf("⏳ %s — TX MENUNGGU, SF aktif: %d, uptime: %lu dtk\n",
                  ctx, activeSF, millis() / 1000);
  }
}

void txBeacon() {
  static unsigned long lastBeacon = 0;
  if (millis() - lastBeacon >= 12000UL) {
    lastBeacon = millis();
    LoRa.idle();
    LoRa.beginPacket();
    LoRa.print("TYPE=BEACON,SF=" + String(activeSF));
    LoRa.endPacket();
    LoRa.receive();
    Serial.printf("📻 BEACON terkirim (SF %d)\n", activeSF);
  }
}

// Jendela waktu mendengarkan config. durationMs = 0 → tanpa batas waktu.
// stopOnRun = true → keluar begitu perintah CMD=RUN diterima.
void listenForConfig(unsigned long durationMs, bool stopOnRun = false) {
  if (durationMs > 0) {
    Serial.printf("⏳ Jendela config %lu detik — mendengarkan TYPE=CONF (SF aktif: %d)...\n",
                  durationMs / 1000, activeSF);
  }
  LoRa.receive();
  unsigned long start = millis();
  while (true) {
    if (durationMs > 0 && millis() - start >= durationMs) break;
    if (stopOnRun && runRequested) break;
    checkConfig();
    listenBeat("idle-config");
    txBeacon();
    delay(5);
  }
}

// =====================================================
// TAKE BASELINE
// =====================================================

void takeBaseline() {
  Serial.println();
  Serial.println("==================================================");
  Serial.println("              BASELINE SENSOR");
  Serial.println("Letakkan node pada posisi referensi.");
  Serial.println("Baseline akan diambil setelah sensor stabil.");
  Serial.println("==================================================");

  delay(3000);

  const int samples = 20;

  // ADXL BASELINE
  float gSum = 0.0;
  for (int i = 0; i < samples; i++) {
    sensors_event_t event;
    adxl.getEvent(&event);
    float g = sqrt(
      event.acceleration.x * event.acceleration.x +
      event.acceleration.y * event.acceleration.y +
      event.acceleration.z * event.acceleration.z
    ) / 9.80665;
    gSum += g;
    delay(50);
  }
  gBaseline = gSum / samples;

  // BNO BASELINE
  float xSum = 0.0, ySum = 0.0, zSum = 0.0;
  float qwSum = 0.0, qxSum = 0.0, qySum = 0.0, qzSum = 0.0;

  for (int i = 0; i < samples; i++) {
    sensors_event_t orientation;
    bno.getEvent(&orientation, Adafruit_BNO055::VECTOR_EULER);
    imu::Quaternion q = bno.getQuat();

    xSum += orientation.orientation.x;
    ySum += orientation.orientation.y;
    zSum += orientation.orientation.z;
    qwSum += q.w();
    qxSum += q.x();
    qySum += q.y();
    qzSum += q.z();
    delay(50);
  }

  thetaXBaseline = xSum / samples;
  thetaYBaseline = ySum / samples;
  thetaZBaseline = zSum / samples;

  float qw = qwSum / samples;
  float qx = qxSum / samples;
  float qy = qySum / samples;
  float qz = qzSum / samples;
  float qNorm = sqrt(qw * qw + qx * qx + qy * qy + qz * qz);

  if (qNorm > 0.0) {
    qw /= qNorm;
    qx /= qNorm;
    qy /= qNorm;
    qz /= qNorm;
  }

  qBaseline = imu::Quaternion(qw, qx, qy, qz);

  // PRINT BASELINE
  Serial.println();
  Serial.println("BASELINE TERKUNCI:");
  Serial.print("G awal        : "); Serial.print(gBaseline, 4); Serial.println(" g");
  Serial.print("Theta X awal  : "); Serial.print(thetaXBaseline, 2); Serial.println(" deg");
  Serial.print("Theta Y awal  : "); Serial.print(thetaYBaseline, 2); Serial.println(" deg");
  Serial.print("Theta Z awal  : "); Serial.print(thetaZBaseline, 2); Serial.println(" deg");
  Serial.println();
}

// =====================================================
// PEMULIHAN DIRI SAAT BOOT
// =====================================================
// Sebelumnya tiap inisialisasi yang gagal (ADXL/BNO/LoRa) berakhir di
// `while (true) { delay(1000); }` — alat diam total sampai listriknya dicabut
// manual. Delay di dalam loop itu justru MEMBERI MAKAN watchdog, jadi WDT
// bawaan ESP32 pun tidak bisa menyelamatkan.
//
// Sekarang: coba beberapa kali (kabel longgar / sensor telat naik tegangan
// sering lolos pada percobaan ke-2), lalu restart sendiri. Kalau sensornya
// memang rusak permanen, alat akan reboot berulang — dan itu TERLIHAT di
// Serial & dashboard, bukan diam tanpa jejak.

/** Nama penyebab reset terakhir — untuk log di Serial Monitor. */
const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON (colokan/listrik)";
    case ESP_RST_EXT:      return "EXT (tombol reset)";
    case ESP_RST_SW:       return "SW (ESP.restart)";
    case ESP_RST_PANIC:    return "PANIC (crash / watchdog)";
    case ESP_RST_INT_WDT:  return "INT_WDT (interrupt macet)";
    case ESP_RST_TASK_WDT: return "TASK_WDT (loop membeku)";
    case ESP_RST_WDT:      return "WDT (watchdog)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (tegangan drop)";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    default:               return "UNKNOWN";
  }
}

/**
 * Jalankan `probe` sampai berhasil, atau restart ESP32 kalau habis percobaan.
 * Sengaja TANPA default argument: preprocessor Arduino menyalin signature
 * fungsi ke prototype, dan default argumen ganda membuat compile gagal.
 */
void bootRequire(const char* name, bool (*probe)(), int tries) {
  for (int i = 1; i <= tries; i++) {
    if (probe()) return;
    Serial.printf("   %s gagal (percobaan %d/%d)\n", name, i, tries);
    delay(500);
  }
  Serial.printf("ERROR: %s tidak merespons setelah %d percobaan\n", name, tries);
  Serial.println("       Restart otomatis — cek kabel I2C/SPI & catu daya.");
  Serial.flush();
  delay(300);
  ESP.restart();
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println();
  Serial.println("==================================================");
  Serial.println("       LADET TRANSMITTER v3.0");
  Serial.println("==================================================");
  Serial.print("Reset terakhir  : ");
  Serial.println(resetReasonName());

  // I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);
  Serial.println("I2C             : OK");

  // ADC
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);
  Serial.println("ADC SOIL        : OK");

  // RTC: TIDAK DIPAKAI. TX tidak punya jam dinding — waktu absolut diisi RX.
  // (Komponen DS3231 boleh dilepas; firmware tidak lagi menuntutnya saat boot.)

  // ADXL345
  bootRequire("ADXL345", []() { return adxl.begin(); }, 5);
  adxl.setRange(ADXL345_RANGE_16_G);
  Serial.println("ADXL345         : OK");

  // BNO055
  bootRequire("BNO055", []() { return bno.begin(); }, 5);
  delay(1000);
  bno.setExtCrystalUse(true);
  Serial.println("BNO055          : OK");

  // LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  bootRequire("LoRa SX1278", []() { return LoRa.begin(LORA_FREQUENCY) != 0; }, 5);

  LoRa.setSpreadingFactor(activeSF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.enableCrc();
  Serial.println("LoRa            : OK");

  // Pulihkan SF terakhir dari NVS: supaya setelah reset TX tetap sinkron dengan
  // RX yang sudah dikonfigurasi lewat web — tidak balik ke default #define.
  prefs.begin("ladet", true);
  int savedSF = prefs.getUChar("sf", 0);
  prefs.end();
  if (savedSF == 7 || savedSF == 9 || savedSF == 12) {
    activeSF = savedSF;
    LoRa.setSpreadingFactor(activeSF);
    Serial.printf("SF tersimpan    : %d (dipulihkan dari NVS)\n", activeSF);
  }

  Serial.println();
  Serial.println("LORA CONFIGURATION");
  Serial.println("Frequency       : 433 MHz");
  Serial.print("SF              : "); Serial.println(activeSF);
  Serial.println("BW              : 125 kHz");
  Serial.println("CR              : 4/5");
  Serial.print("TX Power        : "); Serial.print(LORA_TX_POWER); Serial.println(" dBm");
  Serial.println("CRC             : ON");

  // BASELINE
  takeBaseline();

  Serial.println("==================================================");
  Serial.println("       TX SIAP MELAKUKAN PENGUJIAN");
  Serial.println("==================================================");
  Serial.print("Total paket : "); Serial.println(TOTAL_PACKETS);
  Serial.print("Interval    : "); Serial.print(PACKET_INTERVAL); Serial.println(" ms");
  Serial.println();

  // ── MODE OTOMATIS ──
  // Setelah baseline selesai, TX langsung mengirim paket #0 sampai #50.
  // Tidak perlu menekan tombol RUN dari dashboard Engineer agar mode warga
  // selalu mendapat data realtime setelah node dinyalakan.
  runRequested = false;
  engineerMode = false;
  monitorEnabled = true;
  runActive = true;
  packetSequence = 0;
  lastRTT = -1.0;
  LoRa.receive();
  Serial.println("▶️ MODE OTOMATIS AKTIF — mulai mengirim data sekarang.");
  Serial.println("   TX akan mengulang siklus #0..#50 tanpa menunggu RUN.");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  if (engineerMode && !runActive) {
    LoRa.receive();
    checkConfig();
    listenBeat("engineer-standby");
    txBeacon();
    delay(5);
    return;
  }
  if (packetSequence <= TOTAL_PACKETS) {
    // PAKET #0 = KALIBRASI: dikirim paling awal tiap run. #0 TETAP menunggu
    // ACK — RTT-nya disertakan di paket #1 agar RX mengunci offset ABSOLUT
    // sejak paket pertama (delay #1 rill, bukan 0). RX tidak mempublish #0.
    bool kalibrasiPacket = (packetSequence == 0);

    // BACA SENSOR
    SensorData data = readSensors();
    printSensorData(data);

    if (kalibrasiPacket) {
      Serial.println();
      Serial.println("[PAKET #0 = KALIBRASI — di-ACK (RTT utk #1); RX tidak mempublish]");
    }

    // KIRIM DATA
    bool txSuccess = sendData(data);

    // ACK / RTT — #0 JUGA menunggu ACK! RTT#0 yang terukur di sini disertakan
    // TX di paket #1 (field RTT=), sehingga RX mengunci offset ABSOLUT sejak
    // paket pertama → delay #1 = rill, bukan 0 ms.
    if (txSuccess) {
      // #0 = paket kunci kalibrasi: kalau ACK-nya hilang, TX kirim ulang #0
      // (maks 3 percobaan) supaya paket #1 hampir selalu membawa RTT.
      int maxAttempts = kalibrasiPacket ? 3 : 1;

      for (int attempt = 0; attempt < maxAttempts; attempt++) {
        if (attempt > 0) {
          Serial.println("ACK #0 belum masuk → kirim ulang paket kalibrasi...");
          sendData(data);          // SEQ tetap 0
        }

        waitingAck = true;
        controlCommandInterrupted = false;
        ackStartMicros = micros();

        LoRa.receive();

        unsigned long ackStartMillis = millis();
        while (waitingAck && (millis() - ackStartMillis < 1000UL)) {
          checkAck();
          delay(1);
        }

        if (waitingAck) {
          Serial.println("ACK               : TIMEOUT");
          waitingAck = false;
        } else {
          break;                   // ACK diterima → lanjut paket berikutnya
        }

      }
    }

    // Jangan lanjut menaikkan packetSequence setelah command RUN mengubah
    // sequence ke #0, atau command ENGINEER/MONITOR memindahkan mode TX.
    if (controlCommandInterrupted) return;

    // SEQUENCE BERIKUTNYA
    packetSequence++;

    // INTERVAL
    if (packetSequence <= TOTAL_PACKETS) {
      Serial.println();
      Serial.print("Menunggu ");
      Serial.print(PACKET_INTERVAL);
      Serial.println(" ms...");
      // Jangan tidur utuh selama interval: command ENGINEER/MONITOR harus
      // diterima segera walaupun TX sedang berada di antara dua paket.
      LoRa.receive();
      unsigned long waitStart = millis();
      while (millis() - waitStart < PACKET_INTERVAL) {
        checkConfig();
        if (engineerMode && !runActive) return;
        if (controlCommandInterrupted) return;
        delay(5);
      }
    }

  } else {
    if (engineerMode) {
      // Satu RUN Engineer selesai; tetap diam sampai MONITOR atau RUN baru.
      runActive = false;
      Serial.println("✅ RUN Engineer selesai — TX diam menunggu MONITOR/RUN.");
      LoRa.receive();
      while (engineerMode && !runActive) {
        checkConfig();
        txBeacon();
        delay(5);
      }
      return;
    }
    // MONITOR — lanjut otomatis ke siklus berikutnya.
    // Engineer tetap dapat mengubah SF melalui konfigurasi RX; perubahan itu
    // akan diproses di sela-sela paket berikutnya tanpa menghentikan monitoring.
    Serial.println();
    Serial.println("==================================================");
    Serial.println("       SIKLUS 50 PAKET SELESAI");
    Serial.println("==================================================");
    Serial.println("TX melanjutkan pengiriman otomatis...");

    // Beri waktu singkat untuk memproses config/beacon dan memberi jarak antar
    // siklus, kemudian mulai lagi dari paket kalibrasi #0.
    LoRa.receive();
    unsigned long cycleStart = millis();
    while (millis() - cycleStart < 1000UL) {
      checkConfig();
      txBeacon();
      delay(5);
    }

    packetSequence = 0;     // siklus baru: #0 kalibrasi → #1..#50
    lastRTT = -1.0;
    runRequested = false;
    Serial.println("▶️ Siklus otomatis baru dimulai (baseline lama dipakai).");
  }
}
