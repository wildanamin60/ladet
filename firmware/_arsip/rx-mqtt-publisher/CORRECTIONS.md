# RX Firmware - Koreksi Konfigurasi LoRa

**Date**: 2026-09-15
**Status**: ✅ FIXED

## Masalah yang Ditemukan

### 1. ❌ DIO0 Pin Salah (CRITICAL)
**Sebelum**:
```cpp
const int LORA_DIO0 = 2;  // SALAH!
```

**Sesudah**:
```cpp
#define LORA_DIO0 = 4  // GPIO4 sesuai wiring LADET
```

**Impact**: Tanpa ini, RX tidak akan menerima interrupt dari LoRa module.

---

### 2. ⚠️ SPI Pins Tidak Eksplisit
**Sebelum**:
```cpp
LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
// Mengandalkan default SPI
```

**Sesudah**:
```cpp
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23

SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
```

**Rationale**: Dokumentasi eksplisit, hindari ambiguitas.

---

### 3. ❌ SF Hardcoded ke 9 (CRITICAL)
**Sebelum**:
```cpp
LoRa.setSpreadingFactor(9);  // Dipaksa SF9
```

**Sesudah**:
```cpp
#define LORA_SF = 7  // Configurable: 7, 9, atau 12
LoRa.setSpreadingFactor(LORA_SF);
```

**Impact**: SF RX HARUS sama dengan TX. Tidak ada "auto-adjust".

---

### 4. ❌ CRC Tidak Diaktifkan
**Sebelum**:
```cpp
// Tidak ada LoRa.enableCrc()
```

**Sesudah**:
```cpp
LoRa.enableCrc();  // Sama dengan TX
```

---

### 5. ❌ Frekuensi Salah
**Sebelum**:
```cpp
#define LORA_FREQ = 915E6  // 915 MHz (AS)
```

**Sesudah**:
```cpp
#define LORA_FREQ = 433E6  // 433 MHz (Indonesia)
```

---

## Konfigurasi Final (Benar)

```cpp
// LoRa Pins - LADET Wiring
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0   4   // GPIO4!

// LoRa Parameters
#define LORA_FREQ  433E6  // 433 MHz
#define LORA_SF     7     // 7, 9, atau 12 (HARUS sama TX)

void setupLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("❌ LoRa init gagal!");
    while (1);
  }
  
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();  // PENTING!
}
```

---

## Checklist Konfigurasi

✅ DIO0 = GPIO4 (bukan GPIO2)
✅ SPI eksplisit (18/19/23)
✅ SF configurable (bukan hardcoded 9)
✅ CRC enabled
✅ Frekuensi 433 MHz
✅ BW 125 kHz
✅ CR 4/5
✅ Sync word 0x12

---

## Testing Impact

### TEST 0 (MQTT only)
**Status**: ✅ Tidak terpengaruh
**Reason**: `MQTT_TEST_MODE` tidak menggunakan LoRa

### TEST 1 (TX → LoRa → RX)
**Status**: ⚠️ WAJIB gunakan firmware yang sudah diperbaiki
**Reason**: DIO0 salah = tidak ada interrupt = tidak menerima packet

### TEST 2-5
**Status**: ⚠️ WAJIB gunakan firmware yang sudah diperbaiki
**Reason**: SF harus match, CRC harus enabled

---

## Action Items

1. ✅ Firmware RX sudah diperbaiki
2. 🔄 TEST 0 boleh jalan dengan firmware baru (MQTT tidak terpengaruh)
3. ⏸️ TEST 1 menunggu firmware ini di-upload ke RX
4. 📝 README updated dengan konfigurasi benar

---

**Prepared by**: Kiro AI
**Reviewed by**: User (hardware verification)
**Status**: Ready for TEST 0 → TEST 1
