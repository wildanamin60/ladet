'use client';

import { useEffect, useRef, useState, useCallback } from 'react';
import QoSCard from '@/components/QoSCard';
import RxHealthCard from '@/components/RxHealthCard';
import SensorCard from '@/components/SensorCard';
import RealtimeChart from '@/components/RealtimeChart';
import PacketTable from '@/components/PacketTable';
import ScenarioSelector from '@/components/ScenarioSelector';
import type { SF } from '@/components/ScenarioSelector';
import { formatTimeWIB, calculatePacketLoss } from '@/lib/calculations';
import { normalizeLevel, soilLevel, vibrationLevel, tiltLevel, applyCalibration } from '@/lib/thresholds';
import { generateDemoPayload, generateDemoHistory, resetDemoSequence } from '@/lib/demo';
import { getMqttClient, parsePayload, publishCmd, publishPublicMode, DEFAULT_MQTT_CONFIG, LADET_TOPICS, CONFIG_STATUS_TOPIC, parseConfigStatus, type ConfigStatus, RX_ONLINE_TOPIC, TX_ONLINE_TOPIC, PUBLIC_MODE_TOPIC, parsePublicMode, RX_STATUS_TOPIC, parseRxHealth, type RxHealth } from '@/lib/mqtt';
import { insertSensorData } from '@/lib/supabase';
import type { LadetPayload } from '@/lib/mqtt';
import type { SensorData } from '@/lib/supabase';
import type { ChartPoint } from '@/components/RealtimeChart';
import type { MqttClient } from 'mqtt';

const IS_DEMO = process.env.NEXT_PUBLIC_DEMO_MODE === 'true' || !process.env.NEXT_PUBLIC_MQTT_BROKER_URL;
const MAX_CHART = 50;

const LEVEL_BG: Record<string, string> = {
  aman: 'bg-green-500', 'hati-hati': 'bg-yellow-400',
  waspada: 'bg-orange-500', bahaya: 'bg-red-600',
};
const LEVEL_TEXT: Record<string, string> = {
  aman: 'text-white', 'hati-hati': 'text-yellow-900',
  waspada: 'text-white', bahaya: 'text-white',
};
const LEVEL_ICON: Record<string, string> = {
  aman: '✅', 'hati-hati': '⚠️', waspada: '‼️', bahaya: '🚨',
};
const LEVEL_LABEL: Record<string, string> = {
  aman: 'AMAN', 'hati-hati': 'HATI-HATI', waspada: 'WASPADA', bahaya: 'BAHAYA',
};

function emptyPayload(): LadetPayload {
  return {
    node_id: 'LADET-NODE-01', timestamp: Date.now(),
    soil_saturation: 0, vibration_freq: 0, tilt_angle: 0,
    rssi: 0, snr: 0, delay_ms: 0, packet_loss: 0, packet_id: 0,
    spreading_factor: 'SF9', distance_m: 100, level: 'aman',
  };
}

