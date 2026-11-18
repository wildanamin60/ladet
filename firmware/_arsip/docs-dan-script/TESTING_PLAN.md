# LADET Hardware Testing - Sequential Test Plan

## Testing Philosophy

**Incremental validation**: Setiap layer diverifikasi secara terpisah sebelum menambah kompleksitas.

**Traceability**: Setiap packet harus bisa dibuktikan ujung-ke-ujung (TX → RX → MQTT → Website).

**Data integrity**: Nilai yang tampil di dashboard harus identik dengan data TX (dengan transformasi yang terdokumentasi).

---

## Test Sequence Overview

```
TEST 0: RX → MQTT → Website (no LoRa, synthetic data)
         ↓ validates WiFi/MQTT/normalizer
TEST 1: TX → LoRa → RX (monitor Serial only)
         ↓ validates LoRa link + parser
TEST 2: TX → LoRa → RX → MQTT → Website (end-to-end)
         ↓ validates full pipeline
TEST 3: Distance 100m (baseline)
         ↓ establishes reference RSSI/SNR
TEST 4: Distance 300m (medium range)
         ↓ tests signal degradation
TEST 5: Distance 700m (long range)
         ↓ tests extreme conditions
```

After TEST 5 success → **Research data collection phase** (50 packets/scenario).

---

## TEST 0 - MQTT Connectivity (No LoRa)

**Status**: Ready to run
**Documentation**: `rx-mqtt-publisher/TEST_0_MQTT_ONLY.md`
**Duration**: 10-15 minutes
**Hardware**: ESP32 RX only

**Goal**: Verify RX can publish to MQTT and website receives data correctly.

**Success criteria**:
- ✅ WiFi connected
- ✅ MQTT connected
- ✅ JSON publishes every 5s
- ✅ Website displays values correctly
- ✅ Units show "Δg" (not "Hz")

**Next**: If pass → TEST 1

---

## TEST 1 - LoRa Link Validation (TX → RX, Serial monitoring)

**Hardware**: ESP32 TX + ESP32 RX (1-5 meters apart)
**Duration**: 15-20 minutes

See `firmware/TEST_PLAN_DETAILS.md` for complete checklist.

**Key validation**: All 21 TX fields parsed correctly, transformations accurate.

**Next**: If pass → TEST 2

---

## TEST 2 - End-to-End Pipeline

**Goal**: Trace ONE packet from TX Serial → RX Serial → Website Dashboard

**Success**: Values match across all three systems (within rounding).

**Next**: If pass → TEST 3 (distance testing)

---

## TEST 3-5 - Distance Testing (100m / 300m / 700m)

**Data collection**: 50 packets per scenario

**Metrics**:
- RSSI (dBm)
- SNR (dB)
- Packet loss (%)

**Research output**: RSSI vs Distance, SNR vs Distance, Link Budget analysis

---

## Emergency Stop Conditions

**Stop immediately if**:
- Sensor readings invalid (NaN, negative)
- MQTT connection unstable
- Hardware overheating
- Weather unsafe

---

**See `firmware/TEST_PLAN_DETAILS.md` for full procedures and checklists.**
