/**
 * LADET — MQTT Client via WebSocket
 * Gunakan di sisi client (browser) saja — "use client"
 *
 * Features:
 *  - Singleton with automatic broker fallback (HiveMQ → EMQX → HiveMQ alt)
 *  - Auto-reconnect with smart broker switching on repeated failures
 *  - Proper event handler cleanup (no memory leaks)
 */

import mqtt, { MqttClient, IClientOptions } from 'mqtt';
import { normalizeLevel } from './thresholds';

/* ------------------------------------------------------------------ */
/*  Types                                                              */
/* ------------------------------------------------------------------ */

export interface LadetPayload {
  node_id: string;
  timestamp: number;
  soil_saturation: number;   // % — SEN0308
  vibration_freq: number;    // Δg — ADXL345 (delta getaran; nama field warisan)
  tilt_angle: number;        // ° — BNO055
  /** ── Kolom tambahan CSV skripsi (opsional, dari payload sensor RX) ── */
  theta_measured?: number;       // kelembapan terukur (m³/m³) — THETA dari TX
  theta_r?: number;              // titik kering θr (m³/m³) — THETAR dari TX
  theta_s?: number;              // titik jenuh θs (m³/m³) — THETAS dari TX
  g_initial?: number;            // getaran acuan (baseline) — G0 dari TX
  g_measured?: number;           // getaran terukur — G dari TX
  delta_g?: number;              // delta getaran (acuan status) — DG dari TX
  tilt_angle_measured?: number;  // sudut terukur (BNO Z) — THETAM dari TX
  tilt_angle_reference?: number; // sudut acuan (baseline) — THETA0 dari TX
  tx_time?: string;              // waktu kirim versi TX (dihitung RX dari RX terima − delay; BUKAN RTC mentah)
  tx_rtt_ms?: number;            // RTT paket sebelumnya (ms) — dari field RTT= paket TX (1 jam akurat, tidak perlu RTC)
  rssi: number;              // dBm
  snr: number;               // dB
  delay_ms: number;          // ms
  delay_source?: string;     // 'rtt' | 'clock' | 'clock-rel' | 'invalid' | 'unknown' — cara delay diukur
                             //   clock     = absolut, offset dari RTT TX (paling akurat)
                             //   clock-rel = relatif thd paket tercepat (TX belum kirim RTT=)
                             //   rtt       = RTT/2 langsung (TX tidak kirim MS=)
                             //   invalid   = di luar rentang wajar, dibuang dashboard
  packet_loss: number;       // % — from RX sequence tracking
  packet_id: number;
  /** Nomor urut paket yang DITERIMA RX (monoton naik). Dipakai sebagai kunci
   *  anti-duplikat kalau firmware TX mengirim SEQ yang sama terus (SEQ macet). */
  rx_count?: number;
  /** millis() RX saat paket LoRa diterima */
  rx_ms?: number;
  /** true = SEQ dari TX tidak maju (firmware TX lama selalu SEQ=1) */
  seq_stuck?: boolean;
  /** Nomor boot RX. Berubah di tengah pengujian = gateway reboot. */
  rx_boot?: number;
  /** Umur RX (detik) saat paket ini — kecil berarti baru nyala/reboot. */
  rx_uptime_s?: number;
  /** Jumlah paket TX yang hilang tepat sebelum paket ini (0 = berurutan). */
  seq_gap?: number;
  /** Penyebab reboot RX terakhir (BROWNOUT / TASK_WDT / PANIC / POWERON). */
  rx_reset?: string;
  /** Berapa kali ACK RX gagal selesai (TxDone tidak datang) sejak boot. */
  rx_ack_timeouts?: number;
  /** Tracking loss RX (deteksi gap SEQ): total paket yang dikirim TX.
   *  Sumber kebenaran untuk counter TERKIRIM di dashboard. */
  total_expected?: number;
  /** Tracking loss RX: total paket yang benar-benar diterima RX. */
  total_received?: number;
  spreading_factor: 'SF7' | 'SF9' | 'SF12';
  distance_m: number;        // meter
  level: 'aman' | 'hati-hati' | 'waspada' | 'bahaya';
}

export interface MqttConfig {
  brokerUrl: string;
  clientId?: string;
  username?: string;
  password?: string;
}

/* ------------------------------------------------------------------ */
/*  Topics                                                             */
/* ------------------------------------------------------------------ */

/**
 * Topik data yang di-subscribe dashboard.
 *
 * Arsitektur resmi LADET: TX → LoRa → RX → MQTT → dashboard.
 * Satu-satunya sumber data sensor adalah RX yang mem-publish JSON nested ke
 * `ladet/data`. Topik lama `ladet/sensor/*`, `ladet/qos/*`, dan
 * `ladet/status/level` (dari firmware TX→MQTT langsung / mode pameran) sudah
 * TIDAK dipakai lagi supaya dashboard tidak bisa menampilkan data tanpa RX.
 */
