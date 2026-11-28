'use client';

import { useEffect, useRef, useState, useCallback } from 'react';
import QoSCard from '@/components/QoSCard';
import RxHealthCard from '@/components/RxHealthCard';
import SensorCard from '@/components/SensorCard';
import RealtimeChart from '@/components/RealtimeChart';
import PacketTable from '@/components/PacketTable';
import type { SF } from '@/components/ScenarioSelector';
import { formatTimeWIB, calculatePacketLoss } from '@/lib/calculations';
import { normalizeLevel, soilLevel, vibrationLevel, tiltLevel, applyCalibration } from '@/lib/thresholds';
import { generateDemoPayload, generateDemoHistory, resetDemoSequence } from '@/lib/demo';
import { getMqttClient, parsePayload, DEFAULT_MQTT_CONFIG, LADET_TOPICS, CONFIG_STATUS_TOPIC, parseConfigStatus, type ConfigStatus, RX_ONLINE_TOPIC, TX_ONLINE_TOPIC, RX_STATUS_TOPIC, parseRxHealth, type RxHealth } from '@/lib/mqtt';
import { insertSensorData } from '@/lib/supabase';
import type { LadetPayload } from '@/lib/mqtt';
import type { SensorData } from '@/lib/supabase';
import type { ChartPoint } from '@/components/RealtimeChart';
import type { MqttClient } from 'mqtt';

const IS_DEMO = process.env.NEXT_PUBLIC_DEMO_MODE === 'true' || !process.env.NEXT_PUBLIC_MQTT_BROKER_URL;
const MAX_CHART = 50;
// Satu pengujian = 50 SLOT SEQ TX (= 50 transmisi), BUKAN 50 paket yang berhasil
// masuk. Jendelanya tetap diambil dari aliran realtime berikutnya, jadi TX TIDAK
// dihentikan dan halaman warga tetap menerima data selama perekaman.
// Konsekuensinya penting untuk packet loss: jumlah transmisi per pengujian selalu
// 50 (durasinya tetap, tidak memanjang saat banyak paket hilang), dan paket yang
// hilang meninggalkan SLOT KOSONG. Jadi loss = slot kosong / slot yang sudah
// dilewati — pada slot ke-50 nilainya tepat paket hilang / 50 (mis. 2 hilang →
// 48/50 = 4 %), bukan hilang / jumlah slot yang terbentang (dulu 50/62 = 19 %).
const TEST_TARGET = 50;

// Menghitung "jumlah paket yang SEHARUSNYA diterima" dari deret SEQ TX.
// Dipakai untuk packet loss jendela pengujian — tahan terhadap dua kejadian:
//   1. Paket hilang  → SEQ melompat (mis. 115 → 126 = 10 paket hilang).
//   2. TX/RX reboot  → SEQ bisa mundur/ulang; itu epoch baru, bukan loss negatif.
// Hasilnya selalu: 1 (baris pertama) + jumlah slot yang dilewati.
function expectedFromSeq(rows: { tx_seq?: number }[]): number {
  if (rows.length === 0) return 0;
  let expected = 1;
  for (let i = 1; i < rows.length; i++) {
    const prev = rows[i - 1].tx_seq;
    const curr = rows[i].tx_seq;
    if (prev == null || curr == null) { expected += 1; continue; }
    expected += curr > prev ? (curr - prev) : 1;  // mundur = epoch baru
  }
  return expected;
}

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

// Baris data ekspor = SensorData + kolom tambahan skripsi. Ditulis di luar
// komponen karena hanya bergantung pada argumennya - itu pula yang membuat
// applyPayload() tetap sah dengan [] sebagai dependency.
interface ExportRow extends SensorData {
  tx_seq?: number;       // nomor SEQ ASLI dari TX (bisa >50 karena continuous)
  tx_time?: string;      // waktu kirim versi TX (= RX terima − delay; BUKAN RTC mentah)
  tx_rtt_ms?: number;    // RTT paket sebelumnya (ms) — 1 jam akurat, tanpa RTC
  loss_ratio?: string;   // "terima/kirim" kumulatif, mis. 16/16
  total_expected?: number;
  total_received?: number;
  /** Nomor boot RX — berubah = gateway reboot di tengah pengujian. */
  rx_boot?: number;
  /** Umur RX (detik) saat paket ini — kecil = baru nyala/reboot. */
  rx_uptime_s?: number;
  /** Paket TX yang hilang tepat sebelum baris ini (0 = berurutan). */
  seq_gap?: number;
  /** Penyebab reboot RX terakhir menurut firmware (BROWNOUT / TASK_WDT / …). */
  rx_reset?: string;
  /** Asal angka delay: rtt | clock | clock-rel | invalid | unknown.
   *  "unknown" = kalibrasi tidak pernah berhasil (mis. field MS= tidak ada). */
  delay_source?: string;
  theta_measured?: number;
  theta_r?: number;
  theta_s?: number;
  g_initial?: number;
  g_measured?: number;
  delta_g?: number;
  tilt_reference?: number;
  tilt_measured?: number;
}

