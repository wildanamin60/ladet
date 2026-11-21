/**
 * LADET — TES POLARITAS RELAY (diagnosa "semua aktuator nyala saat boot")
 *
 * Sketch ini TIDAK memakai LoRa/WiFi. Upload ke board RX (atau board mana pun
 * yang terpasang modul relay), buka Serial Monitor 115200, lalu perhatikan
 * relay/LED/buzzer:
 *
 *   Fase A: semua pin = LOW  (5 detik)
 *   Fase B: semua pin = HIGH (5 detik)
 *   ...berulang
 *
 * Cara membaca hasil:
 *   - Relay MATI saat fase A (LOW)  → modul AKTIF HIGH
 *         → di firmware utama set  #define RELAY_ACTIVE_LOW 0
 *   - Relay MATI saat fase B (HIGH) → modul AKTIF LOW
 *         → di firmware utama set  #define RELAY_ACTIVE_LOW 1
 *   - Relay NYALA TERUS di kedua fase → bukan masalah polaritas, tapi LEVEL
 *     TEGANGAN: modul 5 V tidak bisa dimatikan oleh sinyal 3.3 V ESP32.
 *         → perbaikan hardware:
 *           1. Lepas jumper VCC–JD-VCC pada modul.
 *           2. VCC (sisi logika/opto)  → 3.3 V ESP32
 *           3. JD-VCC (kumparan)       → 5 V adaptor
 *           4. GND logika tetap tersambung ke GND ESP32
 *
 * Perintah lewat Serial Monitor (akhiri dengan Enter):
 *   h = tahan semua HIGH
 *   l = tahan semua LOW
 *   p = kembali ke mode bergantian otomatis
 *   k = matikan semua (HIGH untuk modul aktif LOW, LOW untuk aktif HIGH)
 */

#define PIN_RED     25
#define PIN_YELLOW  26
#define PIN_GREEN   27
#define PIN_BUZZER  33

const int PINS[] = { PIN_RED, PIN_YELLOW, PIN_GREEN, PIN_BUZZER };
const int PIN_COUNT = sizeof(PINS) / sizeof(PINS[0]);

char mode = 'p';                    // p = ping-pong, h = HIGH, l = LOW
unsigned long lastFlip = 0;
bool phaseHigh = false;

void writeAll(int level) {
  for (int i = 0; i < PIN_COUNT; i++) digitalWrite(PINS[i], level);
}

void setup() {
  for (int i = 0; i < PIN_COUNT; i++) {
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);     // mulai dari LOW, aman untuk modul apa pun
  }
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== TES POLARITAS RELAY LADET ===");
  Serial.println("Pin: merah=25 kuning=26 hijau=27 buzzer=33");
  Serial.println();
  Serial.println("Amati kapan relay/LED/buzzer MATI:");
  Serial.println("  mati saat LOW  -> modul AKTIF HIGH  (RELAY_ACTIVE_LOW 0)");
  Serial.println("  mati saat HIGH -> modul AKTIF LOW   (RELAY_ACTIVE_LOW 1)");
  Serial.println("  nyala terus    -> level 3.3V kurang; pakai VCC logika 3.3V + JD-VCC 5V");
  Serial.println();
  Serial.println("Perintah: h=HIGH  l=LOW  p=bergantian  k=mati");
  Serial.println();
  lastFlip = millis();
}

void loop() {
  // Baca perintah Serial
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r' || c == ' ') continue;
    if (c == 'h' || c == 'H') { mode = 'h'; writeAll(HIGH); Serial.println(">> Semua HIGH"); }
    else if (c == 'l' || c == 'L') { mode = 'l'; writeAll(LOW); Serial.println(">> Semua LOW"); }
    else if (c == 'p' || c == 'P') { mode = 'p'; lastFlip = millis(); Serial.println(">> Mode bergantian"); }
    else if (c == 'k' || c == 'K') {
      mode = 'h';
      writeAll(HIGH);
      Serial.println(">> Semua OFF (asumsi modul aktif LOW: HIGH=off)");
      Serial.println("   Kalau masih nyala, kirim 'l' (berarti modul aktif HIGH).");
    }
  }

  if (mode == 'p' && millis() - lastFlip >= 5000UL) {
    lastFlip = millis();
    phaseHigh = !phaseHigh;
    writeAll(phaseHigh ? HIGH : LOW);
    Serial.printf(">> Sekarang semua %s (5 detik)\n", phaseHigh ? "HIGH" : "LOW");
  }
}