export const LADET_TOPICS = [
  'ladet/data',              // satu-satunya topik data dari RX (JSON nested)
] as const;

export type LadetTopic = typeof LADET_TOPICS[number];

/* ------------------------------------------------------------------ */
/*  Singleton State                                                    */
/* ------------------------------------------------------------------ */

let clientInstance: MqttClient | null = null;
let connectFailCount = 0;

function buildBrokerUrl(config: MqttConfig): string {
  return config.brokerUrl || process.env.NEXT_PUBLIC_MQTT_BROKER_URL ||
    'wss://broker.hivemq.com:8884/mqtt';
}

/* ------------------------------------------------------------------ */
/*  Core: getMqttClient                                                */
/* ------------------------------------------------------------------ */

export function getMqttClient(config: MqttConfig): MqttClient {
  // Return existing client even if disconnected — let auto-reconnect handle it
  if (clientInstance) {
    console.log('[MQTT] Reusing existing client instance, connected:', clientInstance.connected);
    return clientInstance;
  }

  const brokerUrl = buildBrokerUrl(config);
  const options: IClientOptions = {
    clientId: config.clientId ?? `ladet-${Math.random().toString(16).slice(2, 10)}`,
    clean: true,
    // Keep retrying the SAME broker. The ESP32 publisher also uses HiveMQ;
    // switching the browser to another public broker would show "connected"
    // but it could never receive ladet/data from the ESP32.
    reconnectPeriod: 2000,
    connectTimeout: 15000,
    keepalive: 20,
    protocolVersion: 4,          // MQTT 3.1.1
    resubscribe: true,           // auto-resubscribe on reconnect
    rejectUnauthorized: false,   // some brokers need this for WSS
    ...(config.username && { username: config.username }),
    ...(config.password && { password: config.password }),
  };

  console.log(`[MQTT] 🆕 Creating client → ${brokerUrl}`);
  clientInstance = mqtt.connect(brokerUrl, options);
  console.log('[MQTT] ℹ️ Automatic reconnect is enabled; broker will not be switched');

  /* --- connection lifecycle --- */
  clientInstance.on('connect', () => {
    connectFailCount = 0;  // reset on success
    console.log('[MQTT] ✅ Client connected');
  });

  clientInstance.on('reconnect', () => {
    console.log('[MQTT] 🔄 Reconnecting...');
  });

  clientInstance.on('offline', () => {
    console.log('[MQTT] 📴 Client offline');
  });

  clientInstance.on('error', (err) => {
    connectFailCount++;
    console.error(`[MQTT] ❌ Error (#${connectFailCount}):`, err.message);
  });

  clientInstance.on('close', () => {
    console.log('[MQTT] 🔒 Connection closed');
  });

  return clientInstance;
}

/** Destroy current singleton cleanly */
function destroyClient(): void {
  if (clientInstance) {
    clientInstance.removeAllListeners();
    clientInstance.end(true);
    clientInstance = null;
  }
  connectFailCount = 0;
}

export function disconnectMqtt(): void {
  destroyClient();
}

/** Enhanced payload structure from RX (nested sensor/radio/test) */
export interface EnhancedPayload {
  type?: string;
  packet_id: number;
  timestamp: string;
  sensor?: {
    soil_saturation: number;
    g_initial?: number;
    g?: number;
    delta_g?: number;
    tilt_x?: number;
    tilt_y?: number;
    tilt_z?: number;
    delta_tilt_x?: number;
    delta_tilt_y?: number;
    delta_tilt_z?: number;
    tilt_angle?: number;
  };
  status?: string;
  radio?: {
    sf: string;
    rssi: number;
    snr: number;
  };
  test?: {
    distance_m: number;
    delay_ms?: number | null;
    delay_source?: string;
    packet_loss?: number;
    total_expected?: number;
    total_received?: number;
    rx_count?: number;
    rx_ms?: number;
    seq_stuck?: boolean;
    /** Nomor boot RX — berubah di tengah run berarti gateway reboot. */
    rx_boot?: number;
    /** Umur RX (detik) saat paket ini diterima — kecil = baru nyala. */
    rx_uptime_s?: number;
    /** Jumlah paket TX yang hilang tepat sebelum paket ini (0 = berurutan). */
    seq_gap?: number;
    /** Penyebab reboot RX terakhir (BROWNOUT / TASK_WDT / PANIC / POWERON). */
    rx_reset?: string;
    /** Berapa kali ACK RX gagal selesai (TxDone tidak datang) sejak boot. */
    rx_ack_timeouts?: number;
  };
}

