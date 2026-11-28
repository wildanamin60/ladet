/**
 * LADET — Supabase client
 * Pastikan env vars diset di .env.local:
 *   NEXT_PUBLIC_SUPABASE_URL=...
 *   NEXT_PUBLIC_SUPABASE_ANON_KEY=...
 */

import { createClient } from '@supabase/supabase-js';

const configuredUrl = process.env.NEXT_PUBLIC_SUPABASE_URL ?? '';
const configuredKey = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY ?? '';

/**
 * Supabase hanya dipakai bila URL & anon key benar-benar diisi (bukan
 * placeholder). Kalau tidak, client tetap dibuat dengan nilai dummy supaya
 * `createClient` tidak melempar error saat modul di-import — halaman yang
 * tidak memakai Supabase tetap bisa tampil (mis. deploy hanya dengan MQTT).
 */
export const SUPABASE_ENABLED =
  !!configuredUrl && !!configuredKey &&
  !configuredUrl.includes('your-project') &&
  !configuredKey.includes('your-anon-key');

const supabaseUrl     = SUPABASE_ENABLED ? configuredUrl : 'https://placeholder.supabase.co';
const supabaseAnonKey = SUPABASE_ENABLED ? configuredKey : 'placeholder-anon-key';

export const supabase = createClient(supabaseUrl, supabaseAnonKey);

// ── Types ─────────────────────────────────────────────────────────────────────

export interface SensorData {
  id?: number;
  created_at?: string;
  node_id: string;
  soil_saturation: number;          // % — SEN0308
  vibration_freq: number;           // Δg — ADXL345 (delta getaran)
  tilt_angle: number;               // ° — BNO055
  rssi: number;
  snr: number;
  delay_ms: number;
  packet_id: number;
  is_received: boolean;
  status_level: 'aman' | 'hati-hati' | 'waspada' | 'bahaya';
  spreading_factor: 'SF7' | 'SF9' | 'SF12';
  distance_m: number;
}

export interface PacketLog {
  id?: number;
  created_at?: string;
  packet_id: number;
  sent_at: string;
  received_at: string;
  is_received: boolean;
  spreading_factor: 'SF7' | 'SF9' | 'SF12';
  distance_m: number;
  delay_ms: number;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

export async function insertSensorData(data: Omit<SensorData, 'id' | 'created_at'>) {
  if (!SUPABASE_ENABLED) return;
  const { error } = await supabase.from('sensor_data').insert(data);
  if (error) console.warn('[Supabase] insertSensorData:', error.message);
}

export async function fetchLatestSensorData(
  limit = 50,
  sf?: 'SF7' | 'SF9' | 'SF12',
  distanceM?: number
) {
  if (!SUPABASE_ENABLED) return [] as SensorData[];
  let query = supabase
    .from('sensor_data')
    .select('*')
    .order('created_at', { ascending: false })
    .limit(limit);
  if (sf)        query = query.eq('spreading_factor', sf);
  if (distanceM) query = query.eq('distance_m', distanceM);
  const { data, error } = await query;
  if (error) console.warn('[Supabase] fetchLatestSensorData:', error.message);
  return (data ?? []) as SensorData[];
}

export async function fetchSensorDataByScenario(
  sf: 'SF7' | 'SF9' | 'SF12',
  distanceM: number,
  limit = 50
) {
  if (!SUPABASE_ENABLED) return [] as SensorData[];
  const { data, error } = await supabase
    .from('sensor_data')
    .select('*')
    .eq('spreading_factor', sf)
    .eq('distance_m', distanceM)
    .order('created_at', { ascending: true })
    .limit(limit);
  if (error) console.warn('[Supabase] fetchSensorDataByScenario:', error.message);
  return (data ?? []) as SensorData[];
}

export async function fetchAllScenarioStats() {
  if (!SUPABASE_ENABLED) return [] as Pick<SensorData, 'spreading_factor' | 'distance_m' | 'rssi' | 'snr' | 'delay_ms' | 'is_received'>[];
  const { data, error } = await supabase
    .from('sensor_data')
    .select('spreading_factor, distance_m, rssi, snr, delay_ms, is_received');
  if (error) console.warn('[Supabase] fetchAllScenarioStats:', error.message);
  return (data ?? []) as Pick<SensorData, 'spreading_factor' | 'distance_m' | 'rssi' | 'snr' | 'delay_ms' | 'is_received'>[];
}

export async function insertPacketLog(log: Omit<PacketLog, 'id' | 'created_at'>) {
  if (!SUPABASE_ENABLED) return;
  const { error } = await supabase.from('packet_log').insert(log);
  if (error) console.warn('[Supabase] insertPacketLog:', error.message);
}

export async function fetchPacketLog(sf?: 'SF7' | 'SF9' | 'SF12', distanceM?: number) {
  if (!SUPABASE_ENABLED) return [] as PacketLog[];
  let query = supabase
    .from('packet_log')
    .select('*')
    .order('created_at', { ascending: true });
  if (sf)        query = query.eq('spreading_factor', sf);
  if (distanceM) query = query.eq('distance_m', distanceM);
  const { data, error } = await query;
  if (error) console.warn('[Supabase] fetchPacketLog:', error.message);
  return (data ?? []) as PacketLog[];
}

// ── Riwayat peringatan (ditulis oleh firmware RX) ────────────────────────────

/** Satu baris tabel `ladet_alerts`. */
export interface AlertRow {
  id: number;
  created_at: string;
  /** 1 = HATI-HATI, 2 = WASPADA, 3 = BAHAYA */
  level: number;
  level_name: string;
  soil_saturation: number | null;
  delta_g: number | null;
  tilt_angle: number | null;
}

export async function fetchAlertHistory(limit = 200): Promise<AlertRow[]> {
  if (!SUPABASE_ENABLED) return [];
  const { data, error } = await supabase
    .from('ladet_alerts')
    .select('*')
    .order('created_at', { ascending: false })
    .limit(limit);
  if (error) {
    console.warn('[Supabase] fetchAlertHistory:', error.message);
    return [];
  }
  return (data ?? []) as AlertRow[];
}

