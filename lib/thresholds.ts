/**
 * LADET — Ambang batas sensor
 * Nilai ini dapat diubah sesuai kalibrasi di lapangan.
 * Referensi: Tabel 2.2 Skripsi LADET
 */

export interface MetricThresholds {
  /** Ambang masuk level HATI-HATI (🟡) */
  hatiHati: number;
  /** Ambang masuk level WASPADA (🟠) */
  waspada: number;
  /** Ambang masuk level BAHAYA (🔴) */
  bahaya: number;
}

export interface Thresholds {
  soil_saturation: MetricThresholds; // %  — SEN0308, semakin tinggi makin bahaya
  vibration_freq:  MetricThresholds; // Δg — ADXL345 (delta getaran, bukan Hz)
  tilt_angle:      MetricThresholds; // °  — BNO055,  semakin tinggi makin bahaya
}

/** Nilai default ambang batas — sesuai Tabel 2.2 skripsi & firmware TX */
export const DEFAULT_THRESHOLDS: Thresholds = {
  soil_saturation: { hatiHati: 60,  waspada: 75, bahaya: 90 },  // %
  vibration_freq:  { hatiHati: 0.5, waspada: 1.0, bahaya: 2.0 }, // Δg
  tilt_angle:      { hatiHati: 15,  waspada: 30, bahaya: 45 },   // °
};

/** Urutan level dari paling ringan ke paling berat — dipakai untuk membandingkan level */
export const LEVEL_ORDER = ['aman', 'hati-hati', 'waspada', 'bahaya'] as const;

const LS_KEY = 'ladet_thresholds';

/** Ambil thresholds dari localStorage (dengan fallback ke default) */
export function loadThresholds(): Thresholds {
  if (typeof window === 'undefined') return DEFAULT_THRESHOLDS;
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (!raw) return DEFAULT_THRESHOLDS;
    return { ...DEFAULT_THRESHOLDS, ...JSON.parse(raw) };
  } catch {
    return DEFAULT_THRESHOLDS;
  }
}

/** Simpan thresholds ke localStorage */
export function saveThresholds(t: Thresholds): void {
  if (typeof window === 'undefined') return;
  localStorage.setItem(LS_KEY, JSON.stringify(t));
}

/**
 * Hitung status level dari nilai sensor
 * Level 3 (BAHAYA) dominan — cek dari level tertinggi dulu
 */
/**
 * Hitung status level dari nilai sensor.
 * Untuk tiap metrik, nilai yang lebih besar = lebih berbahaya.
 * Level akhir = level tertinggi di antara ketiga metrik.
 */
export function computeStatusLevel(
  soil_saturation: number,
  vibration_freq: number,
  tilt_angle: number,
  thresholds: Thresholds = DEFAULT_THRESHOLDS
): 'aman' | 'hati-hati' | 'waspada' | 'bahaya' {
  let idx = 0;
  const check = (val: number, t: MetricThresholds) => {
    if (val >= t.bahaya)   idx = Math.max(idx, 3);
    if (val >= t.waspada)  idx = Math.max(idx, 2);
    if (val >= t.hatiHati) idx = Math.max(idx, 1);
  };
  check(soil_saturation, thresholds.soil_saturation);
  check(vibration_freq,  thresholds.vibration_freq);
  check(tilt_angle,      thresholds.tilt_angle);
  return LEVEL_ORDER[idx];
}

/** Konversi string level ke angka untuk UI */
export function levelToNumber(level: 'aman' | 'hati-hati' | 'waspada' | 'bahaya'): 0 | 1 | 2 | 3 {
  const map = { aman: 0, 'hati-hati': 1, waspada: 2, bahaya: 3 } as const;
  return map[level];
}

/**
 * ️ JANGAN DIPAKAI LAGI UNTUK STATUS LIVE (halaman utama & dashboard).
 *
 * Firmware TX sudah punya TUPOKSI per sensor (tanah → HATI-HATI, getaran →
 * WASPADA, kemiringan → BAHAYA) dan mengirim status final lewat field
 * `status` di paket LoRa → MQTT. Status itu satu-satunya sumber kebenaran.
 *
 * Fungsi ini dulu menaikkan status TX memakai ambang Tabel 2.2 (3 tingkat),
 * sehingga sensor tanah 100 % (yang di firmware hanya HATI-HATI) tampil
 * BAHAYA — keluar dari tupoksi sensor. Sekarang fungsi ini hanya disimpan
 * untuk analisis offline / pembanding, bukan untuk tampilan real-time.
 */
export function resolveLevel(
  firmwareLevel: string | undefined,
  soil_saturation: number,
  vibration_freq: number,
  tilt_angle: number,
  thresholds: Thresholds = DEFAULT_THRESHOLDS
): 'aman' | 'hati-hati' | 'waspada' | 'bahaya' {
  const fwKey = (firmwareLevel ?? '').toLowerCase();
  const fwIdx = (LEVEL_ORDER as readonly string[]).indexOf(fwKey);
  const calcIdx = levelToNumber(
    computeStatusLevel(soil_saturation, vibration_freq, tilt_angle, thresholds)
  );
  const finalIdx = Math.max(fwIdx < 0 ? 0 : fwIdx, calcIdx);
  return LEVEL_ORDER[finalIdx];
}

/* ------------------------------------------------------------------ */
/*  TUPOKSI SENSOR — ambang BINER firmware TX (sumber kebenaran)       */
/* ------------------------------------------------------------------ */

