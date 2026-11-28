'use client';

import { useEffect, useState, useRef } from 'react';
import Link from 'next/link';
import { formatTimeWIB } from '@/lib/calculations';
import { DEFAULT_THRESHOLDS, applyCalibration } from '@/lib/thresholds';
import { getMqttClient, parsePayload, DEFAULT_MQTT_CONFIG, LADET_TOPICS, RX_ONLINE_TOPIC } from '@/lib/mqtt';
import { supabase, SUPABASE_ENABLED } from '@/lib/supabase';
import type { LadetPayload } from '@/lib/mqtt';
import type { SensorData } from '@/lib/supabase';
import type { MqttClient } from 'mqtt';

// Check if we should use MQTT (not demo mode)
const IS_DEMO = process.env.NEXT_PUBLIC_DEMO_MODE === 'true' || !process.env.NEXT_PUBLIC_MQTT_BROKER_URL;

// Debug: Log environment variables on load
console.log('[MQTT Home Debug] NEXT_PUBLIC_DEMO_MODE:', process.env.NEXT_PUBLIC_DEMO_MODE);
console.log('[MQTT Home Debug] NEXT_PUBLIC_MQTT_BROKER_URL:', process.env.NEXT_PUBLIC_MQTT_BROKER_URL);
console.log('[MQTT Home Debug] IS_DEMO:', IS_DEMO);

// Supabase dipakai sebagai cadangan bila MQTT tidak mengirim data.
// (Definisi tunggal ada di lib/supabase.ts supaya tidak berbeda antar halaman.)

const POLL_MS = 10_000;            // polling data tiap 10 detik
const STALE_MS = 5 * 60 * 1000;   // OFFLINE bila data terakhir > 5 menit
// Arsitektur resmi: TX → LoRa → RX → MQTT. Badge "LIVE" hanya boleh menyala
// bila data segar benar-benar datang dari RX, bukan sekadar browser terhubung
// ke broker. TX mengirim tiap 2 dtk; 35 dtk memberi ruang untuk SF12 / antrean.
const RX_FRESH_MS = 35_000;

type Level = 'aman' | 'hati-hati' | 'waspada' | 'bahaya';
type AlertItem = { level: Level; message: string; created_at: string };

const LEVEL_ACCENT: Record<Level, string> = {
  aman: '#4CAF50', 'hati-hati': '#F9A825', waspada: '#E65100', bahaya: '#B71C1C',
};

const ALERT_MESSAGE: Record<Level, string> = {
  aman: 'Kondisi aman.',
  'hati-hati': 'Kelembapan tanah meningkat.',
  waspada: 'Getaran terdeteksi pada lereng.',
  bahaya: 'LERENG BERGERAK — segera evakuasi!',
};

// ── Supabase data helpers ──────────────────────────────────────────────
function mapSensorData(r: SensorData): LadetPayload {
  return {
    node_id: r.node_id,
    timestamp: r.created_at ? new Date(r.created_at).getTime() : Date.now(),
    soil_saturation: r.soil_saturation,
    vibration_freq: r.vibration_freq,
    tilt_angle: r.tilt_angle,
    rssi: r.rssi, snr: r.snr, delay_ms: r.delay_ms, packet_loss: 0,
    packet_id: r.packet_id,
    spreading_factor: r.spreading_factor,
    distance_m: r.distance_m,
    level: r.status_level,
  };
}

async function fetchLatestSupabase(): Promise<LadetPayload | null> {
  const { data, error } = await supabase
    .from('sensor_data')
    .select('*')
    .order('created_at', { ascending: false })
    .limit(1);
  if (error || !data || data.length === 0) return null;
  return mapSensorData(data[0] as SensorData);
}



