'use client';

import { useEffect } from 'react';
import { DEFAULT_MQTT_CONFIG, getMqttClient, LADET_TOPICS, parsePayload } from '@/lib/mqtt';
import {
  ALERT_MESSAGE,
  ingestAlerts,
  levelFromNumber,
  recordLevel,
  wibToIso,
  type AlertEntry,
} from '@/lib/alertHistory';

/** Log peringatan terakhir dari RX (retained) — berisi kejadian saat web tertutup. */
const ALERT_LOG_TOPIC = 'ladet/alert/log';

const IS_DEMO = process.env.NEXT_PUBLIC_DEMO_MODE === 'true';

/**
 * Perekam riwayat peringatan. Dipasang sekali di root layout supaya riwayat
 * tetap terkumpul di halaman mana pun yang sedang dibuka, bukan hanya di
 * halaman /riwayat.
 */
export default function AlertRecorder() {
  useEffect(() => {
    if (IS_DEMO) return;

    const client = getMqttClient(DEFAULT_MQTT_CONFIG);

    const handleMessage = (topic: string, msg: Buffer) => {
      if (topic === ALERT_LOG_TOPIC) {
        try {
          const arr = JSON.parse(msg.toString());
          if (!Array.isArray(arr)) return;
          const entries: AlertEntry[] = [];
          for (const o of arr) {
            const level = levelFromNumber(Number(o?.l));
            const t = typeof o?.t === 'string' ? o.t : '';
            if (!level || !t) continue;
            entries.push({ level, message: ALERT_MESSAGE[level], created_at: wibToIso(t) });
          }
          ingestAlerts(entries);
        } catch {
          /* payload rusak diabaikan */
        }
        return;
      }

      const parsed = parsePayload(topic, msg);
      if (parsed?.level) recordLevel(parsed.level);
    };

    const handleConnect = () => {
      LADET_TOPICS.forEach((t) => client.subscribe(t));
      client.subscribe(ALERT_LOG_TOPIC);
    };

    if (client.connected) handleConnect();
    client.on('connect', handleConnect);
    client.on('message', handleMessage);

    return () => {
      client.off('connect', handleConnect);
      client.off('message', handleMessage);
    };
  }, []);

  return null;
}