// Satu baris data dari payload RX. packetId = nomor tampilan (1..50 saat
// pengujian), tx_seq = SEQ asli TX (dipakai menghitung packet loss jendela).
const buildRow = (pp: LadetPayload, packetId: number): ExportRow => ({
  id: Date.now() + Math.random(), created_at: new Date().toISOString(),
  node_id: pp.node_id,
  soil_saturation: pp.soil_saturation,
  vibration_freq: pp.vibration_freq,
  tilt_angle: pp.tilt_angle,
  rssi: pp.rssi, snr: pp.snr, delay_ms: pp.delay_ms,
  packet_id: packetId, is_received: true,
  status_level: pp.level,
  spreading_factor: pp.spreading_factor,
  distance_m: pp.distance_m,
  tx_seq: pp.packet_id,
  tx_time: pp.tx_time,
  tx_rtt_ms: pp.tx_rtt_ms,
  loss_ratio: (pp.total_expected != null && pp.total_received != null)
    ? `${pp.total_received}/${pp.total_expected}` : undefined,
  total_expected: pp.total_expected,
  total_received: pp.total_received,
  rx_boot: pp.rx_boot,
  rx_uptime_s: pp.rx_uptime_s,
  seq_gap: pp.seq_gap,
  rx_reset: pp.rx_reset,
  delay_source: pp.delay_source,
  theta_measured: pp.theta_measured,
  theta_r: pp.theta_r,
  theta_s: pp.theta_s,
  g_initial: pp.g_initial,
  g_measured: pp.g_measured,
  delta_g: pp.delta_g,
  tilt_reference: pp.tilt_angle_reference,
  tilt_measured: pp.tilt_angle_measured,
});

const pointFromRow = (row: ExportRow, loss: number): ChartPoint => ({
  time: formatTimeWIB(new Date(row.created_at ?? Date.now())),
  label: `#${row.packet_id}`,
  packet_id: row.packet_id,
  rssi: row.rssi, snr: row.snr, delay: row.delay_ms,
  soil_moisture: row.soil_saturation,
  vibration: row.vibration_freq,
  tilt_angle: row.tilt_angle,
  packet_loss: loss,
});