/**
 * Bentuk payload MENTAH dari RX sebelum dinormalisasi. Semua field opsional:
 * firmware lama belum mengirim semuanya, jadi `normalizePayload` mengisi nilai
 * bawaan. Nested (sensor/radio/test) = format resmi RX; flat = format lama.
 */
export type RawLadetPayload = Partial<LadetPayload> & {
  sensor?: Partial<NonNullable<EnhancedPayload['sensor']>> & {
    theta_measured?: number;
    theta_r?: number;
    theta_s?: number;
    tilt_angle_measured?: number;
    tilt_angle_reference?: number;
    tx_time?: string;
  };
  radio?: Partial<NonNullable<EnhancedPayload['radio']>>;
  test?: Partial<NonNullable<EnhancedPayload['test']>> & { rtt_ms?: number };
  status?: string;
  status_level?: string;
};

/** Normalize enhanced payload to LadetPayload (backward compatible) */
export function normalizePayload(raw: RawLadetPayload): LadetPayload {
  // If nested structure (new format from RX)
  if (raw.sensor || raw.radio || raw.test) {
    return {
      node_id: 'LADET-NODE-01',
      timestamp: Date.now(),
      soil_saturation: raw.sensor?.soil_saturation ?? 0,
      vibration_freq: raw.sensor?.delta_g ?? 0,  // DG dari TX — satuan Δg (bukan Hz)
      tilt_angle: raw.sensor?.tilt_angle ?? 0,
      rssi: raw.radio?.rssi ?? -100,
      snr: raw.radio?.snr ?? 0,
      delay_ms: raw.test?.delay_ms ?? 0,
      delay_source: raw.test?.delay_source ?? 'unknown',
      // Kolom tambahan CSV skripsi (ada di firmware baru; undefined dgn firmware lama)
      theta_measured: raw.sensor?.theta_measured ?? undefined,
      theta_r: raw.sensor?.theta_r ?? undefined,
      theta_s: raw.sensor?.theta_s ?? undefined,
      g_initial: raw.sensor?.g_initial ?? undefined,
      g_measured: raw.sensor?.g ?? undefined,
      delta_g: raw.sensor?.delta_g ?? undefined,
      tilt_angle_measured: raw.sensor?.tilt_angle_measured ?? undefined,
      tilt_angle_reference: raw.sensor?.tilt_angle_reference ?? undefined,
      // Waktu kirim versi TX: dihitung RX = (waktu RX terima) − delay.
      // JANGAN pakai RTC mentah (raw.timestamp): RTC belum diset → 1/1/2000.
      // RTT paket sebelumnya ikut diteruskan apa adanya (1 jam akurat).
      tx_time: raw.sensor?.tx_time ?? undefined,
      tx_rtt_ms: raw.test?.rtt_ms ?? undefined,
      packet_loss: raw.test?.packet_loss ?? 0,
      packet_id: raw.packet_id ?? 0,
      rx_count: raw.test?.rx_count ?? undefined,
      rx_ms: raw.test?.rx_ms ?? undefined,
      seq_stuck: raw.test?.seq_stuck ?? false,
      rx_boot: raw.test?.rx_boot ?? undefined,
      rx_uptime_s: raw.test?.rx_uptime_s ?? undefined,
      seq_gap: raw.test?.seq_gap ?? undefined,
      rx_reset: raw.test?.rx_reset ?? undefined,
      rx_ack_timeouts: raw.test?.rx_ack_timeouts ?? undefined,
      total_expected: raw.test?.total_expected ?? undefined,
      total_received: raw.test?.total_received ?? undefined,
      spreading_factor: (raw.radio?.sf ?? 'SF9') as LadetPayload['spreading_factor'],
      distance_m: raw.test?.distance_m ?? 100,
      // Status dari firmware dipakai apa adanya (tupoksi sensor sudah
      // ditentukan di firmware TX) — hanya dinormalisasi bentuknya.
      level: normalizeLevel(raw.status),
    };
  }
  // Flat structure — format lama dari TX→MQTT langsung / demo pameran.
  // Dipertahankan hanya sebagai kompatibilitas parser; `parsePayload` sudah
  // menolak payload flat untuk topik `ladet/data` supaya jalur resmi tetap
  // TX → LoRa → RX → MQTT.
  // Normalize `status` here too: otherwise a flat payload containing
  // `status: "WASPADA"` has no usable `level` field in the UI.
  return {
    ...raw,
    level: normalizeLevel(raw.level ?? raw.status ?? raw.status_level),
  } as LadetPayload;
}