async function fetchAlertsSupabase(): Promise<AlertItem[]> {
  const { data, error } = await supabase
    .from('sensor_data')
    .select('status_level, created_at')
    .neq('status_level', 'aman')
    .order('created_at', { ascending: false })
    .limit(10);
  if (error || !data) return [];
  return (data as { status_level: Level; created_at: string }[]).map((r) => ({
    level: r.status_level,
    message: ALERT_MESSAGE[r.status_level],
    created_at: r.created_at,
  }));
}

function emptyPayload(): LadetPayload {
  return {
    node_id: 'LADET-NODE-01', timestamp: Date.now(),
    soil_saturation: 0, vibration_freq: 0, tilt_angle: 0,
    rssi: 0, snr: 0, delay_ms: 0, packet_loss: 0, packet_id: 0,
    spreading_factor: 'SF9', distance_m: 100, level: 'aman',
  };
}

const T = DEFAULT_THRESHOLDS;

function sensorWord(type: 'moisture' | 'vibration' | 'tilt', v: number): string {
  if (type === 'moisture') {
    const t = T.soil_saturation;
    return v < t.hatiHati ? 'Kering' : v < t.waspada ? 'Lembap' : v < t.bahaya ? 'Basah' : 'Jenuh';
  }
  if (type === 'vibration') {
    const t = T.vibration_freq;
    return v < t.hatiHati ? 'Tenang' : v < t.waspada ? 'Getar Ringan' : v < t.bahaya ? 'Bergetar' : 'Kuat';
  }
  const t = T.tilt_angle;
  return v < t.hatiHati ? 'Stabil' : v < t.waspada ? 'Miring' : v < t.bahaya ? 'Curam' : 'Kritis';
}

// Exact per-level hex tokens: [mid / dark / light-accent]
const LEVEL_THEME: Record<string, { top: string; bottom: string; accent: string }> = {
  aman: { top: '#4CAF50', bottom: '#1B5E20', accent: '#A5D6A7' },
  'hati-hati': { top: '#FFD54F', bottom: '#F9A825', accent: '#FFF9C4' },
  waspada: { top: '#FF6D00', bottom: '#E65100', accent: '#FFE0B2' },
  bahaya: { top: '#F44336', bottom: '#B71C1C', accent: '#FFCDD2' },
};
const LEVEL_ICON: Record<string, string> = {
  aman: '✅', 'hati-hati': '⚠️', waspada: '‼️', bahaya: '🚨',
};
const LEVEL_TITLE: Record<string, string> = {
  aman: 'AMAN', 'hati-hati': 'HATI-HATI', waspada: 'WASPADA', bahaya: 'BAHAYA',
};
const LEVEL_ACTION: Record<string, string> = {
  aman: 'Kondisi aman. Aktivitas dapat berjalan seperti biasa.',
  'hati-hati': 'Waspadai cuaca hujan. Pantau terus kondisi lereng.',
  waspada: 'Kurangi aktivitas di lereng. Siapkan jalur & titik kumpul evakuasi.',
  bahaya: 'SEGERA evakuasi ke tempat aman. Hubungi perangkat desa / BPBD.',
};

