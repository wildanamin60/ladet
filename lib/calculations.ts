/**
 * LADET — Landslide Early Detector Tool
 * Rumus kalkulasi QoS: Delay, Packet Loss, SNR
 */

export interface PacketEntry {
  packet_id: number;
  sent_at: number;      // ms timestamp
  received_at: number;  // ms timestamp
  is_received: boolean;
}

export interface QoSStats {
  delayAvg: number;       // ms
  delayMin: number;       // ms
  delayMax: number;       // ms
  packetLoss: number;     // %
  packetSent: number;
  packetReceived: number;
  snrAvg: number;         // dB
  snrMin: number;
  snrMax: number;
  rssiAvg: number;        // dBm
  rssiMin: number;
  rssiMax: number;
}

/**
 * Delay = t_terima − t_kirim  (ms)
 */
export function calculateDelay(sentAt: number, receivedAt: number): number {
  return Math.max(0, receivedAt - sentAt);
}

/**
 * Delay rata-rata = ΣDelay / n
 */
export function calculateAvgDelay(delays: number[]): number {
  if (delays.length === 0) return 0;
  return delays.reduce((sum, d) => sum + d, 0) / delays.length;
}

/**
 * Packet Loss (%) = ((P_kirim − P_terima) / P_kirim) × 100
 */
export function calculatePacketLoss(sent: number, received: number): number {
  if (sent === 0) return 0;
  return Math.max(0, ((sent - received) / sent) * 100);
}

/**
 * SNR (dB) = 10 × log10(P_signal / P_noise)
 * Catatan: biasanya sudah diberikan langsung oleh LoRa modul
 */
export function calculateSNR(signalPower: number, noisePower: number): number {
  if (noisePower === 0) return 0;
  return 10 * Math.log10(signalPower / noisePower);
}

/**
 * Hitung statistik lengkap dari array data historis
 */
export function computeQoSStats(
  packets: PacketEntry[],
  rssiValues: number[],
  snrValues: number[]
): QoSStats {
  const received = packets.filter(p => p.is_received);
  const delays = received.map(p => calculateDelay(p.sent_at, p.received_at));

  const delayAvg = calculateAvgDelay(delays);
  const delayMin = delays.length > 0 ? Math.min(...delays) : 0;
  const delayMax = delays.length > 0 ? Math.max(...delays) : 0;

  const packetLoss = calculatePacketLoss(packets.length, received.length);

  const snrAvg = snrValues.length > 0
    ? snrValues.reduce((s, v) => s + v, 0) / snrValues.length : 0;
  const snrMin = snrValues.length > 0 ? Math.min(...snrValues) : 0;
  const snrMax = snrValues.length > 0 ? Math.max(...snrValues) : 0;

  const rssiAvg = rssiValues.length > 0
    ? rssiValues.reduce((s, v) => s + v, 0) / rssiValues.length : 0;
  const rssiMin = rssiValues.length > 0 ? Math.min(...rssiValues) : 0;
  const rssiMax = rssiValues.length > 0 ? Math.max(...rssiValues) : 0;

  return {
    delayAvg: +delayAvg.toFixed(2),
    delayMin: +delayMin.toFixed(2),
    delayMax: +delayMax.toFixed(2),
    packetLoss: +packetLoss.toFixed(2),
    packetSent: packets.length,
    packetReceived: received.length,
    snrAvg: +snrAvg.toFixed(2),
    snrMin: +snrMin.toFixed(2),
    snrMax: +snrMax.toFixed(2),
    rssiAvg: +rssiAvg.toFixed(2),
    rssiMin: +rssiMin.toFixed(2),
    rssiMax: +rssiMax.toFixed(2),
  };
}

/**
 * Format timestamp ke WIB (UTC+7), format DD/MM/YYYY HH:MM:SS
 */
export function formatWIB(date: Date | string | number): string {
  const d = new Date(date);
  return d.toLocaleString('id-ID', {
    timeZone: 'Asia/Jakarta',
    day: '2-digit',
    month: '2-digit',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  }).replace(/\./g, '/');
}

/**
 * Format waktu singkat HH:MM:SS WIB
 */
export function formatTimeWIB(date: Date | string | number): string {
  const d = new Date(date);
  return d.toLocaleTimeString('id-ID', {
    timeZone: 'Asia/Jakarta',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  });
}