export function parsePayload(topic: string, message: Buffer | string): Partial<LadetPayload> | null {
  // Data sensor resmi hanya datang dari RX lewat `ladet/data`.
  if (topic !== 'ladet/data') return null;

  try {
    const raw = typeof message === 'string' ? message : message.toString();
    const parsed = JSON.parse(raw);

    // Arsitektur resmi LADET: data sensor WAJIB datang dari RX dalam format
    // nested (sensor/radio/test). Payload flat adalah sisa jalur TX→MQTT
    // langsung (mode pameran) — tolak supaya dashboard tidak bisa menampilkan
    // data tanpa melewati LoRa + RX.
    if (!parsed.sensor && !parsed.radio && !parsed.test) {
      console.warn('[MQTT] Payload flat dari TX→MQTT diabaikan — data harus dari RX (nested).');
      return null;
    }

    return normalizePayload(parsed);
  } catch {
    // Bukan JSON valid → bukan payload RX.
    return null;
  }
}

/** Publish konfigurasi SF ke ESP32 via MQTT */
export function publishSF(client: MqttClient, sf: 'SF7' | 'SF9' | 'SF12'): void {
  client.publish('ladet/config/sf', sf, { retain: true });
}

/** Publish jarak pengujian ke ESP32 via MQTT */
export function publishDistance(client: MqttClient, distanceM: number): void {
  client.publish('ladet/config/distance', String(distanceM), { retain: true });
}

export const DEFAULT_MQTT_CONFIG: MqttConfig = {
  brokerUrl: process.env.NEXT_PUBLIC_MQTT_BROKER_URL ?? 'wss://broker.hivemq.com:8884/mqtt',
};

/* ------------------------------------------------------------------ */
/*  Config remote (web -> MQTT -> RX -> LoRa -> TX)                    */
/* ------------------------------------------------------------------ */

/** RX firmware mem-publish konfirmasi config ke topik ini (JSON). */
export const CONFIG_STATUS_TOPIC = 'ladet/config/status';

/** Kehadiran perangkat (retained "online"/"offline" — LWT untuk RX). */
export const RX_ONLINE_TOPIC = 'ladet/rx/online';
export const TX_ONLINE_TOPIC = 'ladet/tx/online';

/**
 * Diagnostik gateway RX (JSON, non-retained, tiap 1 menit).
 * Bukan data sensor — dipakai panel engineer untuk melihat kesehatan perangkat:
 * penyebab reboot terakhir, jumlah boot, uptime, heap, dan kekuatan WiFi.
 */
export const RX_STATUS_TOPIC = 'ladet/rx/status';

export interface ConfigStatus {
  /** SF radio yang aktif setelah config (7/9/12) */
  sf?: number;
  /** jarak aktif yang dilaporkan payload (meter) */
  distance?: number;
  /** true = TX sudah mengonfirmasi via handshake LoRa */
  tx_confirmed?: boolean;
  /** applied | queued | rejected | tx_unconfirmed | idle */
  state?: 'applied' | 'queued' | 'rejected' | 'tx_unconfirmed' | 'idle';
}

export function parseConfigStatus(msg: Buffer | string): ConfigStatus | null {
  try {
    return JSON.parse(typeof msg === 'string' ? msg : msg.toString()) as ConfigStatus;
  } catch {
    return null;
  }
}

/* ------------------------------------------------------------------ */
/*  Diagnostik gateway RX (ladet/rx/status)                            */
/* ------------------------------------------------------------------ */

export interface RxHealth {
  /** POWERON | EXT_RESET | SW_RESTART | PANIC | TASK_WDT | WDT | BROWNOUT | … */
  reset?: string;
  /** Jumlah boot sejak NVS dihapus (disimpan firmware di flash) */
  boot_count?: number;
  /** Uptime firmware dalam detik */
  uptime_s?: number;
  /** Free heap (byte) */
  heap?: number;
  /** Kekuatan sinyal WiFi (dBm) */
  wifi_rssi?: number;
  /** Spreading factor yang sedang aktif di RX */
  sf?: number;
}

export function parseRxHealth(msg: Buffer | string): RxHealth | null {
  try {
    const parsed = JSON.parse(typeof msg === 'string' ? msg : msg.toString()) as RxHealth;
    if (!parsed || typeof parsed !== 'object') return null;
    // Wajib punya setidaknya satu field yang dikenal, supaya pesan sampah
    // di topik ini tidak menampilkan kartu kosong.
    const known = ['reset', 'boot_count', 'uptime_s', 'heap', 'wifi_rssi', 'sf'];
    if (!known.some((k) => k in parsed)) return null;
    return parsed;
  } catch {
    return null;
  }
}