export default function DashboardPage() {
  const [sf]                        = useState<SF>('SF9');
  const [distanceM]                 = useState(100);
  const [live, setLive]             = useState<LadetPayload>(emptyPayload());
  const [chartData, setChartData]   = useState<ChartPoint[]>([]);
  // Demo: tabel langsung berisi riwayat contoh. Diisi di initializer (BUKAN di
  // dalam efek) supaya render tetap murni dan tidak memicu cascading render.
  const [tableData, setTableData]   = useState<SensorData[]>(() =>
    IS_DEMO ? generateDemoHistory(30, sf, distanceM) : []);
  // brokerConnected = status broker MQTT sebenarnya. Nilai `connected` untuk
  // tampilan diturunkan (mode demo selalu dianggap terhubung), jadi efek demo
  // tidak perlu memanggil setState sama sekali.
  const [brokerConnected, setBrokerConnected] = useState(false);
  const [demo, setDemo]             = useState(IS_DEMO);
  const connected = demo || brokerConnected;
  const [lastUpdate, setLastUpdate] = useState('—');
  const [packetSent, setPacketSent] = useState(0);
  const [packetRecv, setPacketRecv] = useState(0);
  // ── Pengujian 50 paket (paralel dengan realtime) ──
  // testActive : sedang merekam 50 paket berikutnya dari aliran realtime.
  // testDone   : 50 paket sudah lengkap; grafik/tabel menyimpan hasil ini
  //              sampai pengujian baru dimulai (realtime tetap jalan di kartu A/B).
  const [testActive, setTestActive] = useState(false);
  const [testDone, setTestDone] = useState(false);
  const [testCount, setTestCount] = useState(0);   // slot SEQ yang sudah dilewati (1..50)
  const [testRecv, setTestRecv] = useState(0);     // slot yang paketnya benar-benar sampai
  const [testLoss, setTestLoss] = useState(0);
  const [testMsg, setTestMsg] = useState('');      // peringatan (mis. TX reboot di tengah uji)
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
  // Hitung reboot RX yang BENAR-BENAR terjadi selama sesi ini, dari perubahan
  // nomor boot di payload (`rx_boot`). Ini berbeda dari `boot_count` kumulatif
  // yang tersimpan seumur NVS (bisa 130 tanpa masalah) — hanya reboot yang
  // jatuh di tengah aliran data yang dihitung.
  const [rxRebootCount, setRxRebootCount] = useState(0);
  const lastRxBootRef = useRef<number | null>(null);
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
  // Jam acuan = `nowMs` (state yang di-update timer tiap detik), BUKAN Date.now():
  // render harus murni. Selama timer belum tick (nowMs = 0) perangkat dianggap
  // masih segar supaya tidak ada kedip "RX offline" di detik pertama.
  const rxFresh = rxSeenAt != null && (nowMs === 0 || nowMs - rxSeenAt < PRESENCE_TIMEOUT_MS);
  const txFresh = txSeenAt != null && (nowMs === 0 || nowMs - txSeenAt < PRESENCE_TIMEOUT_MS);
  // Status EFEKTIF: mentah dari broker DAN masih segar. Inilah yang dipakai
  // semua indikator — bukan rxOnline/txOnline mentah.
  const rxOnlineEff = rxOnline === true && rxFresh;
  const txOnlineEff = txOnline === true && txFresh;
  // Evidence timestamps are kept per device. Do not use one shared data
  // timestamp here: RX MQTT heartbeats and TX LoRa evidence have different
  // sources and an event from one device must not mask the other device's LWT.
  const rxEvidenceAtRef = useRef<number | null>(null);
  const txEvidenceAtRef = useRef<number | null>(null);
  const demoTimer = useRef<ReturnType<typeof setInterval> | null>(null);
  const mqttRef   = useRef<MqttClient | null>(null);
  const sentRef   = useRef(0);
  const recvRef   = useRef(0);
  const lastDedupeKeyRef = useRef('');   // dedupe: kunci paket terakhir yang diproses
  const realtimeRowRef = useRef(0);      // nomor tampilan realtime (1,2,3,... — bukan SEQ TX)
  const lastDataAtRef = useRef(0);       // waktu (ms) paket data terakhir — penentu "data segar"
  // ── Arsip & pengujian 50 paket ──────────────────────────────────────────
  // exportLogRef : semua paket realtime (grafik/tabel bergulir + CSV biasa)
  // testSlotsRef : hasil pengujian per SLOT SEQ (null = paket di slot itu hilang)
  const exportLogRef = useRef<ExportRow[]>([]);
  // Hasil pengujian disimpan PER SLOT: testSlots[0] = slot SEQ pertama jendela.
  // null = paket di slot itu tidak pernah datang. Slotnya DIBIARKAN kosong, tidak
  // digeser, supaya nomor paket tetap 1..50 dan loss-nya jujur.
  const testSlotsRef = useRef<(ExportRow | null)[]>([]);
  const testActiveRef = useRef(false);
  const testDoneRef = useRef(false);
  const testBaseSeqRef = useRef<number | null>(null);  // SEQ TX tepat sebelum slot 1
  const testScannedRef = useRef(0);                    // slot terakhir yang sudah dilewati
  const lastSeqRef = useRef<number | null>(null);      // SEQ terakhir yang masuk (jangkar jendela)

  const applyPayload = useCallback((p: LadetPayload) => {
    // ── Deteksi reboot RX dari nomor boot di payload ──
    // Dilakukan SEBELUM filter #0/dedupe supaya tidak ada reboot yang lolos.
    // Paket pertama sesi hanya menjadi baseline (tidak dihitung reboot).
    if (p.rx_boot != null) {
      if (lastRxBootRef.current != null && p.rx_boot !== lastRxBootRef.current) {
        setRxRebootCount(c => c + 1);
      }
      lastRxBootRef.current = p.rx_boot;
    }

    // ── PAKET #0 = KALIBRASI (bukan data) ──
    // TX mengirim SEQ=0 sekali saat boot; RX memakainya hanya untuk mengunci
    // kalibrasi delay. #0 tidak masuk grafik, tabel, arsip, maupun Supabase.
    if (p.packet_id === 0) return;

    // SEQ terakhir yang masuk = jangkar jendela pengujian. Diambil SEBELUM
    // dedupe supaya jangkar tetap mutakhir walau paket ini nanti di-dedupe.
    lastSeqRef.current = p.packet_id;

    // ── STATUS REALTIME: selalu diperbarui, apa pun mode yang aktif ──
    // (kartu status, QoS, peta warga tetap hidup walau pengujian berjalan)
    setLive({
      ...p,
      level: applyCalibration(p.level, p.soil_saturation, p.vibration_freq, p.tilt_angle),
    });
    setLastUpdate(formatTimeWIB(new Date()));

    // ── Dedupe: 1 paket LoRa = 1 baris data ──
    // Pakai rx_count dari RX (selalu naik) kalau tersedia, atau bandingkan isi
    // payload (untuk firmware lama yang SEQ-nya macet).
    const dedupeKey = p.rx_count != null && p.rx_count > 0
      ? `rx#${p.rx_count}`
      : [p.packet_id, p.soil_saturation, p.vibration_freq, p.tilt_angle,
         p.rssi, p.snr, p.delay_ms].join('|');
    if (dedupeKey === lastDedupeKeyRef.current) return;
    lastDedupeKeyRef.current = dedupeKey;

    // ── Arsip permanen (Supabase) — realtime maupun pengujian ──
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

    // ── Guard delay tidak wajar (>5 s mustahil untuk LoRa) ──
    const delayValid = p.delay_source !== 'invalid'
      && p.delay_ms >= 0 && p.delay_ms < 5000;

    // ══════════════════════════════════════════════════════════════════
    // PENGUJIAN 50 SLOT SEQ TX — direkam dari aliran realtime yang sedang jalan.
    // TX tidak dihentikan; halaman warga tetap menerima data yang sama.
    // ══════════════════════════════════════════════════════════════════
    if (testActiveRef.current) {
      // Slot 1 = paket PERTAMA sesudah tombol Mulai; SEQ sebelum itu = jangkar.
      if (testBaseSeqRef.current == null) testBaseSeqRef.current = p.packet_id - 1;
      const base = testBaseSeqRef.current;
      const slot = p.packet_id - base;                 // 1..TEST_TARGET

      const finishTest = (msg: string) => {
        testActiveRef.current = false;
        setTestActive(false);
        testDoneRef.current = true;
        setTestDone(true);
        if (msg) setTestMsg(msg);
      };

      // Paket yang SEQ-nya ≤ jangkar = kiriman basi/ulang, bukan bagian jendela →
      // cukup diabaikan (bukan kesalahan). TAPI kalau SEQ mundur jauh, itu TX
      // reboot: jendela tidak utuh lagi (slot 1..n milik firmware baru), jadi
      // hasilnya tidak sah → dibatalkan eksplisit, bukan dibiarkan tampak seperti
      // paket hilang.
      if (slot < 1) {
        if (p.packet_id <= base - 2) {
          finishTest('TX reboot terdeteksi (SEQ TX kembali ke awal) — pengujian tidak sah, ulangi.');
        }
        return;
      }
      // Lewat slot terakhir = jendela tutup. Slot yang tidak pernah datang
      // (termasuk slot 50 kalau paketnya hilang) tetap kosong.
      if (slot > TEST_TARGET) { finishTest(''); return; }

      const slots = testSlotsRef.current;
      if (slots[slot - 1]) return;                     // slot ini sudah terisi
      slots[slot - 1] = buildRow(p, slot);
      const scanned = Math.min(slot, TEST_TARGET);
      testScannedRef.current = scanned;

      // Ringkasan memakai slot yang SUDAH dilewati — slot yang belum dikirim
      // tidak boleh dihitung sebagai paket hilang. Di slot ke-50 nilainya
      // otomatis menjadi: paket hilang / 50.
      const recv = slots.slice(0, scanned).filter(Boolean).length;
      const loss = ((scanned - recv) / scanned) * 100;
      const lossAt = (upto: number) => {
        const r = slots.slice(0, upto).filter(Boolean).length;
        return ((upto - r) / upto) * 100;
      };

      // Tabel & grafik = SATU BARIS PER SLOT. Slot kosong tampil kosong (bukan
      // dihapus/digeser), persis seperti tabel skripsi: nomor paket tetap 1..50.
      const emptyRow = (slotNo: number): ExportRow => ({
        id: 0, created_at: '', node_id: p.node_id,
        soil_saturation: 0, vibration_freq: 0, tilt_angle: 0,
        rssi: 0, snr: 0, delay_ms: 0, packet_id: slotNo, is_received: false,
        status_level: 'aman',
        spreading_factor: p.spreading_factor, distance_m: p.distance_m,
        tx_seq: base + slotNo,
      });
      const rows: ExportRow[] = slots.slice(0, scanned)
        .map((row, i) => row ?? emptyRow(i + 1));
      // Grafik: titik tetap ada untuk SETIAP slot (garis RSSI/SNR/delay putus di
      // slot hilang lewat nilai kosong, kurva loss tetap naik bertahap). Delay
      // paket yang sumbernya tidak terukur dikosongkan agar tidak ada lonjakan.
      const points: ChartPoint[] = rows.map((row, i) => {
        const l = lossAt(i + 1);
        if (!row.is_received) {
          return { time: '', label: `#${i + 1}`, packet_id: i + 1, packet_loss: l };
        }
        const pt = pointFromRow(row, l);
        if (row.packet_id === slot && !delayValid) pt.delay = undefined;
        return pt;
      });

      setTestCount(scanned);
      setTestRecv(recv);
      setTestLoss(loss);
      setPacketSent(scanned);
      setPacketRecv(recv);
      setTableData(rows);
      setChartData(points);

      if (slot === TEST_TARGET) finishTest('');
      return;
    }

    // ── Hasil 50 paket sudah lengkap: jangan ditimpa aliran realtime ──
    if (testDoneRef.current) return;

    // ══════════════════════════════════════════════════════════════════
    // REALTIME BIASA: grafik & tabel bergulir (50 paket terakhir).
    // Nomor "Paket" di sini murni urutan kedatangan (1,2,3,...), BUKAN SEQ TX.
    // Jadi kalau TX reboot dan SEQ kembali ke 1, tampilan realtime tetap rapi
    // dan tidak melompat. SEQ asli tetap disimpan di kolom "SEQ TX" (CSV).
    // ══════════════════════════════════════════════════════════════════
    realtimeRowRef.current += 1;
    const row = buildRow(p, realtimeRowRef.current);
    exportLogRef.current = [...exportLogRef.current, row].slice(-500);

    // Packet loss & counter Terkirim/Diterima (kumulatif dari RX).
    const rxTracks = p.total_expected != null && p.total_received != null;
    const pl = (p.packet_loss > 0 || rxTracks)
      ? p.packet_loss
      : (() => {
          const isLost = p.rssi <= -120 || p.delay_ms >= 9999;
          sentRef.current += 1;
          if (!isLost) recvRef.current += 1;
          return calculatePacketLoss(sentRef.current, recvRef.current);
        })();

    if (rxTracks) {
      setPacketSent(p.total_expected!);
      setPacketRecv(p.total_received!);
    } else {
      setPacketSent(n => n + 1);
      setPacketRecv(n => (p.rssi > -120 ? n + 1 : n));
    }
    if (!delayValid) return;

    setChartData(prev => {
      const next = [...prev, pointFromRow(row, pl)];
      return next.length > MAX_CHART ? next.slice(-MAX_CHART) : next;
    });
    setTableData(prev => [row, ...prev].slice(0, MAX_CHART));
  }, []);

  useEffect(() => {
    if (!demo) return;
    // Hanya side effect (reset ref + timer). Perubahan state yang terlihat
    // dilakukan di event handler toggleDemo(), bukan di badan efek.
    sentRef.current = 0; recvRef.current = 0;
    lastDedupeKeyRef.current = '';
    realtimeRowRef.current = 0;
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
      setBrokerConnected(true);
      // JANGAN menghapus hasil pengujian di sini. Broker publik (HiveMQ) bisa
      // memutus sambungan sebentar saat WiFi lemah lalu menyambung lagi; kalau
      // setiap reconnect mengosongkan testSlotsRef, jendela yang sudah
      // terekam bisa hilang di tengah jalan. Cukup lanjutkan subscribe:
      // data yang sudah direkam tetap utuh, dan paket baru diteruskan normal.
      LADET_TOPICS.forEach(t => client.subscribe(t));
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
      setBrokerConnected(false);
    };
    
    const handleOffline = () => {
      console.log('[Dashboard] 📴 MQTT Offline');
      setBrokerConnected(false);
    };
    
    const handleMessage = (topic: string, msg: Buffer) => {
      // Diagnostik gateway RX — bukan data sensor. Jangan diteruskan ke pipeline
      // payload (parsePayload akan menolaknya karena topik bukan `ladet/data`).
      if (topic === RX_STATUS_TOPIC) {
        const health = parseRxHealth(msg);
        if (health) {
          setRxHealth(health);
          setRxHealthAt(Date.now());
          // `ladet/rx/status` hanya bisa dikirim RX yang sedang hidup dan
          // terhubung ke broker. Jadikan ini bukti hidup juga, supaya badge
          // tidak menulis "offline" hanya karena topik kehadiran basi.
          rxEvidenceAtRef.current = Date.now();
          setRxOnline(true);
          setRxSeenAt(Date.now());
        }
        return;
      }
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
      console.log('[Dashboard] 📨 Message received:', topic, parsed);
      // Satu payload RX = satu titik data. applyPayload() memperbarui status
      // realtime DAN (kalau sedang aktif) merekam paket ke pengujian 50 paket.
      // Dipanggil di luar setState updater supaya tidak dobel saat StrictMode.
      const u = { ...(parsed as LadetPayload) };
      u.level = normalizeLevel(u.level);
      applyPayload(u);
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

  // ── Pengujian 50 slot SEQ: cukup "mulai" di browser ──
  // TIDAK mengirim perintah apa pun ke TX. TX sudah mengalirkan data realtime
  // tanpa henti; 50 transmisi berikutnya direkam sebagai data skripsi.
  // Jangkar = SEQ terakhir yang sudah masuk, jadi slot 1 = paket pertama sesudah
  // tombol ditekan. Kalau belum ada paket sama sekali, jangkar diambil dari paket
  // pertama yang datang (lihat applyPayload).
  // Ganti mode demo. Semua setState di sini ada di EVENT HANDLER (bukan di efek),
  // jadi tidak memicu cascading render.
  function toggleDemo() {
    const next = !demo;
    if (next) {
      setTableData(generateDemoHistory(30, sf, distanceM));
      setChartData([]);
      setPacketSent(0);
      setPacketRecv(0);
      setLastUpdate('—');
    }
    setDemo(next);
  }

  function startTest() {
    testSlotsRef.current = [];
    testBaseSeqRef.current = lastSeqRef.current;
    testScannedRef.current = 0;
    testActiveRef.current = true;
    testDoneRef.current = false;
    setTestActive(true);
    setTestDone(false);
    setTestMsg('');
    setTestCount(0);
    setTestRecv(0);
    setTestLoss(0);
    setChartData([]);
    setTableData([]);
    setPacketSent(0);
    setPacketRecv(0);
    sentRef.current = 0;
    recvRef.current = 0;
  }

  // Hentikan perekaman / kosongkan hasil → grafik & tabel kembali realtime.
  function stopTest() {
    testSlotsRef.current = [];
    testBaseSeqRef.current = null;
    testScannedRef.current = 0;
    testActiveRef.current = false;
    testDoneRef.current = false;
    setTestActive(false);
    setTestDone(false);
    setTestMsg('');
    setTestCount(0);
    setTestRecv(0);
    setTestLoss(0);
    setChartData([]);
    setTableData([]);
    setPacketSent(0);
    setPacketRecv(0);
    realtimeRowRef.current = 0;   // kembali ke realtime: nomor mulai dari 1 lagi
  }

  function exportCSV() {
    // Sumber:
    //   - hasil pengujian (50 slot SEQ) kalau ada → CSV skripsi siap olah
    //   - selain itu: arsip semua paket realtime sejak halaman dibuka
    //
    // Hasil pengujian ditulis SATU BARIS PER SLOT: slot yang paketnya hilang ikut
    // ditulis sebagai baris kosong. Dengan begitu nomor paket tetap 1..50 dan
    // tidak ada baris yang digeser — "hilang di paket 33" = baris 33 kosong.
    const testSlots = testSlotsRef.current;
    const testScanned = testScannedRef.current;
    const isTestFile = testActiveRef.current || testDoneRef.current;
    const baseSeq = testBaseSeqRef.current ?? 0;
    const src: ExportRow[] = isTestFile
      ? testSlots.slice(0, testScanned).map((row, i) => row ?? {
          id: 0, created_at: '', node_id: '',
          soil_saturation: 0, vibration_freq: 0, tilt_angle: 0,
          rssi: 0, snr: 0, delay_ms: 0, packet_id: i + 1, is_received: false,
          status_level: 'aman', spreading_factor: sf, distance_m: distanceM,
          tx_seq: baseSeq + i + 1,
        } as ExportRow)
      : (exportLogRef.current.length > 0 ? exportLogRef.current : tableData as ExportRow[]);

    // "Seharusnya": versi pengujian = slot yang sudah dilewati (jendelanya tetap
    // 50 transmisi, jadi slot 50 → 50/50 syarat maksimal). Versi realtime tetap
    // memakai selisih SEQ aliran, yang juga tahan TX/RX reboot.
    const recvCount = src.filter(r => r.is_received !== false).length;
    const windowExpected = isTestFile ? (testScanned || TEST_TARGET) : expectedFromSeq(src);

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
    // CATATAN PENTING (kenapa tidak pakai ="..."):
    // Trik ="teks" memang memaksa Excel membaca teks, TETAPI pada file CSV yang
    // dibuka langsung, Excel memperlakukan field berawalan "=" sebagai formula
    // dan MENGGABUNGKAN field di sebelahnya (gejala "70 1/1" jadi satu sel).
    // Akibatnya semua kolom setelahnya bergeser. Jadi CSV ini memakai nilai
    // MENTAH tanpa formula, tetapi tetap "dipaksa" menjadi teks dengan cara aman:
    //
    //  1) JAM: Excel mengubah "10:41:11.600" menjadi nilai waktu dan menampilkan
    //     format "mm:ss.0" → jamnya HILANG, tampil "41:11.6" (sudah diuji di
    //     Excel 16). Solusi: beri akhiran " WIB" → "10:41:11.600 WIB". Excel
    //     tidak bisa mem-parse-nya sebagai waktu, jadi tampil utuh sebagai teks.
    //  2) RASIO "1/1": Excel membacanya sebagai tanggal (1-Jan). Solusi: satu
    //     spasi di depan (" 1/1") membuatnya teks. Spasi ini tidak terlihat di
    //     Excel dan tetap tampil persis "1/1". (Spasi di belakang TIDAK ampuh —
    //     sudah diuji: "1/1 " tetap jadi 1-Jan.)
    const cell = (v: unknown): string => {
      if (v == null) return '';
      const s = String(v);
      if (!s.trim()) return '';
      // Kutip hanya bila perlu (mengandung pemisah/baris baru/petik).
      if (s.includes(',') || s.includes('"') || s.includes('\n') || s.includes('\r')) {
        return `"${s.replace(/"/g, '""')}"`;
      }
      return s;
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
    const header = ['Paket', 'SEQ TX', 'SF', 'Jarak (m)', 'Tanggal (WIB)',
      't_kirim (jam WIB)', 't_terima (jam WIB)',
      'Delay (ms) = t_terima - t_kirim', 'Sumber delay',
      'Diterima/Seharusnya (paket)', 'Loss (%)',
      'Paket hilang sebelum baris ini (gap)', 'Penyebab paket hilang',
      'RSSI', 'SNR',
      'Kelembapan terukur (m3/m3)', 'Saturasi (%)',
      'Kelembapan kering th_r (m3/m3)', 'Kelembapan jenuh th_s (m3/m3)',
      'Getaran acuan (g)', 'Getaran terukur (g)', 'Delta getaran (g)',
      'Sudut acuan (deg)', 'Sudut terukur (deg)', 'Delta sudut (deg)',
      'Status'];
    const rows = src.map((r, i) => {
      const e = r as ExportRow;
      const rxIso = r.created_at ?? '';
      const txIso = txISO(rxIso, r.delay_ms);
      const [rxDate, rxTime] = splitISO(rxIso);
      const [txDate, txTime] = splitISO(txIso);
      // 1 kolom tanggal saja (tgl TX = tgl RX; 50 pkt × 2 dtk ≈ 100 dtk).
      // Spasi di depan memaksa Excel menyimpannya sebagai TEKS. Tanpa itu Excel
      // mengubah "2026-11-24" jadi tanggal lokal (mis. 11/24/2026 di Excel
      // en-US) lalu kolom sempit menampilkannya "########".
      const tanggal = rxDate || txDate;
      const tanggalText = tanggal ? ` ${tanggal}` : '';
      // Loss KUMULATIF dari baris 1..i.
      //   • Pengujian: seharusnya = jumlah slot yang sudah dilewati (baris ke-i =
      //     slot ke-i). Di baris terakhir (slot 50) angkanya persis paket
      //     hilang / 50 → inilah angka yang dipakai di Bab 4.
      //   • Realtime: seharusnya = selisih SEQ (paket hilang bikin SEQ melompat),
      //     tidak bergantung counter RX sehingga tetap jujur walau gateway reboot.
      const received = isTestFile
        ? src.slice(0, i + 1).filter(x => x.is_received !== false).length
        : i + 1;
      const cumExpected = isTestFile ? (i + 1) : expectedFromSeq(src.slice(0, i + 1));
      const lossPct = cumExpected > 0
        ? (((cumExpected - received) / cumExpected) * 100).toFixed(2)
        : '0.00';
      // Penyebab paket hilang: kalau gap terjadi tepat saat nomor boot RX
      // berubah / RX baru nyala, itu DOWNTIME GATEWAY (RX mati/restart),
      // bukan kegagalan propagasi LoRa. Penting untuk Bab 4: keduanya harus
      // dilaporkan terpisah supaya loss LoRa tidak terlihat membengkak.
      // Baris DITERIMA terakhir SEBELUM baris ini. Baris slot kosong dilewati,
      // supaya penanda "RX restart" tidak hilang hanya karena ada paket hilang.
      let prev: ExportRow | null = null;
      let blanksBefore = 0;      // slot kosong tepat sebelum baris ini
      for (let k = i - 1; k >= 0; k--) {
        const s = src[k] as ExportRow;
        if (s.is_received === false) { blanksBefore += 1; continue; }
        prev = s; break;
      }
      // Pengujian: jumlah slot kosong sebelum baris ini = paket yang hilang.
      // Dipakai kalau RX sendiri tidak melaporkan gap (mis. RX baru reboot
      // sehingga hitungannya mulai dari nol) — tanpa ini sebab loss bisa hilang.
      const gap = isTestFile ? Math.max(e.seq_gap ?? 0, blanksBefore) : (e.seq_gap ?? 0);
      let gapCause = '';
      if (gap > 0) {
        // Hanya sebut "RX restart" kalau ADA bukti restart:
        //   • nomor boot RX berubah, atau
        //   • uptime RX MUNDUR (paket ini lebih muda dari paket sebelumnya).
        // Jangan pakai "uptime < 180 dtk" saja: saat pengujian dimulai beberapa
        // menit setelah RX dinyalakan, SEMUA gap akan salah dicap "RX restart"
        // padahal paketnya cuma gagal di radio. Data skripsi harus jujur.
        const bootChanged = prev?.rx_boot != null && e.rx_boot != null && e.rx_boot !== prev.rx_boot;
        const uptimeWentBack = prev?.rx_uptime_s != null && e.rx_uptime_s != null
          && e.rx_uptime_s + 1 < prev.rx_uptime_s;
        // Sebab reboot ikut ditulis dari payload (BROWNOUT / TASK_WDT / PANIC).
        // Tanpa ini Bab 4 hanya bisa bilang "RX restart"; dengan ini bisa
        // dibuktikan apakah gateway kehabisan daya atau loop-nya membeku.
        const sebab = bootChanged && e.rx_reset ? ` — ${e.rx_reset}` : '';
        gapCause = (bootChanged || uptimeWentBack)
          ? `RX restart/offline${sebab}`
          : 'Radio/LoRa';
      }
      // Jam WIB lengkap (HH:MM:SS.mmm). Akhiran " WIB" WAJIB: tanpanya Excel
      // mengubahnya jadi nilai waktu dan menampilkan "41:11.6" (jam hilang).
      const clockWIB = (t: string) => (t ? `${t} WIB` : '');
      // Rasio digabung jadi satu sel "1/1". Spasi di depan supaya Excel tidak
      // menganggapnya tanggal (1-Jan). Lihat catatan di atas.
      const ratio = ` ${received}/${cumExpected}`;
      // Baris slot kosong (paket tidak sampai): sengaja dikosongkan semua —
      // hanya nomor paket, SEQ TX, SF/jarak, rasio, dan Loss yang terisi.
      // Sesuai revisi: "hilang di paket 33 → baris 33 kosong, loss-nya nambah".
      if (e.is_received === false) {
        const blank: string[] = Array(header.length).fill('');
        blank[0] = num(r.packet_id);
        blank[1] = num(e.tx_seq ?? '');
        blank[2] = cell(r.spreading_factor);
        blank[3] = num(r.distance_m);
        blank[9] = cell(ratio);
        blank[10] = num(lossPct);
        return blank;
      }
      return [num(r.packet_id), num(e.tx_seq ?? r.packet_id), cell(r.spreading_factor), num(r.distance_m),
        cell(tanggalText), cell(clockWIB(txTime)), cell(clockWIB(rxTime)), num(r.delay_ms),
        cell(r.delay_source ?? ''), cell(ratio), num(lossPct),
        num(gap), cell(gapCause),
        num(r.rssi), num(r.snr),
        num(e.theta_measured), num(r.soil_saturation),
        num(e.theta_r), num(e.theta_s),
        num(e.g_initial), num(e.g_measured), num(e.delta_g),
        num(e.tilt_reference), num(e.tilt_measured), num(r.tilt_angle),
        cell(r.status_level)];
    });

    // Jaga jumlah kolom seragam: baris yang jumlahnya beda membuat Excel
    // menggeser kolom (gejala "data tidak rapi"). Kalau ada yang meleset,
    // lengkapi dengan sel kosong / potong kelebihan.
    const width = header.length;
    const normalized = rows.map(cells => {
      if (cells.length === width) return cells;
      if (cells.length < width) return [...cells, ...Array(width - cells.length).fill('')];
      return cells.slice(0, width);
    });

    // Header juga WAJIB lewat cell(): kalau ada judul kolom yang mengandung
    // koma (mis. "t_terima, WIB") dan ditulis mentah, koma itu memecah header
    // jadi 2 sel → SEMUA judul setelahnya bergeser satu kolom, sementara baris
    // data tetap 25 sel. Gejalanya persis seperti laporan: kolom kiri rapi,
    // mulai kolom ke-7 judul tidak sejajar, dan "Status" tampak kosong.
    //
    // Excel di Windows: UTF-8 BOM (agar θ/° tampil benar) + CRLF (baris rapi).
    const csv = '\uFEFF' + [header.map(cell).join(','), ...normalized.map(c => c.join(','))].join('\r\n');
    const name = isTestFile
      ? `ladet-uji-${recvCount}dari${windowExpected}-${Date.now()}.csv`
      : `ladet-realtime-${Date.now()}.csv`;

    const a = Object.assign(document.createElement('a'), {
      href: URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' })),
      download: name,
    });
    a.click();
  }

  const level = live.level;
  // Loss: saat pengujian → loss jendela 50 paket (gap SEQ di dalam jendela).
  // Selain itu → nilai kumulatif RX, fallback heuristik client-side.
  const packetLoss = (testActive || testDone)
    ? testLoss
    : (live.packet_loss > 0 ? live.packet_loss : calculatePacketLoss(packetSent, packetRecv));
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
                rebootCount={rxRebootCount}
              />
            </div>
          )}
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">C — Konfigurasi Radio</p>
          <div className="rounded-xl border border-gray-200 bg-white px-4 py-3">
            <p className="text-sm font-semibold text-gray-700">SF7 · 433 MHz · 125 kHz · 4 dBm</p>
            <p className="mt-1 text-xs text-gray-500">
              Firmware realtime memakai SF7 tetap dan tidak mendengarkan{' '}
              <code>ladet/config/*</code>, jadi SF tidak bisa diganti dari sini.
              Untuk mengubahnya, ubah <code>LORA_SF</code> di kedua firmware lalu upload ulang.
            </p>
            {configStatus?.state && configStatus.state !== 'idle' && (
              <p className="mt-2 text-xs text-amber-700">
                RX melaporkan status config: {configStatus.state}
              </p>
            )}
          </div>
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">
            D — {testDone ? `Grafik Hasil Pengujian (${testRecv}/${TEST_TARGET} paket)` : testActive ? `Grafik Pengujian (${testCount}/${TEST_TARGET} slot)` : 'Grafik Real-time'}
          </p>
          <div className="flex flex-col gap-3">
            <RealtimeChart data={chartData} type="rssi"   />
            <RealtimeChart data={chartData} type="snr"    />
            <RealtimeChart data={chartData} type="delay"  />
            <RealtimeChart data={chartData} type="packetLoss" />
            <RealtimeChart data={chartData} type="sensor" />
          </div>
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">
            E — {testDone ? `Data Hasil Pengujian (${testRecv}/${TEST_TARGET} paket)` : testActive ? `Merekam Data (${testCount}/${TEST_TARGET} slot)` : 'Histori Data Real-time'}
          </p>
          <PacketTable data={tableData} onExportCSV={exportCSV} />
        </section>

        <section className="bg-white rounded-2xl border border-gray-200 p-4 shadow-sm">
          <div className="flex items-start justify-between gap-3">
            <div>
              <p className="text-sm font-bold text-gray-800">🎯 Pengujian 50 Paket</p>
              <p className="mt-1 text-xs leading-relaxed text-gray-500">
                Tekan <b>Mulai Pengujian</b>. Dashboard merekam <b>50 transmisi TX
                berikutnya</b> (slot SEQ), bukan 50 paket yang berhasil masuk. Jadi
                jumlah paket per pengujian selalu <b>50</b> dan durasinya tetap.
                Paket yang hilang meninggalkan <b>baris kosong</b> di nomornya sendiri
                (hilang di paket 33 → baris 33 kosong) sehingga loss ={' '}
                <b>hilang / 50</b>, mis. 2 hilang = 48/50 = 4 % — bukan 50/62.
                TX <b>tidak dihentikan</b>: aliran realtime & notifikasi warga tetap jalan.
              </p>
            </div>
            <span className={`shrink-0 rounded-full px-2 py-1 text-[10px] font-bold ${
              testActive ? 'bg-amber-100 text-amber-700'
                : testDone ? 'bg-green-100 text-green-700'
                : 'bg-gray-100 text-gray-600'
            }`}>
              {testActive ? 'MEREKAM' : testDone ? 'SELESAI' : 'SIAP'}
            </span>
          </div>

          {/* Progres perekaman */}
          <div className="mt-3">
            <div className="flex items-center justify-between text-[11px] font-semibold text-gray-500">
              <span>{testActive ? `Merekam slot ${testCount}/${TEST_TARGET} · ${testRecv} paket diterima` : testDone ? `Selesai · ${testRecv} dari ${testCount} slot diterima` : 'Belum ada pengujian berjalan'}</span>
              <span>{testCount > 0 ? `Loss: ${testLoss.toFixed(1)}%` : ''}</span>
            </div>
            <div className="mt-1 h-2 w-full overflow-hidden rounded-full bg-gray-100">
              <div className={`h-full rounded-full transition-all ${testDone ? 'bg-green-500' : 'bg-amber-500'}`}
                style={{ width: `${Math.min(100, (testCount / TEST_TARGET) * 100)}%` }} />
            </div>
          </div>

          <div className="mt-3 flex flex-wrap gap-2">
            <button
              onClick={startTest}
              disabled={testActive}
              className="rounded-xl bg-amber-500 px-3 py-2 text-xs font-bold text-white disabled:cursor-not-allowed disabled:opacity-40">
              {testDone ? 'Mulai Pengujian Baru' : 'Mulai Pengujian'}
            </button>
            <button
              onClick={stopTest}
              disabled={!testActive && !testDone}
              className="rounded-xl bg-gray-100 px-3 py-2 text-xs font-bold text-gray-600 disabled:cursor-not-allowed disabled:opacity-40">
              Kembali ke Realtime
            </button>
            <button
              onClick={exportCSV}
              disabled={testCount === 0}
              className="rounded-xl bg-blue-600 px-3 py-2 text-xs font-bold text-white disabled:cursor-not-allowed disabled:opacity-40">
              ⬇ Unduh CSV 50 Paket
            </button>
          </div>

          {testDone && !testMsg && (
            <p className="mt-2 text-[11px] font-medium text-green-700">
              ✅ Jendela pengujian tutup: {testRecv} dari {testCount} slot diterima
              ({((testCount - testRecv) / Math.max(1, testCount) * 100).toFixed(1)} % loss).
              CSV-nya berisi satu baris per slot — slot yang hilang tampil kosong di
              nomornya sendiri. Rumus Bab 4: loss = paket hilang / {testCount}.
            </p>
          )}
          {testMsg && (
            <p className="mt-2 text-[11px] font-bold text-red-600">⚠️ {testMsg}</p>
          )}
          {testActive && (
            <p className="mt-2 text-[11px] font-medium text-amber-700">
              Merekam dari aliran realtime — tidak ada perintah yang dikirim ke TX.
            </p>
          )}
        </section>

        <div className="bg-white rounded-2xl border border-gray-200 p-4 flex items-center justify-between">
          <div>
            <p className="text-sm font-semibold text-gray-700">Mode Demo</p>
            <p className="text-xs text-gray-400">{packetSent} paket · {sf} · {distanceM} m</p>
          </div>
          <button onClick={toggleDemo}
            className={`relative inline-flex h-7 w-12 items-center rounded-full transition-colors ${demo ? 'bg-purple-500' : 'bg-gray-200'}`}>
            <span className={`inline-block h-5 w-5 transform rounded-full bg-white shadow transition-transform ${demo ? 'translate-x-6' : 'translate-x-1'}`} />
          </button>
        </div>

      </div>
    </main>
  );
}

