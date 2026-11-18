/**
 * LADET — Mode Demo: data dummy untuk testing tanpa hardware
 * 12 skenario: 4 jarak (100m, 300m, 700m, 1000m) × 3 SF (SF7, SF9, SF12)
 */

import type { LadetPayload } from './mqtt';
import type { SensorData } from './supabase';

export type SF = 'SF7' | 'SF9' | 'SF12';

// Karakteristik delay dan RSSI per SF — SF12 paling lambat, range paling jauh
const SF_PROFILE: Record<SF, { delayBase: number; delayJitter: number; rssiBonus: number; snrBase: number }> = {
  SF7:  { delayBase: 80,  delayJitter: 60,  rssiBonus: 0,  snrBase: 9  },
  SF9:  { delayBase: 200, delayJitter: 100, rssiBonus: 3,  snrBase: 7  },
  SF12: { delayBase: 500, delayJitter: 200, rssiBonus: 6,  snrBase: 4  },
};

// RSSI makin lemah seiring jarak. Clamp ke ± noise floor praktis (≈ −119 dBm):
// rumus log-linear murni menghasilkan −149/−185 dBm di 700/1000 m — di bawah
// sensitivitas SX1278 (≈ −137 dBm), tidak masuk akal untuk grafik demo.
function rssiForDistance(distanceM: number, sfBonus: number): number {
  const base = -65 - (distanceM / 100) * 12 + sfBonus;
  return +(Math.max(-119, base + (Math.random() * 6 - 3))).toFixed(1);
}

// Packet loss makin besar seiring jarak dan SF rendah
function packetLossRate(distanceM: number, sf: SF): number {
  const base = (distanceM / 1000) * (sf === 'SF7' ? 15 : sf === 'SF9' ? 8 : 3);
  return Math.min(0.5, base);
}

// ── Counter SEQ demo (meniru SEQ 1..50 dari firmware TX) ─────────────────────
let demoTickRef = 0;
export function resetDemoSequence(): void { demoTickRef = 0; }

export function generateDemoPayload(
  sf: SF = 'SF9',
  distanceM: number = 100
): LadetPayload {
  const profile = SF_PROFILE[sf];
  const rssi    = rssiForDistance(distanceM, profile.rssiBonus);
  const snr     = +(profile.snrBase + (Math.random() * 4 - 2)).toFixed(1);
  const delay   = +(profile.delayBase + Math.random() * profile.delayJitter).toFixed(0);
  const lost    = Math.random() < packetLossRate(distanceM, sf);

  // Level simulasi — mayoritas aman, kadang naik
  const rand = Math.random();
  let level: LadetPayload['level'] = 'aman';
  if      (rand > 0.93) level = 'bahaya';
  else if (rand > 0.80) level = 'waspada';
  else if (rand > 0.60) level = 'hati-hati';

  const soil = level === 'bahaya'    ? 80 + Math.random() * 15
             : level === 'waspada'   ? 65 + Math.random() * 12
             : level === 'hati-hati' ? 62 + Math.random() * 8
             : 20  + Math.random() * 38;

  const vibFreq = level === 'bahaya'    ? 8  + Math.random() * 5
                : level === 'waspada'   ? 4  + Math.random() * 3
                : level === 'hati-hati' ? 1  + Math.random() * 2
                : Math.random() * 0.8;

  const tilt = level === 'bahaya'    ? 18 + Math.random() * 10
             : level === 'waspada'   ? 8  + Math.random() * 6
             : level === 'hati-hati' ? 3  + Math.random() * 4
             : Math.random() * 2.5;

  return {
    node_id:          'LADET-NODE-01',
    timestamp:        Date.now(),
    soil_saturation:  +soil.toFixed(1),
    vibration_freq:   +vibFreq.toFixed(2),
    tilt_angle:       +tilt.toFixed(2),
    rssi:             lost ? -120 : rssi,
    snr:              lost ? -5   : snr,
    delay_ms:         lost ? 9999 : delay,
    packet_loss:      lost ? +(packetLossRate(distanceM, sf) * 100).toFixed(1) : 0,
    packet_id:        ++demoTickRef,
    spreading_factor: sf,
    distance_m:       distanceM,
    level,
  };
}

/** Generate histori dummy untuk grafik & tabel */
export function generateDemoHistory(
  count = 50,
  sf: SF = 'SF9',
  distanceM = 100
): SensorData[] {
  const now = Date.now();
  return Array.from({ length: count }, (_, i) => {
    const p = generateDemoPayload(sf, distanceM);
    return {
      id:               count - i,
      created_at:       new Date(now - (count - i) * 5000).toISOString(),
      node_id:          'LADET-NODE-01',
      soil_saturation:  p.soil_saturation,
      vibration_freq:   p.vibration_freq,
      tilt_angle:       p.tilt_angle,
      rssi:             p.rssi,
      snr:              p.snr,
      delay_ms:         p.delay_ms,
      packet_id:        i + 1,
      is_received:      Math.random() > packetLossRate(distanceM, sf),
      status_level:     p.level,
      spreading_factor: sf,
      distance_m:       distanceM,
    };
  });
}

// ── 12 Skenario Demo (4 jarak × 3 SF) ────────────────────────────────────────

export interface ScenarioStats {
  key:        string;
  label:      string;
  sf:         SF;
  distance_m: number;
  rssiAvg:    number;
  snrAvg:     number;
  delayAvg:   number;
  packetLoss: number;
  packetSent: number;
  packetRecv: number;
}

const DISTANCES = [100, 300, 700, 1000];
const SF_LIST: SF[] = ['SF7', 'SF9', 'SF12'];

export const DEMO_SCENARIOS: ScenarioStats[] = DISTANCES.flatMap((d, di) =>
  SF_LIST.map((sf, si) => {
    const profile   = SF_PROFILE[sf];
    const plRate    = packetLossRate(d, sf);
    const sent      = 50;
    const recv      = Math.round(sent * (1 - plRate));
    const rssiAvg   = +(rssiForDistance(d, profile.rssiBonus) - 1).toFixed(1);
    const snrAvg    = +(profile.snrBase - di * 1.5 + (Math.random() * 0.4)).toFixed(1);
    const delayAvg  = +(profile.delayBase + di * 40 + 10).toFixed(0);
    const plPct     = +((sent - recv) / sent * 100).toFixed(1);
    return {
      key:        `S${di * 3 + si + 1}`,
      label:      `S${di * 3 + si + 1} — ${d}m / ${sf}`,
      sf,
      distance_m: d,
      rssiAvg,
      snrAvg,
      delayAvg,
      packetLoss: plPct,
      packetSent: sent,
      packetRecv: recv,
    };
  })
);

