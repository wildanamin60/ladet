/*
 * LADET — Kalibrasi sensor TX (mode menu, satu per satu)
 *
 * Tanpa LoRa/WiFi supaya pembacaan sensor bersih dari lonjakan arus radio.
 * Serial Monitor 115200, line ending: Newline.
 *
 * Semua angka dicetak hanya saat kamu minta, tidak ada output berulang.
 */

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_BNO055.h>

#define SDA_PIN   21
#define SCL_PIN   22
#define SOIL_PIN  34

Adafruit_ADXL345_Unified adxl(12345);
Adafruit_BNO055 bno(55, 0x28);

float gBaseline = 1.0f;
float pitchBaseline = 0.0f;
float rollBaseline = 0.0f;
imu::Quaternion qBaseline;

int soilDry = 0;
int soilWet = 0;

String readLine() {
  while (!Serial.available()) delay(10);
  String s = Serial.readStringUntil('\n');
  s.trim();
  return s;
}

int readSoilRaw() {
  long sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogRead(SOIL_PIN);
    delay(12);
  }
  return (int)(sum / 16);
}

float readG() {
  sensors_event_t ev;
  adxl.getEvent(&ev);
  float mag = sqrt(ev.acceleration.x * ev.acceleration.x +
                   ev.acceleration.y * ev.acceleration.y +
                   ev.acceleration.z * ev.acceleration.z);
  return mag / 9.80665f;
}

void readAccelG(float &ax, float &ay, float &az) {
  imu::Vector<3> a = bno.getVector(Adafruit_BNO055::VECTOR_ACCELEROMETER);
  ax = a.x(); ay = a.y(); az = a.z();
}

// Sudut dari vektor gravitasi: tidak memakai magnetometer, jadi tidak drift.
float pitchDeg() {
  float ax, ay, az;
  readAccelG(ax, ay, az);
  return atan2(-ax, sqrt(ay * ay + az * az)) * 180.0f / PI;
}

float rollDeg() {
  float ax, ay, az;
  readAccelG(ax, ay, az);
  return atan2(ay, az) * 180.0f / PI;
}

float angleBetween(imu::Quaternion a, imu::Quaternion b) {
  float dot = fabs(a.w() * b.w() + a.x() * b.x() + a.y() * b.y() + a.z() * b.z());
  dot = constrain(dot, 0.0f, 1.0f);
  return 2.0f * acos(dot) * 180.0f / PI;
}

void printMainMenu() {
  Serial.println();
  Serial.println("=== KALIBRASI SENSOR LADET ===");
  Serial.println("1. Soil moisture (kelembapan tanah)");
  Serial.println("2. Getaran (ADXL345)");
  Serial.println("3. Kemiringan (BNO055)");
  Serial.println("Ketik angka lalu Enter:");
}

void soilMenu() {
  while (true) {
    Serial.println();
    Serial.println("--- SOIL MOISTURE ---");
    Serial.println("1. Ukur kondisi KERING (probe di udara)");
    Serial.println("2. Ukur kondisi BASAH (probe terendam air)");
    Serial.println("3. Tampilkan hasil kalibrasi");
    Serial.println("0. Kembali ke menu utama");

    String c = readLine();

    if (c == "1") {
      Serial.println("Mengukur... jangan sentuh probe.");
      delay(800);
      soilDry = readSoilRaw();
      Serial.printf("ADC kering = %d\n", soilDry);
    } else if (c == "2") {
      Serial.println("Mengukur... pastikan probe terendam.");
      delay(800);
      soilWet = readSoilRaw();
      Serial.printf("ADC basah = %d\n", soilWet);
    } else if (c == "3") {
      if (soilDry == 0 || soilWet == 0) {
        Serial.println("Belum lengkap: ukur kering dan basah dulu.");
      } else if (soilDry <= soilWet) {
        Serial.printf("Tidak wajar: kering (%d) harus LEBIH BESAR dari basah (%d).\n", soilDry, soilWet);
        Serial.println("Cek probe atau catu daya, lalu ukur ulang.");
      } else {
        Serial.println();
        Serial.println("Salin ke ladet-tx-realtime.ino:");
        Serial.printf("#define SOIL_ADC_DRY  %.1ff\n", (float)soilDry);
        Serial.printf("#define SOIL_ADC_WET  %.1ff\n", (float)soilWet);
      }
    } else if (c == "0") {
      return;
    } else {
      Serial.println("Pilihan tidak dikenal.");
    }
  }
}