export default function HomePage() {
  const [data, setData] = useState<LadetPayload>(emptyPayload());
  const [lastUpdate, setLastUpdate] = useState('—');
  const [connected, setConnected] = useState(false);
  const [lastDataAt, setLastDataAt] = useState<number | null>(null); // kapan data RX terakhir masuk
  const [nowMs, setNowMs] = useState(0);                   // jam utk hitung kesegaran data
  const [alerts, setAlerts] = useState<AlertItem[]>([]);
  const mqttRef = useRef<MqttClient | null>(null);
  const prevLevelRef = useRef<Level>('aman');   // level paket sebelumnya — deteksi transisi
  const dataRef = useRef<LadetPayload | null>(null);  // payload terakhir (untuk merge)
  const lastMqttAtRef = useRef<number | null>(null);       // kapan MQTT terakhir kirim data (gate polling Supabase)

  // Deterministic initial value so SSR and client first render match
  // (real WIB time is set client-side in the effect below to avoid hydration mismatch).
  const [clock, setClock] = useState<string>('--:--:--');

  useEffect(() => {
    // Satu interval untuk jam + kesegaran data RX. Nilai `Date.now()` disimpan
    // di state supaya render tetap murni (tidak memanggil fungsi impure).
    const tick = () => {
      const n = Date.now();
      setNowMs(n);
      setClock(formatTimeWIB(new Date(n)));
    };
    tick();
    const id = setInterval(tick, 1000);
    return () => clearInterval(id);
  }, []);

  // MQTT real-time connection (like Dashboard Engineer)
  useEffect(() => {
    console.log('[MQTT Home] useEffect triggered, IS_DEMO:', IS_DEMO);
    if (IS_DEMO) {
      console.log('[MQTT Home] Skipping MQTT - demo mode active');
      return; // Skip if demo mode
    }

    console.log('[MQTT Home] Connecting to broker:', DEFAULT_MQTT_CONFIG.brokerUrl);
    const client = getMqttClient(DEFAULT_MQTT_CONFIG);
    mqttRef.current = client;

    const handleConnect = () => {
      console.log('[Home] ✅ MQTT Connected');
      setConnected(true);
      LADET_TOPICS.forEach(t => client.subscribe(t));
      // LWT RX: dipakai untuk mematikan badge LIVE lebih cepat saat gateway mati.
      client.subscribe(RX_ONLINE_TOPIC);
    };

    const handleDisconnect = () => {
      console.log('[Home] ⚠️ MQTT Disconnected');
      setConnected(false);
    };

    const handleOffline = () => {
      console.log('[Home] 📴 MQTT Offline');
      setConnected(false);
    };

    const handleMessage = (topic: string, msg: Buffer) => {
      if (topic === RX_ONLINE_TOPIC) {
        // "offline" = LWT RX (gateway mati). Kosongkan penanda data supaya
        // badge langsung MENUNGGU RX, bukan menunggu timeout 35 dtk.
        if (msg.toString().trim() !== 'online') { setLastDataAt(null); lastMqttAtRef.current = null; }
        return;
      }
      // Arsitektur resmi LADET: data sensor HANYA berasal dari RX via
      // `ladet/data`. Topik lama TX→MQTT (ladet/sensor/*, ladet/qos/*,
      // ladet/status/level) diabaikan agar halaman warga tidak menampilkan
      // data tanpa melewati LoRa + RX.
      if (topic !== 'ladet/data') return;
      const parsed = parsePayload(topic, msg);
      if (!parsed) return;
      console.log('[Home]  Message received:', topic, parsed);

      // Gabungkan dengan payload terakhir dari RX.
      //
      // Status dasar tetap dari firmware TX (tupoksi per sensor). Ambang di
      // halaman Pengaturan hanya boleh MENAIKKAN level, tidak menurunkannya,
      // jadi keputusan firmware tidak pernah "dilunakkan" oleh dashboard.
      const merged = { ...(dataRef.current ?? emptyPayload()), ...parsed } as LadetPayload;
      // Ambang dari halaman Pengaturan diterapkan di sini (bukan saat render):
      // hanya bisa MENAIKKAN level, status dasar tetap dari firmware TX.
      merged.level = applyCalibration(
        merged.level,
        merged.soil_saturation,
        merged.vibration_freq,
        merged.tilt_angle
      );
      dataRef.current = merged;
      setData(merged);
      setLastUpdate(formatTimeWIB(new Date()));
      // Data dari `ladet/data` hanya bisa ada kalau RX menerima LoRa dari TX —
      // ini bukti paling kuat bahwa jalur TX → LoRa → RX → MQTT hidup.
      setLastDataAt(Date.now());
      lastMqttAtRef.current = Date.now();
      setConnected(true);

      // ─ Riwayat peringatan dari MQTT ──
      // Sebelumnya daftar ini HANYA diisi lewat polling Supabase, sehingga saat
      // halaman berjalan dengan data MQTT (Supabase belum dikonfigurasi) daftar
      // peringatan selalu kosong. Sekarang transisi level dari payload MQTT
      // langsung dicatat, jadi notifikasi tetap muncul tanpa Supabase.
      const lvl = merged.level;
      if (lvl !== prevLevelRef.current) {
        prevLevelRef.current = lvl;
        if (lvl !== 'aman') {
          setAlerts(prev => [
            {
              level: lvl,
              message: ALERT_MESSAGE[lvl] ?? 'Peringatan',
              created_at: new Date().toISOString(),
            },
            ...prev,
          ].slice(0, 10));

          // Notifikasi Telegram otomatis ditangani langsung oleh firmware RX (24/7)
          // agar tetap terkirim saat web tidak dibuka dan tidak terjadi pesan ganda.
        }
      }
    };

    const handleError = (err: Error) => {
      console.error('[Home] ❌ MQTT Error:', err.message);
    };

    // Attach event handlers
    client.on('connect', handleConnect);
    client.on('disconnect', handleDisconnect);
    client.on('offline', handleOffline);
    client.on('message', handleMessage);
    client.on('error', handleError);

    // If already connected, trigger connect manually
    if (client.connected) {
      handleConnect();
    }

    return () => {
      // Remove event handlers only — don't kill the singleton!
      client.off('connect', handleConnect);
      client.off('disconnect', handleDisconnect);
      client.off('offline', handleOffline);
      client.off('message', handleMessage);
      client.off('error', handleError);
      mqttRef.current = null;
    };
  }, []);

  // Supabase polling — cadangan saat MQTT tidak mengirim data.
  // Dulu syaratnya `if (!IS_DEMO || !SUPABASE_ENABLED) return;`, sehingga
  // polling hanya hidup di mode demo — padahal mode demo justru mematikan MQTT.
  // Sekarang polling berjalan bila Supabase terisi, tetapi DIABAIKAN selama
  // data MQTT masih segar agar tidak menimpa data real-time dengan baris lama.
  useEffect(() => {
    if (!SUPABASE_ENABLED) return;

    let cancelled = false;
    let timer: ReturnType<typeof setInterval> | null = null;

    async function poll() {
      try {
        const mqttFresh = lastMqttAtRef.current != null
          && Date.now() - lastMqttAtRef.current < RX_FRESH_MS;
        if (mqttFresh) return;   // MQTT mengalir → Supabase tidak dipakai

        const latest = await fetchLatestSupabase();
        if (latest && !cancelled) {
          const stale = Date.now() - latest.timestamp > STALE_MS;
          setData(latest);
          setLastUpdate(formatTimeWIB(new Date(latest.timestamp)));
          setConnected(!stale);
          setLastDataAt(stale ? null : Date.now());
        } else if (!cancelled) {
          setConnected(false);
          setLastDataAt(null);
        }
        const al = await fetchAlertsSupabase();
        if (!cancelled) { setAlerts(al); }
      } catch {
        if (!cancelled) { setConnected(false); setLastDataAt(null); }
      }
    }

    poll();
    timer = setInterval(poll, POLL_MS);
    return () => { cancelled = true; if (timer) clearInterval(timer); };
  }, []);

  const level = data.level;
  const theme = LEVEL_THEME[level];

  // ── Kehadiran RX ──────────────────────────────────────────────────
  // LIVE hanya bila broker terhubung DAN ada data RX yang masih segar.
  // Ini mencegah badge menyala saat browser tersambung ke broker tetapi RX
  // (atau jalur LoRa-nya) mati — kondisi yang dulu terlihat seperti "sistem
  // jalan padahal TX→MQTT langsung".
  const dataAgeMs = lastDataAt == null || nowMs === 0 ? Infinity : Math.max(0, nowMs - lastDataAt);
  const linkState: 'live' | 'waiting' | 'offline' =
    !connected ? 'offline' : dataAgeMs < RX_FRESH_MS ? 'live' : 'waiting';

  // BAHAYA: getarkan HP pengguna (Vibration API) saat diakses.
  // Catatan: API ini hanya berjalan di Android (Chrome/Edge) lewat HTTPS
  // dan umumnya butuh satu interaksi (tap) lebih dulu — jadi kita panggil
  // saat level bahaya muncul DAN saat interaksi pertama sebagai fallback.
  useEffect(() => {
    if (level !== 'bahaya') return;
    const nav = navigator as Navigator & { vibrate?: (pattern: number | number[]) => boolean };
    if (typeof nav.vibrate !== 'function') return;
    const pattern = [300, 100, 300, 100, 500];
    const tryVibrate = () => { try { nav.vibrate(pattern); } catch { /* no-op */ } };
    tryVibrate();
    const events = ['pointerdown', 'touchstart', 'click', 'keydown'] as const;
    events.forEach((e) => window.addEventListener(e, tryVibrate, { once: true, passive: true }));
    return () => events.forEach((e) => window.removeEventListener(e, tryVibrate));
  }, [level]);

  return (
    <main
      className="flex flex-col min-h-screen text-white relative overflow-hidden"
      style={{ background: `linear-gradient(180deg, ${theme.top} 0%, ${theme.bottom} 100%)` }}
    >
      {level === 'bahaya' && (
        <div className="pulse-bg pointer-events-none absolute inset-0 bg-[#ff1744]" aria-hidden />
      )}
      <header className="bg-black/15 backdrop-blur border-b border-white/20 px-4 py-2.5 sticky top-0 z-40">
        <div className="flex items-center justify-between gap-3 max-w-lg mx-auto">
          <div className="flex items-center gap-2.5 min-w-0">
            <span className="text-2xl leading-none">🏔</span>
            <div className="min-w-0 overflow-hidden">
              <h1 className="text-xl font-black text-white leading-tight tracking-tight">LADET</h1>
              <p className="marquee text-xs text-white/80 font-medium leading-tight">
                Sistem Deteksi Longsor Desa Klungkung&nbsp;&nbsp;·&nbsp;&nbsp;Sistem Deteksi Longsor Desa Klungkung
              </p>
            </div>
          </div>
          <div className="flex items-center gap-2 shrink-0">
            <span className="inline-flex items-center gap-1 text-[10px] font-bold px-2 py-1 rounded-full bg-white/25 text-white tabular-nums">
              🕐 {clock.slice(0, 5)} WIB
            </span>
            <Link href="/login"
              className="inline-flex items-center text-[10px] font-bold px-3 py-1 rounded-full bg-white/25 text-white hover:bg-white/40 transition-colors">
              🔒 Engineer
            </Link>
          </div>
        </div>
      </header>

      <div className="flex-1 px-4 py-4 flex flex-col max-w-lg mx-auto w-full gap-4">
        <section
          className={`flex-1 backdrop-blur-md border border-white/15 shadow-2xl flex flex-col gap-4 text-white${level === 'bahaya' ? ' danger-blink' : ''}`}
          style={{ background: 'rgba(0,0,0,0.20)', borderRadius: '16px', padding: '20px 24px' }}
        >
          <div className="flex items-start gap-4">
            <span className="text-6xl leading-none drop-shadow shrink-0" style={{ color: theme.accent }}>{LEVEL_ICON[level]}</span>
            <div className="flex-1 min-w-0">
              <div className="flex items-center justify-between gap-2">
                <p className="text-xs font-bold uppercase tracking-widest text-white/70">Status Peringatan</p>
                <div
                  className={`inline-flex items-center gap-1.5 border rounded-full px-2.5 py-1 shrink-0 backdrop-blur ${linkState === 'live'
                      ? 'bg-white/15 border-white/20'
                      : linkState === 'waiting'
                        ? 'bg-amber-400/30 border-amber-200/50'
                        : 'bg-white/10 border-white/15'
                    }`}
                  style={{ flexShrink: 0 }}
                  title={
                    linkState === 'live'
                      ? `Broker terhubung · data RX terakhir ${Math.round(dataAgeMs / 1000)} dtk lalu`
                      : linkState === 'waiting'
                        ? 'Broker terhubung, tetapi belum ada data baru dari RX — periksa gateway RX / jalur LoRa'
                        : 'Tidak terhubung ke broker MQTT'
                  }
                >
                  {linkState === 'live' ? (
                    <>
                      <span className="animate-blink text-[11px] leading-none text-white">●</span>
                      <span className="text-[10px] font-bold text-white/90">LIVE</span>
                    </>
                  ) : linkState === 'waiting' ? (
                    <>
                      <span className="text-[11px] leading-none text-amber-100 animate-blink">◐</span>
                      <span className="text-[10px] font-bold text-amber-50">MENUNGGU RX</span>
                    </>
                  ) : (
                    <>
                      <span className="text-[11px] leading-none text-white/70">○</span>
                      <span className="text-[10px] font-bold text-white/70">OFFLINE</span>
                    </>
                  )}
                </div>
              </div>
              <p className="text-2xl sm:text-3xl md:text-4xl font-black leading-tight tracking-tight text-white drop-shadow whitespace-nowrap">{LEVEL_TITLE[level]}</p>
            </div>
          </div>

          <p className="text-lg font-semibold leading-snug text-white/90">{LEVEL_ACTION[level]}</p>

          <p className="text-[11px] font-medium text-white/60 tabular-nums leading-tight">
            {linkState === 'live'
              ? `Data terakhir dari gateway RX: ${lastUpdate} WIB`
              : linkState === 'waiting'
                ? 'Menunggu data baru dari gateway RX…'
                : 'Belum ada koneksi ke server pemantauan'}
          </p>

          <div className="grid grid-cols-3 gap-2.5 mt-auto">
            <div className="bg-white/10 rounded-2xl p-4 flex flex-col items-center text-center gap-1.5 border border-white/15 backdrop-blur-sm" style={{ width: '100%', minWidth: 0, overflow: 'hidden' }}>
              <span className="text-3xl">💧</span>
              <p className="text-[10px] font-semibold uppercase tracking-wide text-white/60 leading-tight">Kelembapan</p>
              <p className="sensor-pulse text-[18px] font-black leading-none" style={{ color: theme.accent }}>{sensorWord('moisture', data.soil_saturation)}</p>
              <p className="text-sm font-medium text-white/70 tabular-nums">{data.soil_saturation.toFixed(1)}%</p>
            </div>
            <div className="bg-white/10 rounded-2xl p-4 flex flex-col items-center text-center gap-1.5 border border-white/15 backdrop-blur-sm" style={{ width: '100%', minWidth: 0, overflow: 'hidden' }}>
              <span className="text-3xl">📳</span>
              <p className="text-[10px] font-semibold uppercase tracking-wide text-white/60 leading-tight">Getaran</p>
              <p className="sensor-pulse text-[18px] font-black leading-none" style={{ color: theme.accent }}>{sensorWord('vibration', data.vibration_freq)}</p>
              <p className="text-sm font-medium text-white/70 tabular-nums">{data.vibration_freq < 1 ? data.vibration_freq.toFixed(3) : data.vibration_freq.toFixed(2)} Δg</p>
            </div>
            <div className="bg-white/10 rounded-2xl p-4 flex flex-col items-center text-center gap-1.5 border border-white/15 backdrop-blur-sm" style={{ width: '100%', minWidth: 0, overflow: 'hidden' }}>
              <span className="text-3xl">📐</span>
              <p className="text-[10px] font-semibold uppercase tracking-wide text-white/60 leading-tight">Kemiringan</p>
              <p className="sensor-pulse text-[18px] font-black leading-none" style={{ color: theme.accent }}>{sensorWord('tilt', data.tilt_angle)}</p>
              <p className="text-sm font-medium text-white/70 tabular-nums">{data.tilt_angle.toFixed(2)}°</p>
            </div>
          </div>
        </section>



        <a
          href="https://t.me/ladetklungkung"
          target="_blank"
          rel="noopener noreferrer"
          className="w-full flex items-center gap-3 text-left text-white transition-colors backdrop-blur-md border shadow-lg"
          style={{ background: 'rgba(0,136,204,0.14)', borderColor: 'rgba(0,136,204,0.45)', borderRadius: '14px', padding: '14px 16px', cursor: 'pointer' }}
          onMouseEnter={(e) => (e.currentTarget.style.background = 'rgba(0,136,204,0.24)')}
          onMouseLeave={(e) => (e.currentTarget.style.background = 'rgba(0,136,204,0.14)')}
        >
          <svg viewBox="0 0 24 24" width="22" height="22" fill="#0088cc" aria-hidden="true">
            <path d="M9.78 18.65l.28-4.23 7.68-6.92c.34-.31-.07-.46-.52-.19L7.74 13.3 3.64 12c-.88-.25-.89-.86.2-1.3l13.78-5.33c.78-.32 1.46.18 1.26 1.34l-2.36 11.07c-.18.81-.66 1.01-1.34.63l-3.66-2.7-1.76 1.69c-.2.2-.36.36-.74.36z" />
          </svg>
          <span className="flex-1 min-w-0">
            <span className="block text-sm font-bold leading-tight">Gabung Channel LADET</span>
            <span className="block text-[11px] text-white/80 leading-tight">Terima notifikasi peringatan langsung di Telegram</span>
          </span>
          <span className="shrink-0 inline-flex items-center gap-1 rounded-full bg-white/35 px-3 py-1.5 text-[11px] font-black text-white shadow animate-pulse" aria-hidden>
            👆 KLIK
          </span>
        </a>

        {/* Riwayat peringatan */}
        <section
          className="backdrop-blur-md border border-white/15 shadow-2xl text-white"
          style={{ background: 'rgba(0,0,0,0.20)', borderRadius: '16px', padding: '16px 18px' }}
        >
          <p className="text-xs font-bold uppercase tracking-widest text-white/70 mb-3">Riwayat Peringatan</p>
          {alerts.length === 0 ? (
            <p className="text-xs text-white/50 py-4 text-center">Tidak ada peringatan</p>
          ) : (
            <>
              <ul className="flex flex-col gap-2">
                {alerts.slice(0, 3).map((a, i) => (
                  <li key={i} className="flex items-center gap-3 bg-white/10 rounded-xl px-3 py-2 border border-white/10" style={{ borderLeft: `3px solid ${LEVEL_ACCENT[a.level]}` }}>
                    <span className="text-lg leading-none" style={{ color: LEVEL_ACCENT[a.level] }}>{LEVEL_ICON[a.level]}</span>
                    <div className="flex-1 min-w-0">
                      <p className="text-sm font-bold leading-tight text-white">
                        {LEVEL_TITLE[a.level]}
                      </p>
                      <p className="text-[11px] text-white/70 leading-tight truncate">{a.message}</p>
                    </div>
                    <span className="text-[10px] text-white/50 font-mono shrink-0 whitespace-nowrap">
                      {formatTimeWIB(a.created_at)}
                    </span>
                  </li>
                ))}
              </ul>
              {alerts.length > 3 && (
                <Link href="/riwayat" className="mt-1 flex items-center justify-center gap-1.5 text-[11px] font-semibold text-white/80 hover:text-white transition-colors">
                  Lihat semua riwayat →
                </Link>
              )}
            </>
          )}
        </section>



        <p className="text-center text-[10px] text-white/70 pt-1">
          LADET — Landslide Early Detector Tool<br />
          Teknik Elektro · Universitas Jember
        </p>
      </div>
    </main>
  );
}