/**
 * Level status LADET.
 */
export type Level = 'aman' | 'hati-hati' | 'waspada' | 'bahaya';

/**
 * Salinan ambang milik firmware TX (firmware/ladet-tx/ladet-tx.ino):
 *
 *   #define SOIL_THRESHOLD          0.60   → 60 % saturasi
 *   #define DELTA_G_THRESHOLD       0.50   → 0.50 Δg
 *   #define DELTA_THETA_THRESHOLD   5.0    → 5.0 ° perubahan sudut
 *
 * Firmware TX memetakan tiap sensor ke status MILIKNYA SENDIRI (tupoksi):
 *
 *   kelembapan tanah (SEN0308) → HATI-HATI
 *   getaran (ADXL345)          → WASPADA
 *   kemiringan (BNO055)        → BAHAYA
 *
 * Status global = yang PALING BERAT di antara ketiganya — itulah string
 * `STATUS=` yang ikut di paket LoRa dan harus ditampilkan apa adanya.
 * Kalau ambang di firmware TX diubah, ubah angka di sini juga.
 */
export const FIRMWARE_THRESHOLDS = {
  soilSaturationPct: 60,    // SOIL_THRESHOLD 0.60 (dipakai sebagai %)
  deltaG:            0.50,  // DELTA_G_THRESHOLD (Δg)
  deltaThetaDeg:     5.0,   // DELTA_THETA_THRESHOLD (°)
} as const;

/** Status milik sensor kelembapan tanah saja — tupoksi: HATI-HATI. */
export function soilLevel(saturationPct: number): Level {
  return saturationPct > FIRMWARE_THRESHOLDS.soilSaturationPct ? 'hati-hati' : 'aman';
}

/** Status milik sensor getaran saja — tupoksi: WASPADA. */
export function vibrationLevel(deltaG: number): Level {
  return deltaG > FIRMWARE_THRESHOLDS.deltaG ? 'waspada' : 'aman';
}

/** Status milik sensor kemiringan saja — tupoksi: BAHAYA. */
export function tiltLevel(deltaThetaDeg: number): Level {
  return deltaThetaDeg > FIRMWARE_THRESHOLDS.deltaThetaDeg ? 'bahaya' : 'aman';
}

/**
 * Normalisasi string status dari firmware (bisa "HATI-HATI", "hati hati",
 * "BAHAYA") menjadi Level yang dikenal UI. String yang tidak dikenal
 * (mis. payload kosong) diperlakukan sebagai 'aman' supaya UI tidak rusak.
 */
export function normalizeLevel(status: string | undefined | null): Level {
  // Firmware may send labels in uppercase and use spaces, underscores, or
  // repeated separators (for example "HATI_HATI" or "HATI HATI").
  const s = (status ?? '')
    .trim()
    .toLowerCase()
    .replace(/[\s_]+/g, '-')
    .replace(/-+/g, '-');
  return (LEVEL_ORDER as readonly string[]).includes(s) ? (s as Level) : 'aman';
}

/* ------------------------------------------------------------------ */
/*  KALIBRASI LOKAL — memakai ambang dari halaman Pengaturan           */
/* ------------------------------------------------------------------ */

/** Indeks level (0=aman … 3=bahaya) dari satu nilai sensor + ambangnya. */
function thresholdIndex(value: number, t: MetricThresholds): number {
  if (!Number.isFinite(value)) return 0;
  if (value >= t.bahaya)   return 3;
  if (value >= t.waspada)  return 2;
  if (value >= t.hatiHati) return 1;
  return 0;
}

/**
 * Terapkan ambang kalibrasi (disimpan dari halaman Pengaturan) ke level
 * yang dikirim firmware TX.
 *
 * Aturan mainnya:
 *  - Hanya bisa MENAIKKAN level, tidak pernah menurunkan. Status dasar dari
 *    firmware TX tetap sumber kebenaran; web hanya boleh menambah kepekaan.
 *  - Tiap sensor dibatasi tupoksinya seperti firmware: tanah maks HATI-HATI,
 *    getaran maks WASPADA, kemiringan maks BAHAYA. Tanpa batas ini, tanah
 *    100 % akan tampil BAHAYA padahal firmware hanya memberi HATI-HATI.
 *  - Ambang default identik dengan angka firmware (`FIRMWARE_THRESHOLDS`),
 *    jadi selama pengguna tidak mengubah apa pun hasilnya sama persis.
 *
 * Dipanggil di handler pesan MQTT (bukan saat render) supaya pembacaan
 * localStorage tetap di luar render dan tidak memicu hydration mismatch.
 */
export function applyCalibration(
  firmwareLevel: Level,
  soil_saturation: number,
  vibration: number,
  tilt_angle: number,
  thresholds: Thresholds = loadThresholds()
): Level {
  const fwIdx = (LEVEL_ORDER as readonly string[]).indexOf(firmwareLevel);
  const soilIdx = Math.min(thresholdIndex(soil_saturation, thresholds.soil_saturation), 1);
  const vibIdx  = Math.min(thresholdIndex(vibration, thresholds.vibration_freq), 2);
  const tiltIdx = thresholdIndex(tilt_angle, thresholds.tilt_angle);
  const finalIdx = Math.max(fwIdx < 0 ? 0 : fwIdx, soilIdx, vibIdx, tiltIdx);
  return LEVEL_ORDER[finalIdx];
}