void vibrationMenu() {
  while (true) {
    Serial.println();
    Serial.println("--- GETARAN (ADXL345) ---");
    Serial.println("1. Ambil baseline (node DIAM 3 detik)");
    Serial.println("2. Ukur getaran (jeda 3 detik, rekam puncak 5 detik)");
    Serial.println("0. Kembali ke menu utama");

    String c = readLine();

    if (c == "1") {
      Serial.println("Jangan sentuh node. Mengambil 20 sampel...");
      delay(3000);
      float sum = 0;
      for (int i = 0; i < 20; i++) {
        sum += readG();
        delay(50);
      }
      gBaseline = sum / 20.0f;
      Serial.printf("Baseline g = %.4f\n", gBaseline);
      Serial.println("Kalau angka ini bukan sekitar 1.0000, ulangi baseline.");
    } else if (c == "2") {
      Serial.println("Bersiap, jangan sentuh dulu...");
      for (int i = 3; i >= 1; i--) {
        Serial.printf("%d...\n", i);
        delay(1000);
      }
      Serial.println("MULAI - beri getaran sekarang (5 detik)");

      float peak = 0.0f;
      int over = 0;
      int samples = 0;
      unsigned long t0 = millis();
      while (millis() - t0 < 5000UL) {
        float dg = fabs(readG() - gBaseline);
        if (dg > peak) peak = dg;
        if (dg > 0.50f) over++;
        samples++;
        delay(20);
      }

      Serial.printf("Selesai: %d sampel dalam 5 detik.\n", samples);
      Serial.printf("deltaG puncak      = %.4f\n", peak);
      Serial.printf("Sampel > 0.50      = %d dari %d\n", over, samples);
      Serial.println(peak > 0.50f ? "-> Ambang WASPADA (0.50) TERCAPAI" : "-> Ambang WASPADA (0.50) belum tercapai");
    } else if (c == "0") {
      return;
    } else {
      Serial.println("Pilihan tidak dikenal.");
    }
  }
}

void showBnoCalibration() {
  uint8_t sys, gyro, accel, mag;
  bno.getCalibration(&sys, &gyro, &accel, &mag);
  Serial.printf("Status kalibrasi BNO055: sys=%d gyro=%d accel=%d mag=%d\n", sys, gyro, accel, mag);
  Serial.println("Skala 0-3, makin tinggi makin baik. Target: semua 3.");
  if (mag < 3) {
    Serial.println("mag rendah = kemiringan arah atas-bawah (yaw) akan melenceng.");
  }
}

void bnoCalibrationRoutine() {
  Serial.println();
  Serial.println("Gerakkan node membentuk ANGKA 8 di udara, perlahan, 10-30 detik.");
  Serial.println("Putar ke berbagai arah. Jangan dekat logam/besi/magnet/speaker.");
  Serial.println("Tekan Enter untuk berhenti lebih awal.");
  Serial.println();

  while (!Serial.available()) {
    uint8_t sys, gyro, accel, mag;
    bno.getCalibration(&sys, &gyro, &accel, &mag);
    Serial.printf("sys=%d gyro=%d accel=%d mag=%d\n", sys, gyro, accel, mag);
    if (sys == 3 && gyro == 3 && accel == 3 && mag == 3) {
      Serial.println("SELESAI - semua sudah 3.");
      break;
    }
    delay(1000);
  }
  while (Serial.available()) Serial.read();
  Serial.println("Keluar dari mode kalibrasi BNO055.");
}