export default function DashboardPage() {
  const [live, setLive]             = useState<LadetPayload>(emptyPayload());
  const [chartData, setChartData]   = useState<ChartPoint[]>([]);
  const [tableData, setTableData]   = useState<SensorData[]>([]);
  const [connected, setConnected]   = useState(false);
  const [demo, setDemo]             = useState(IS_DEMO);
  const [lastUpdate, setLastUpdate] = useState('—');
  const [sf, setSF]                 = useState<SF>('SF9');
  const [distanceM, setDistanceM]   = useState(100);
  const [packetSent, setPacketSent] = useState(0);
  const [packetRecv, setPacketRecv] = useState(0);
  const [publicTesting, setPublicTesting] = useState(false);
  const [publicModeUntil, setPublicModeUntil] = useState<number | null>(null);
  const [engineerSession, setEngineerSession] = useState(false);
  // Konfirmasi config remote dari RX (ladet/config/status): SF/jarak yang
  // benar-benar aktif di TX+RX. null = belum ada info (RX offline/lama).
  const [configStatus, setConfigStatus] = useState<ConfigStatus | null>(null);
  // Kehadiran perangkat ASLI (bukan sekadar status browser↔broker):
  // RX via LWT/retained 'ladet/rx/online', TX via paket LoRa yang diterima RX.
  const [rxOnline, setRxOnline] = useState<boolean | null>(null);
  const [txOnline, setTxOnline] = useState<boolean | null>(null);
  // Timeout anti-pesan-BASI: pesan 'online' retained/LWT dari sesi lama bisa
  // menempel di broker BERJAM-JAM setelah perangkat mati. Setiap bukti hidup
  // (online/data/config-status) me-reset timer ini; kalau 35 dtk tidak ada
  // bukti baru → perangkat dianggap mati, walau pesan 'online' basi masih ada.
  const [rxSeenAt, setRxSeenAt] = useState<number | null>(null);
  const [txSeenAt, setTxSeenAt] = useState<number | null>(null);
  // Diagnostik gateway RX (ladet/rx/status): reset terakhir, boot count, uptime,
  // heap, RSSI. Bukan data sensor — hanya kesehatan perangkat.
  const [rxHealth, setRxHealth] = useState<RxHealth | null>(null);
  const [rxHealthAt, setRxHealthAt] = useState<number | null>(null);
  // rxSeenAt/txSeenAt di state → memicu re-render tiap bukti hidup; timer di
  // bawah memaksa re-render tiap detik supaya timeout basi tampil tepat waktu.
  const [, setNowTick] = useState(0);
  // Jam browser disimpan di state (diisi timer), BUKAN Date.now() saat render —
  // supaya render tetap murni dan tidak memicu peringatan lint.
  const [nowMs, setNowMs] = useState(0);
  useEffect(() => {
    if (demo) return;
    const t = setInterval(() => {
      setNowTick(n => n + 1);
      setNowMs(Date.now());
    }, 1000);
    return () => clearInterval(t);
  }, [demo]);
  const PRESENCE_TIMEOUT_MS = 35000;
  const rxFresh = rxSeenAt != null && Date.now() - rxSeenAt < PRESENCE_TIMEOUT_MS;
  const txFresh = txSeenAt != null && Date.now() - txSeenAt < PRESENCE_TIMEOUT_MS;
  // Status EFEKTIF: mentah dari broker DAN masih segar. Inilah yang dipakai
  // semua indikator — bukan rxOnline/txOnline mentah.
  const rxOnlineEff = rxOnline === true && rxFresh;
  const txOnlineEff = txOnline === true && txFresh;
  // Evidence timestamps are kept per device. Do not use one shared data
  // timestamp here: RX MQTT heartbeats and TX LoRa evidence have different
  // sources and an event from one device must not mask the other device's LWT.
  const rxEvidenceAtRef = useRef<number | null>(null);
  const txEvidenceAtRef = useRef<number | null>(null);
  useEffect(() => {
    if (!publicTesting || !publicModeUntil) return;
    const remaining = publicModeUntil - Date.now();
    if (remaining <= 0) {
      if (mqttRef.current?.connected) {
        publishCmd(mqttRef.current, 'MONITOR');
        publishPublicMode(mqttRef.current, false);
      }
      setEngineerSession(false); setPublicTesting(false); setPublicModeUntil(null); return;
    }
    const timer = setTimeout(() => {
      if (mqttRef.current?.connected) {
        publishCmd(mqttRef.current, 'MONITOR');
        publishPublicMode(mqttRef.current, false);
      }
      setEngineerSession(false);
      setPublicTesting(false);
      setPublicModeUntil(null);
    }, remaining);
    return () => clearTimeout(timer);
  }, [publicTesting, publicModeUntil]);
  const demoTimer = useRef<ReturnType<typeof setInterval> | null>(null);
  const mqttRef   = useRef<MqttClient | null>(null);
  const sentRef   = useRef(0);
  const recvRef   = useRef(0);
  const lastDedupeKeyRef = useRef('');   // dedupe: kunci paket terakhir yang diproses
  const lastDataAtRef = useRef(0);       // waktu (ms) paket data terakhir — penentu "data segar"
  // ── Khusus ADMIN/engineer (pengambilan data skripsi, TIDAK menyentuh
  // halaman warga di app/page.tsx yang murni realtime) ──
  // Grafik & tabel hanya menampilkan #1..#50 yang rill (1 nomor = 1 titik).
  // Fase pemanasan TX ("pengujian baru" berulang saat belum stabil) bisa
  // mengirim #1 ~20× sebelum run stabil #1→#50 — pengulangan itu diabakan
  // untuk chart/tabel, tapi TETAP disimpan di arsip CSV & Supabase.
  // Arsip SEMUA paket utk CSV skripsi (termasuk #1 pemanasan yg berulang)
  // Baris arsip CSV skripsi = SensorData + kolom tambahan skripsi
  interface ExportRow extends SensorData {
    tx_time?: string;      // waktu kirim versi TX (= RX terima − delay; BUKAN RTC mentah)
    tx_rtt_ms?: number;    // RTT paket sebelumnya (ms) — 1 jam akurat, tanpa RTC
    loss_ratio?: string;   // "terima/kirim" kumulatif, mis. 16/16
    theta_measured?: number;
    theta_r?: number;
    theta_s?: number;
    g_initial?: number;
    g_measured?: number;
    delta_g?: number;
    tilt_reference?: number;
    tilt_measured?: number;
  }
  const exportLogRef = useRef<ExportRow[]>([]);

  const resetRunView = useCallback(() => {
    setChartData([]); setTableData([]); setPacketSent(0); setPacketRecv(0);
    sentRef.current = 0; recvRef.current = 0; lastDedupeKeyRef.current = '';
    exportLogRef.current = [];
    setLive(emptyPayload()); setLastUpdate('—');
  }, []);

  const applyPayload = useCallback((p: LadetPayload) => {
    if (!demo && !engineerSession) return;
    // ── PAKET #0 = KALIBRASI (bukan data) ──
    // Firmware TX mengirim SEQ=0 paling awal tiap run — RX memakainya hanya
    // untuk mengunci kalibrasi delay. Paket #0 TIDAK masuk grafik, tabel,
    // arsip CSV, maupun Supabase; data rill dimulai dari #1.
    if (p.packet_id === 0) return;

    // ── arsip CSV skripsi: catat SEMUA paket apa adanya ──
    // Termasuk #1 pemanasan yang berulang (~20×) sebelum run stabil #1→#50.
    // buildExportRow dipakai sebelum guard di bawah supaya tidak ada yang lolos.
    const archiveRow = (pp: LadetPayload): ExportRow => ({
      id: Date.now() + Math.random(), created_at: new Date().toISOString(),
      node_id: pp.node_id,
      soil_saturation: pp.soil_saturation,
      vibration_freq: pp.vibration_freq,
      tilt_angle: pp.tilt_angle,
      rssi: pp.rssi, snr: pp.snr, delay_ms: pp.delay_ms,
      packet_id: pp.packet_id, is_received: true,
      status_level: pp.level,
      spreading_factor: pp.spreading_factor,
      distance_m: pp.distance_m,
      // kolom tambahan skripsi (opsional — undefined bila firmware lama/demo):
      tx_time: pp.tx_time,
      tx_rtt_ms: pp.tx_rtt_ms,
      loss_ratio: (pp.total_expected != null && pp.total_received != null)
        ? `${pp.total_received}/${pp.total_expected}` : undefined,
      theta_measured: pp.theta_measured,
      theta_r: pp.theta_r,
      theta_s: pp.theta_s,
      g_initial: pp.g_initial,
      g_measured: pp.g_measured,
      delta_g: pp.delta_g,
      tilt_reference: pp.tilt_angle_reference,
      tilt_measured: pp.tilt_angle_measured,
    });
    exportLogRef.current = [...exportLogRef.current, archiveRow(p)].slice(-500);

    // Ambang kalibrasi dari halaman Pengaturan: hanya bisa menaikkan level,
    // status dasar tetap dari firmware TX (lihat lib/thresholds.ts).
    setLive({
      ...p,
      level: applyCalibration(p.level, p.soil_saturation, p.vibration_freq, p.tilt_angle),
    });
    setLastUpdate(formatTimeWIB(new Date()));

    // ── Dedupe: 1 paket LoRa = 1 baris data ──
    // RX mem-publish satu payload lengkap per paket (topic 'ladet/data').
    // Kunci anti-duplikat TIDAK boleh hanya packet_id: kalau firmware TX yang
    // terpasang masih mengirim SEQ yang sama terus (mis. selalu SEQ=1), semua
    // paket berikutnya dianggap duplikat → tabel & grafik berhenti di 1 data
    // ("data cuma 1"). Jadi:
    //   1) pakai rx_count dari RX (selalu naik) kalau tersedia, atau
    //   2) bandingkan seluruh isi payload (nilai sensor ikut berubah tiap paket).
    const dedupeKey = p.rx_count != null && p.rx_count > 0
      ? `rx#${p.rx_count}`
      : [p.packet_id, p.soil_saturation, p.vibration_freq, p.tilt_angle,
         p.rssi, p.snr, p.delay_ms].join('|');
    if (dedupeKey === lastDedupeKeyRef.current) return;
    lastDedupeKeyRef.current = dedupeKey;

    // Deteksi RUN BARU: packet_id balik ke 1 padahal #2..#50 sudah tampil.
    // Fase pemanasan TX (pengujian baru berulang) mengirim #1 berkali-kali
    // lalu run stabil #1->#50. Saat run baru dimulai, grafik dan tabel
    // dikosongkan dulu supaya isinya selalu #1..#50 yang rill.
    // Arsip CSV (exportLogRef) TIDAK dikosongkan: semua paket tetap tersimpan.
    setChartData(function (prev) {
      var maxShown = prev.length ? Math.max.apply(null, prev.map(function (q) { return q.packet_id || 0; })) : 0;
      return (p.packet_id === 1 && maxShown > 1) ? [] : prev;
    });
    setTableData(function (prev) {
      var maxShown = prev.length ? Math.max.apply(null, prev.map(function (r) { return r.packet_id || 0; })) : 0;
      return (p.packet_id === 1 && maxShown > 1) ? [] : prev;
    });

    // ── Guard delay tidak wajar ──
    // Delay > 5 s mustahil untuk LoRa (SF12 max airtime ≈ 2 s). Data seperti
    // itu (mis. dari bug offset jam TX↔RX) dibuang dari grafik & tabel supaya
    // tidak merusak rata-rata, tapi tetap tidak dihitung sebagai packet loss.
    const delayValid = p.delay_source !== 'invalid'
      && p.delay_ms >= 0 && p.delay_ms < 5000;

    // ── Packet loss & counter Terkirim/Diterima ──
    // Grafik loss: pakai nilai KUMULATIF dari RX firmware (deteksi gap SEQ)
    // kalau ada; fallback heuristik client-side untuk firmware lama/demo.
    // PENTING: packet_loss itu KUMULATIF — bukan penanda "paket ini hilang".
    const rxTracks = p.total_expected != null && p.total_received != null;
    const pl = (p.packet_loss > 0 || rxTracks)
      ? p.packet_loss
      : (() => {
          const isLost = p.rssi <= -120 || p.delay_ms >= 9999;
          sentRef.current += 1;
          if (!isLost) recvRef.current += 1;
          return calculatePacketLoss(sentRef.current, recvRef.current);
        })();

    // Counter TERKIRIM/DITERIMA: sumber kebenaran = tracking RX. Payload MQTT
    // hanya dikirim RX untuk paket yang DITERIMA, jadi counter lokal tidak
    // mungkin tahu paket yang hilang di udara — RX menghitungnya lewat gap SEQ.
    // (Bug lama: `packet_loss > 0` dipakai seolah per-paket, padahal kumulatif
    //  → DITERIMA membeku begitu loss pertama terjadi: 11/49 → loss 77.6%.)
    if (rxTracks) {
      setPacketSent(p.total_expected!);
      setPacketRecv(p.total_received!);
    } else {
      // Firmware lama/demo: naikkan per payload; paket "hilang" (rssi −120)
      // tidak menambah DITERIMA.
      setPacketSent(n => n + 1);
      setPacketRecv(n => (p.rssi > -120 ? n + 1 : n));
    }
    if (!delayValid) return;
    setChartData(prev => {
      const pt: ChartPoint = {
        time: formatTimeWIB(new Date()),
        // Label grafik: nomor SEQ dari TX. Kalau SEQ TX macet (firmware lama),
        // pakai nomor terima RX supaya tiap titik tetap bisa dibedakan.
        label: p.seq_stuck && p.rx_count
          ? `RX#${p.rx_count}`
          : p.packet_id > 0 ? `#${p.packet_id}` : formatTimeWIB(new Date()),
        packet_id: p.packet_id,
        rssi: p.rssi, snr: p.snr, delay: p.delay_ms,
        soil_moisture: p.soil_saturation,
        vibration: p.vibration_freq,
        tilt_angle: p.tilt_angle,
        packet_loss: pl,
      };
      // Nomor paket yang SAMA (mis. #1 pemanasan yang datang ~20x lalu #1
      // stabil) menimpa titik yang sudah ada — jadi grafik delay tepat berisi
      // #1..#50 yang rill/terbaru, bukan 50 + 20 sisa pemanasan.
      const idx = prev.findIndex(q => q.packet_id === p.packet_id && p.packet_id > 0);
      if (idx >= 0) {
        const next = [...prev];
        next[idx] = pt;
        return next;
      }
      const next = [...prev, pt];
      return next.length > MAX_CHART ? next.slice(-MAX_CHART) : next;
    });
    setTableData(prev => {
      const row: SensorData = {
        id: Date.now(), created_at: new Date().toISOString(),
        node_id: p.node_id,
        soil_saturation: p.soil_saturation,
        vibration_freq: p.vibration_freq,
        tilt_angle: p.tilt_angle,
        rssi: p.rssi, snr: p.snr, delay_ms: p.delay_ms,
        packet_id: p.packet_id, is_received: true,
        status_level: p.level,
        spreading_factor: p.spreading_factor,
        distance_m: p.distance_m,
      };
      // #1 pemanasan yang berulang (~20x) menimpa baris #1 yang sudah ada —
      // jadi tabel berisi #1..#50 yang rill/terbaru. Arsip CSV (exportLogRef)
      // TIDAK ikut ditimpa: semua paket tetap tersimpan untuk spreadsheet.
      const idx = prev.findIndex(r => r.packet_id === p.packet_id && p.packet_id > 0);
      if (idx >= 0) {
        const next = [...prev];
        next[idx] = row;
        return next;
      }
      return [row, ...prev].slice(0, 100);
    });
    // Notifikasi Telegram TIDAK lagi dikirim dari dashboard.
    // TX → LoRa → RX, dan RX yang punya internet + relay: di sana notifikasi
    // dikirim 24 jam walau halaman ini ditutup, dan tetap jalan saat offline
    // (diAntri lalu dikirim begitu jaringan pulih). Lihat firmware/ladet-rx-mqtt.
    // Tombol tes manual tetap ada di halaman Pengaturan.
    if (process.env.NEXT_PUBLIC_SUPABASE_URL) {
      insertSensorData({
        node_id: p.node_id,
        soil_saturation: p.soil_saturation,
        vibration_freq: p.vibration_freq,
        tilt_angle: p.tilt_angle,
        rssi: p.rssi, snr: p.snr, delay_ms: p.delay_ms,
        packet_id: p.packet_id, is_received: true,
        status_level: p.level,
        spreading_factor: p.spreading_factor,
        distance_m: p.distance_m,
      });
    }
  }, [engineerSession]);

  useEffect(() => {
    if (!demo) return;
    setConnected(true);
    setTableData(generateDemoHistory(30, sf, distanceM));
    setPacketSent(0); setPacketRecv(0); sentRef.current = 0; recvRef.current = 0;
    lastDedupeKeyRef.current = '';
    resetDemoSequence();
    const tick = () => applyPayload(generateDemoPayload(sf, distanceM));
    tick();
    demoTimer.current = setInterval(tick, 3000);
    return () => { if (demoTimer.current) clearInterval(demoTimer.current); };
  }, [demo, sf, distanceM, applyPayload]);

  useEffect(() => {
    if (demo) return;
    
    const client = getMqttClient(DEFAULT_MQTT_CONFIG);
    mqttRef.current = client;
    
    const handleConnect = () => {
      console.log('[Dashboard] ✅ MQTT Connected');
      setConnected(true);
      LADET_TOPICS.forEach(t => client.subscribe(t));
      client.subscribe(PUBLIC_MODE_TOPIC);
      // Config remote: status konfirmasi SF/jarak dari RX firmware
      client.subscribe(CONFIG_STATUS_TOPIC);
      // Kehadiran perangkat (retained → status terakhir langsung masuk)
      client.subscribe(RX_ONLINE_TOPIC);
      client.subscribe(TX_ONLINE_TOPIC);
      // Diagnostik gateway (reset terakhir, boot count, uptime, heap, RSSI)
      client.subscribe(RX_STATUS_TOPIC);
    };
    
    const handleDisconnect = () => {
      console.log('[Dashboard] ⚠️ MQTT Disconnected');
      setConnected(false);
    };
    
    const handleOffline = () => {
      console.log('[Dashboard] 📴 MQTT Offline');
      setConnected(false);
    };
    
    const handleMessage = (topic: string, msg: Buffer) => {
      // Diagnostik gateway RX — bukan data sensor. Jangan diteruskan ke pipeline
      // payload (parsePayload akan menolaknya karena topik bukan `ladet/data`).
      if (topic === RX_STATUS_TOPIC) {
        const health = parseRxHealth(msg);
        if (health) {
          setRxHealth(health);
          setRxHealthAt(Date.now());
        }
        return;
      }
      if (topic === PUBLIC_MODE_TOPIC) {
        const mode = parsePublicMode(msg);
        const active = !!mode?.testing && (!mode.until || mode.until > Date.now());
        setPublicTesting(active);
        setPublicModeUntil(active ? (mode?.until ?? null) : null);
        return;
      }
      // Status config remote (dari RX firmware) → bukti RX HIDUP (mustahil
      // mengirim ini kalau RX mati), sekaligus update indikator konfigurasi.
      if (topic === CONFIG_STATUS_TOPIC) {
        setConfigStatus(parseConfigStatus(msg));
        rxEvidenceAtRef.current = Date.now();
        setRxOnline(true);
        setRxSeenAt(Date.now());
        return;
      }
      // Kehadiran perangkat: "online"/"offline" (retained / LWT / heartbeat RX)
      if (topic === RX_ONLINE_TOPIC || topic === TX_ONLINE_TOPIC) {
        const online = msg.toString().trim() === 'online';
        const now = Date.now();
        const evidenceRef = topic === RX_ONLINE_TOPIC ? rxEvidenceAtRef : txEvidenceAtRef;
        const evidenceFresh = evidenceRef.current != null && now - evidenceRef.current < PRESENCE_TIMEOUT_MS;
        if (online) {
          // Bukti hidup LANGSUNG (me-reset timer anti-basi 35 dtk di bawah)
          evidenceRef.current = now;
          if (topic === RX_ONLINE_TOPIC) { setRxOnline(true); setRxSeenAt(now); }
          else { setTxOnline(true); setTxSeenAt(now); }
        } else if (!evidenceFresh) {
          if (topic === RX_ONLINE_TOPIC) setRxOnline(false);
          else setTxOnline(false);
        }
        return;
      }
      // ── DATA MENGIN = BUKTI HIDUP ──
      // Paket data hanya bisa dipublish oleh RX yang menerima LoRa dari TX.
      // Ini mengesampingkan pesan retained 'offline' basi yang tertinggal di
      // broker (LWT dari sesi sebelumnya) — status mengikuti kenyataan.
      if (topic !== 'ladet/data') return;
      const dataAt = Date.now();
      lastDataAtRef.current = dataAt;
      rxEvidenceAtRef.current = dataAt;
      txEvidenceAtRef.current = dataAt;
      setRxOnline(true);
      setRxSeenAt(dataAt);
      setTxOnline(true);
      setTxSeenAt(dataAt);
      const parsed = parsePayload(topic, msg);
      if (!parsed) return;
      if (!demo && !engineerSession) return;
      console.log('[Dashboard] 📨 Message received:', topic, parsed);
      setLive(prev => { 
        // Status level dasar dari firmware TX (field `status` di payload RX).
        // Firmware sudah punya tupoksi per sensor — tanah → HATI-HATI,
        // getaran → WASPADA, kemiringan → BAHAYA — dan yang dikirim adalah
        // yang paling berat. Ambang di halaman Pengaturan hanya bisa
        // MENAIKKAN level (lihat applyCalibration), jadi tidak bisa
        // melunakkan status firmware; batas tupoksi juga tetap dijaga
        // sehingga tanah 100 % tidak pernah tampil BAHAYA.
        const u = { ...prev, ...parsed } as LadetPayload;
        u.level = normalizeLevel(u.level);
        applyPayload(u); 
        return u; 
      });
    };
    
    const handleError = (err: Error) => {
      console.error('[Dashboard] ❌ MQTT Error:', err.message);
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
  }, [demo, applyPayload]);

  useEffect(() => {
    const onBeforeUnload = () => {
      if (mqttRef.current?.connected) {
        publishCmd(mqttRef.current, 'MONITOR');
        publishPublicMode(mqttRef.current, false);
      }
    };
    window.addEventListener('beforeunload', onBeforeUnload);
    return () => window.removeEventListener('beforeunload', onBeforeUnload);
  }, []);

  function startEngineerRun() {
    if (!mqttRef.current?.connected) return;
    resetRunView();
    setEngineerSession(true);
    publishPublicMode(mqttRef.current, true);
    publishCmd(mqttRef.current, 'ENGINEER');
    window.setTimeout(() => {
      if (mqttRef.current?.connected) publishCmd(mqttRef.current, 'RUN');
    // RX menunggu ACK sampai 1.5 s; beri waktu agar ENGINEER selesai
    // diproses sebelum command RUN masuk dan menimpa queue RX.
    }, 1800);
  }

  function stopEngineerSession() {
    if (mqttRef.current?.connected) {
      publishCmd(mqttRef.current, 'MONITOR');
      publishPublicMode(mqttRef.current, false);
    }
    setEngineerSession(false);
  }

  function resetScenario(newSF: SF, newDist: number) {
    setSF(newSF); setDistanceM(newDist);
    setPacketSent(0); setPacketRecv(0); sentRef.current = 0; recvRef.current = 0;
    lastDedupeKeyRef.current = '';
    resetDemoSequence();
    resetRunView();
  }

  function exportCSV() {
    // Sumber: arsip SEMUA paket (exportLogRef), termasuk #1 pemanasan yang
    // berulang sebelum run stabil. Jadi kolom Paket memperlihatkan persis
    // paket ke berapa tiap baris (#1 × ~20, lalu #1..#50 rill) — data skripsi
    // lengkap, tinggal filter di spreadsheet bila perlu.
    const src = exportLogRef.current.length > 0 ? exportLogRef.current : tableData;
    // Kolom lengkap CSV skripsi (permintaan revisi): acuan & terukur tiap sensor,
    // tanggal & jam TERPISAH (RX terima dan TX kirim) supaya bisa diolah di Excel,
    // RTT paket sebelumnya, loss dalam bentuk terima/kirim.
    //
    // RUMUS WAKTU (penting utk Bab 4 — "t_terima − t_kirim = delay"):
    //   t_terima = jam laptop saat web menerima MQTT (created_at, ISO, WIB-referensi).
    //   t_kirim  = t_terima − delay_ms (delay yg sudah diukur RX, 1 jam akurat).
    // Keduanya dalam SATU basis (jam laptop) → selisihnya di Excel = delay_ms.
    // (RTC TX belum diset → tidak dipakai; tx_time uptime dari RX hanya cadangan.)
    //
    // PENTING utk Excel: sel teks dibungkus ="..." supaya "16/16" tidak dibaca
    // sebagai tanggal 16-Jan; tanggal & jam dipisah jadi 2 kolom (YYYY-MM-DD dan
    // HH:MM:SS.mmm) supaya gampang dikurangi (=C−A → format [h]:mm:ss.000).
    const ex = (v: unknown) => {
      if (v === undefined || v === null) return '""';
      const s = String(v).replace(/"/g, '""');
      return `="${s}"`;
    };
    const num = (v: unknown) => (v === undefined || v === null ? '' : String(v));
    // Pecah ISO "2026-09-22T10:02:29.363Z" → tanggal & jam terpisah.
    // Baris CSV memakai WIB (jam laptop, UTC+7) supaya cocok dengan jam dinding
    // saat pengujian — bukan UTC mentah (selisih 7 jam, mis. 10:36 vs 17:39).
    const WIB_OFFSET_MS = 7 * 3600 * 1000;
    const toWIB = (iso: string): string => {
      const t = Date.parse(iso);
      return isNaN(t) ? '' : new Date(t + WIB_OFFSET_MS).toISOString();
    };
    const splitISO = (iso: string): [string, string] => {
      const wib = iso.endsWith('Z') ? toWIB(iso) : iso;
      const m = wib.match(/^(\d{4}-\d{2}-\d{2})T(\d{2}:\d{2}:\d{2}(?:\.\d+)?)/);
      return m ? [m[1], m[2]] : [wib, ''];
    };
    // t_kirim = t_terima − delay (basis jam laptop yg sama) → ISO baru.
    const txISO = (rxIso: string, delayMs: unknown): string => {
      const t = Date.parse(rxIso);
      const d = typeof delayMs === 'number' ? delayMs : Number(delayMs);
      if (isNaN(t) || isNaN(d)) return '';
      return new Date(t - d).toISOString();
    };
    const h = ['Paket', 'SF', 'Jarak (m)', 'Tanggal (WIB)', 'Waktu TX kirim (t_kirim, WIB)', 'Waktu RX terima (t_terima, WIB)', 'Delay (ms) = t_terima - t_kirim', 'Loss (terima/kirim)',
      'RSSI', 'SNR',
      'Kelembapan terukur (m3/m3)', 'Saturasi (%)',
      'Kelembapan kering th_r (m3/m3)', 'Kelembapan jenuh th_s (m3/m3)',
      'Getaran acuan (g)', 'Getaran terukur (g)', 'Delta getaran (g)',
      'Sudut acuan (deg)', 'Sudut terukur (deg)', 'Delta sudut (deg)',
      'Status'].join(',');
    const rows = src.map(r => {
      const e = r as ExportRow;
      const rxIso = r.created_at ?? '';
      const txIso = txISO(rxIso, r.delay_ms);
      const [rxDate, rxTime] = splitISO(rxIso);
      const [txDate, txTime] = splitISO(txIso);
      // 1 kolom tanggal saja (tgl TX = tgl RX; run tidak pernah lewat tengah malam
      // utk 50 pkt × 2 dtk ≈ 100 dtk — kalau beda, yg dipakai tgl RX).
      const tanggal = rxDate || txDate;
      return [num(r.packet_id), ex(r.spreading_factor), num(r.distance_m), ex(tanggal), ex(txTime), ex(rxTime), num(r.delay_ms), ex(e.loss_ratio ?? ''),
        num(r.rssi), num(r.snr),
        num(e.theta_measured), num(r.soil_saturation),
        num(e.theta_r), num(e.theta_s),
        num(e.g_initial), num(e.g_measured), num(e.delta_g),
        num(e.tilt_reference), num(e.tilt_measured), num(r.tilt_angle),
        ex(r.status_level)].join(',');
    });
    const a = Object.assign(document.createElement('a'), {
      href: URL.createObjectURL(new Blob([[h, ...rows].join('\n')], { type: 'text/csv' })),
      download: `ladet-${Date.now()}.csv`,
    });
    a.click();
  }

  const level = live.level;
  // Prefer RX-side packet_loss (sequence-gap tracking)
  // Fallback: client-side heuristic for backward compatibility
  const packetLoss = live.packet_loss > 0
    ? live.packet_loss
    : calculatePacketLoss(packetSent, packetRecv);
  return (
    <main className="min-h-screen bg-gray-50 pb-8">
      <header className="bg-white border-b border-gray-200 px-4 py-3 sticky top-0 z-40 shadow-sm">
        <div className="flex items-center justify-between max-w-4xl mx-auto">
          <div>
            <h1 className="text-base font-black text-gray-800">📊 Dashboard Teknis</h1>
            <p className="text-[10px] text-gray-400">LADET — Mode Peneliti</p>
          </div>
          <div className="flex items-center gap-2">
            {demo && <span className="text-[10px] font-bold px-2 py-0.5 rounded-full bg-purple-100 text-purple-700">DEMO</span>}
            <span className={`text-[10px] font-bold px-2 py-0.5 rounded-full ${
              !connected ? 'bg-red-100 text-red-600'
                : rxOnlineEff ? 'bg-green-100 text-green-700'
                : rxOnline === false || (rxSeenAt != null && !rxFresh) ? 'bg-yellow-100 text-yellow-700'
                : 'bg-gray-100 text-gray-600'
            }`}>
              {!connected ? '○ Broker MQTT putus'
                : rxOnlineEff ? '● Broker OK · RX online'
                : rxOnline === false || (rxSeenAt != null && !rxFresh) ? '● Broker OK — RX offline'
                : '● Broker OK — RX belum dilaporkan'}
            </span>
          </div>
        </div>
      </header>

      <div className="px-4 py-4 flex flex-col gap-5 max-w-4xl mx-auto">

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">A — Status Real-time</p>
          <div className={`${LEVEL_BG[level]} ${LEVEL_TEXT[level]} rounded-2xl p-4 flex items-center gap-4 mb-3`}>
            <div className="text-4xl">{LEVEL_ICON[level]}</div>
            <div className="flex-1">
              <p className="text-xs opacity-75 font-semibold uppercase">Status Peringatan</p>
              <p className="text-2xl font-black">{LEVEL_LABEL[level]}</p>
              <p className="text-xs opacity-75">Update: {lastUpdate}</p>
            </div>
            <div className="text-right text-xs opacity-80 font-mono">
              <p>Paket #{live.packet_id}</p>
              {live.seq_stuck && (
                <p className="text-amber-600 font-bold"
                   title="Firmware TX yang terpasang mengirim nomor SEQ yang sama terus, sehingga packet loss tidak bisa diukur dari selisih SEQ. Upload ulang firmware/ladet-tx/ladet-tx.ino (SEQ naik + field RTT=).">
                  ⚠️ SEQ TX macet
                </p>
              )}
              <p>{live.spreading_factor} · {live.distance_m} m</p>
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3 sm:grid-cols-3">
            {/* Tiap kartu memakai status MILIK SENSORNYA SENDIRI (tupoksi:
                tanah → HATI-HATI, getaran → WASPADA, kemiringan → BAHAYA).
                Sebelumnya ketiga kartu disetel level global, sehingga tanah
                100 % ikut tampil merah BAHAYA/Kritis. */}
            <SensorCard type="moisture"  value={live.soil_saturation} level={soilLevel(live.soil_saturation)} />
            <SensorCard type="vibration" value={live.vibration_freq}  level={vibrationLevel(live.vibration_freq)} />
            <SensorCard type="tilt"      value={live.tilt_angle}      level={tiltLevel(live.tilt_angle)} />
          </div>
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">B — Kualitas Sinyal (QoS)</p>
          <QoSCard rssi={live.rssi} snr={live.snr} delay={live.delay_ms} packetLoss={packetLoss} connected={connected} rxOnline={demo ? true : (rxOnlineEff ? true : (rxOnline === false || (rxSeenAt != null && !rxFresh) ? false : rxOnline))} txOnline={demo ? true : (txOnlineEff ? true : (txOnline === false || (txSeenAt != null && !txFresh) ? false : txOnline))} />
          {!demo && (
            <div className="mt-3">
              <RxHealthCard
                health={rxHealth}
                updatedAt={rxHealthAt}
                nowMs={nowMs}
                connected={connected}
              />
            </div>
          )}
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">C — Skenario Pengujian</p>
          <ScenarioSelector
            sf={sf} distanceM={distanceM}
            packetSent={packetSent} packetRecv={packetRecv}
            configStatus={configStatus}
            rxOnline={demo ? true : (rxOnlineEff ? true : (rxOnline === false || (rxSeenAt != null && !rxFresh) ? false : rxOnline))} txOnline={demo ? true : (txOnlineEff ? true : (txOnline === false || (txSeenAt != null && !txFresh) ? false : txOnline))} rxSeenAt={rxSeenAt}
            onSFChange={s  => resetScenario(s, distanceM)}
            onDistanceChange={d => resetScenario(sf, d)}
            onScenarioChange={(s, d) => resetScenario(s, d)}
            mqttClient={mqttRef.current}
          />
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">D — Grafik Real-time</p>
          <div className="flex flex-col gap-3">
            <RealtimeChart data={chartData} type="rssi"   />
            <RealtimeChart data={chartData} type="snr"    />
            <RealtimeChart data={chartData} type="delay"  />
            <RealtimeChart data={chartData} type="packetLoss" />
            <RealtimeChart data={chartData} type="sensor" />
          </div>
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">E — Histori Data</p>
          <PacketTable data={tableData} onExportCSV={exportCSV} />
        </section>

        <section className="bg-white rounded-2xl border border-amber-200 p-4 shadow-sm">
          <div className="flex items-start justify-between gap-3">
            <div>
              <p className="text-sm font-bold text-gray-800">🔒 Tampilan Warga</p>
              <p className="mt-1 text-xs leading-relaxed text-gray-500">
                Sembunyikan status eksperimen agar warga tidak mengira perubahan sensor sebagai longsor nyata. Data tetap direkam di panel Engineer.
              </p>
            </div>
            <span className={`shrink-0 rounded-full px-2 py-1 text-[10px] font-bold ${publicTesting ? 'bg-amber-100 text-amber-700' : 'bg-green-100 text-green-700'}`}>
              {publicTesting ? 'MODE UJI' : 'LIVE WARGA'}
            </span>
          </div>
          <div className="mt-3 flex flex-wrap gap-2">
            <button
              onClick={startEngineerRun}
              disabled={!mqttRef.current?.connected || publicTesting}
              className="rounded-xl bg-amber-500 px-3 py-2 text-xs font-bold text-white disabled:cursor-not-allowed disabled:opacity-40">
              Mulai Pengujian Privat
            </button>
            <button
              onClick={stopEngineerSession}
              disabled={!engineerSession || !mqttRef.current?.connected}
              className="rounded-xl bg-green-600 px-3 py-2 text-xs font-bold text-white disabled:cursor-not-allowed disabled:opacity-40">
              Kembalikan ke LIVE Warga
            </button>
          </div>
          {publicTesting && <p className="mt-2 text-[11px] font-medium text-amber-700">ENGINEER aktif: TX diam sampai RUN selesai. Mode uji maksimal 30 menit.</p>}
        </section>

        <div className="bg-white rounded-2xl border border-gray-200 p-4 flex items-center justify-between">
          <div>
            <p className="text-sm font-semibold text-gray-700">Mode Demo</p>
            <p className="text-xs text-gray-400">{packetSent} paket · {sf} · {distanceM} m</p>
          </div>
          <button onClick={() => setDemo(d => !d)}
            className={`relative inline-flex h-7 w-12 items-center rounded-full transition-colors ${demo ? 'bg-purple-500' : 'bg-gray-200'}`}>
            <span className={`inline-block h-5 w-5 transform rounded-full bg-white shadow transition-transform ${demo ? 'translate-x-6' : 'translate-x-1'}`} />
          </button>
        </div>

      </div>
    </main>
  );
}