void tiltMenu() {
  while (true) {
    Serial.println();
    Serial.println("--- KEMIRINGAN (BNO055) ---");
    Serial.println("1. Cek status kalibrasi BNO055");
    Serial.println("2. Kalibrasi BNO055 (gerakan angka 8)");
    Serial.println("3. Ambil baseline (node DATAR, diam)");
    Serial.println("4. Ukur kemiringan - pitch & roll dari gravitasi");
    Serial.println("5. Ukur kemiringan - quaternion (cara firmware)");
    Serial.println("0. Kembali ke menu utama");

    String c = readLine();

    if (c == "1") {
      showBnoCalibration();
    } else if (c == "2") {
      bnoCalibrationRoutine();
    } else if (c == "3") {
      Serial.println("Jangan sentuh node. Mengambil 20 sampel...");
      delay(3000);

      float pSum = 0, rSum = 0;
      float qw = 0, qx = 0, qy = 0, qz = 0;
      for (int i = 0; i < 20; i++) {
        pSum += pitchDeg();
        rSum += rollDeg();
        imu::Quaternion q = bno.getQuat();
        qw += q.w(); qx += q.x(); qy += q.y(); qz += q.z();
        delay(50);
      }

      pitchBaseline = pSum / 20.0f;
      rollBaseline = rSum / 20.0f;

      qw /= 20.0f; qx /= 20.0f; qy /= 20.0f; qz /= 20.0f;
      float norm = sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
      if (norm > 0) { qw /= norm; qx /= norm; qy /= norm; qz /= norm; }
      qBaseline = imu::Quaternion(qw, qx, qy, qz);

      Serial.printf("Baseline pitch = %.2f  roll = %.2f derajat\n", pitchBaseline, rollBaseline);
      showBnoCalibration();
    } else if (c == "4") {
      float p = 0, r = 0;
      for (int i = 0; i < 10; i++) {
        p += pitchDeg();
        r += rollDeg();
        delay(50);
      }
      p /= 10.0f;
      r /= 10.0f;

      float dp = p - pitchBaseline;
      float dr = r - rollBaseline;
      float total = sqrt(dp * dp + dr * dr);

      Serial.printf("pitch = %7.2f  (selisih %+6.2f)\n", p, dp);
      Serial.printf("roll  = %7.2f  (selisih %+6.2f)\n", r, dr);
      Serial.printf("total kemiringan = %.2f derajat\n", total);
      Serial.println(total > 5.0f ? "-> Melewati ambang BAHAYA (5 derajat)" : "-> Di bawah ambang BAHAYA (5 derajat)");
    } else if (c == "5") {
      float dth = angleBetween(qBaseline, bno.getQuat());
      Serial.printf("deltaTheta quaternion = %.2f derajat\n", dth);
      Serial.println(dth > 5.0f ? "-> Melewati ambang BAHAYA (5 derajat)" : "-> Di bawah ambang BAHAYA (5 derajat)");
      Serial.println("Catatan: angka ini ikut terpengaruh yaw/magnetometer.");
    } else if (c == "0") {
      return;
    } else {
      Serial.println("Pilihan tidak dikenal.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  Serial.println();
  Serial.println("LADET - Kalibrasi sensor");
  Serial.println("Line ending Serial Monitor: Newline");

  if (!adxl.begin()) {
    Serial.println("ERROR: ADXL345 tidak terdeteksi. Cek kabel I2C.");
    while (true) delay(1000);
  }
  adxl.setRange(ADXL345_RANGE_16_G);

  if (!bno.begin()) {
    Serial.println("ERROR: BNO055 tidak terdeteksi. Cek kabel I2C.");
    while (true) delay(1000);
  }
  delay(800);
  bno.setExtCrystalUse(true);

  Serial.println("Sensor siap.");
}

void loop() {
  printMainMenu();
  String c = readLine();

  if (c == "1") {
    soilMenu();
  } else if (c == "2") {
    vibrationMenu();
  } else if (c == "3") {
    tiltMenu();
  } else {
    Serial.println("Pilihan tidak dikenal.");
  }
}
