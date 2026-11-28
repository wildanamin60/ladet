'use client';

import { useEffect, useRef, useState } from 'react';
import { DEMO_SCENARIOS } from '@/lib/demo';
import type { SF } from '@/lib/demo';
import {
  getMqttClient,
  LADET_TOPICS,
  parsePayload,
  DEFAULT_MQTT_CONFIG,
} from '@/lib/mqtt';
import type { LadetPayload } from '@/lib/mqtt';

const FORMULAS = [
  { name: 'Delay',          formula: 'Delay = t_terima − t_kirim (ms)' },
  { name: 'Delay Rata-rata',formula: 'Delay_avg = ΣDelay / n' },
  { name: 'Packet Loss',    formula: 'PL (%) = ((P_kirim − P_terima) / P_kirim) × 100' },
  { name: 'SNR',            formula: 'SNR (dB) = 10 × log₁₀(P_signal / P_noise)' },
];

const SF_GROUPS = ['SF7', 'SF9', 'SF12'] as const;
const SF_COLORS: Record<string, string> = {
  SF7:  'bg-blue-100 text-blue-800',
  SF9:  'bg-purple-100 text-purple-800',
  SF12: 'bg-indigo-100 text-indigo-800',
};

/** Satu baris tabel. Nilai null = skenario ini belum pernah menerima paket. */
interface Row {
  key: string;
  label: string;
  sf: SF;
  distance_m: number;
  rssiAvg: number | null;
  snrAvg: number | null;
  delayAvg: number | null;
  packetLoss: number | null;
  packetSent: number;
  packetRecv: number;
}

const SKELETON: Row[] = DEMO_SCENARIOS.map(({ key, label, sf, distance_m }) => ({
  key,
  label,
  sf,
  distance_m,
  rssiAvg: null,
  snrAvg: null,
  delayAvg: null,
  packetLoss: null,
  packetSent: 0,
  packetRecv: 0,
}));

function runningAvg(prev: number | null, n: number, value: number): number {
  if (prev === null || n <= 0) return +value.toFixed(1);
  return +(((prev * n) + value) / (n + 1)).toFixed(1);
}

/** Lipat satu payload RX ke baris skenario yang cocok (SF + jarak). */
function aggregate(rows: Row[], p: Partial<LadetPayload>): Row[] {
  const sf = p.spreading_factor;
  const dist = p.distance_m;
  if (!sf || !dist) return rows;

  const idx = rows.findIndex(r => r.sf === sf && r.distance_m === dist);
  if (idx === -1) return rows;

  const row = rows[idx];
  const recv = row.packetRecv + 1;

  // RX sudah menghitung loss kumulatif dari selisih SEQ; pakai itu kalau ada.
  const sent = p.total_expected ?? row.packetSent + 1;
  const received = p.total_received ?? recv;
  const loss = p.packet_loss ?? (sent > 0 ? ((sent - received) / sent) * 100 : 0);
  const delayValid = p.delay_source !== 'invalid' && typeof p.delay_ms === 'number';

  const next: Row = {
    ...row,
    packetRecv: recv,
    packetSent: sent,
    packetLoss: +loss.toFixed(1),
    rssiAvg: runningAvg(row.rssiAvg, row.packetRecv, p.rssi ?? -120),
    snrAvg: runningAvg(row.snrAvg, row.packetRecv, p.snr ?? 0),
    delayAvg: delayValid ? runningAvg(row.delayAvg, row.packetRecv, p.delay_ms!) : row.delayAvg,
  };

  const copy = rows.slice();
  copy[idx] = next;
  return copy;
}

function StatCell({ v, unit, g, w }: { v: number | null; unit: string; g: number; w: number }) {
  if (v === null) {
    return <td className="px-3 py-2 text-right tabular-nums font-semibold text-xs text-gray-300">—</td>;
  }
  const c = v <= g ? 'text-green-600' : v <= w ? 'text-yellow-600' : 'text-red-600';
  return <td className={`px-3 py-2 text-right tabular-nums font-semibold text-xs ${c}`}>{v}{unit}</td>;
}

export default function AnalisisPage() {
  const [rows, setRows] = useState<Row[]>(SKELETON);
  const [connected, setConnected] = useState(false);
  const [packetCount, setPacketCount] = useState(0);
  const packetsRef = useRef(0);

  const [selected, setSelected] = useState<string | null>(null);
  const [activeSF, setActiveSF] = useState<string | null>(null);

  useEffect(() => {
    const client = getMqttClient(DEFAULT_MQTT_CONFIG);

    const onConnect = () => {
      setConnected(true);
      LADET_TOPICS.forEach(t => client.subscribe(t));
    };
    const onDisconnect = () => setConnected(false);
    const onMessage = (topic: string, message: Buffer) => {
      if (topic !== 'ladet/data') return;
      const payload = parsePayload(topic, message.toString());
      if (!payload) return;

      setRows(prev => aggregate(prev, payload));
      packetsRef.current += 1;
      setPacketCount(packetsRef.current);
    };

    if (client.connected) onConnect();

    client.on('connect', onConnect);
    client.on('disconnect', onDisconnect);
    client.on('offline', onDisconnect);
    client.on('message', onMessage);

    return () => {
      client.off('connect', onConnect);
      client.off('disconnect', onDisconnect);
      client.off('offline', onDisconnect);
      client.off('message', onMessage);
    };
  }, []);

  function resetTally() {
    setRows(SKELETON);
    packetsRef.current = 0;
    setPacketCount(0);
    setSelected(null);
  }

  const filtered = activeSF ? rows.filter(s => s.sf === activeSF) : rows;
  const detail = rows.find(x => x.key === selected);

  function exportCSV() {
    const header = 'Skenario,SF,Jarak (m),RSSI Avg (dBm),SNR Avg (dB),Delay Avg (ms),Packet Loss (%),Paket Terkirim,Paket Diterima';
    const rowsCsv = rows.map(s => [
      s.label, s.sf, s.distance_m,
      s.rssiAvg ?? '', s.snrAvg ?? '', s.delayAvg ?? '',
      s.packetLoss ?? '', s.packetSent, s.packetRecv,
    ].join(','));
    const a = Object.assign(document.createElement('a'), {
      href: URL.createObjectURL(new Blob([[header, ...rowsCsv].join('\n')], { type: 'text/csv' })),
      download: `ladet-analisis-${Date.now()}.csv`,
    });
    a.click();
  }

  return (
    <main className="min-h-screen bg-gray-50 pb-24">
      <header className="bg-white border-b border-gray-200 px-4 py-3 sticky top-0 z-40 shadow-sm">
        <div className="max-w-4xl mx-auto flex items-center gap-2">
          <div className="min-w-0">
            <h1 className="text-base font-black text-gray-800">📈 Analisis QoS</h1>
            <p className="text-[10px] text-gray-400">LADET — 12 Skenario · 3 SF × 4 Jarak</p>
          </div>
          <div className="ml-auto flex items-center gap-2">
            <span className={`w-2 h-2 rounded-full ${connected ? 'bg-green-500' : 'bg-gray-300'}`} />
            <span className="text-[10px] text-gray-500 tabular-nums">{packetCount} paket</span>
            <button onClick={resetTally}
              className="text-[10px] font-semibold text-gray-500 px-2 py-1 rounded-lg border border-gray-200 hover:bg-gray-50">
              Reset
            </button>
          </div>
        </div>
      </header>

      <div className="px-4 py-4 flex flex-col gap-5 max-w-4xl mx-auto">

        {packetCount === 0 && (
          <p className="text-[11px] text-gray-500 bg-white border border-dashed border-gray-300 rounded-xl px-3 py-2">
            Menunggu paket dari RX. Baris terisi setelah RX mem-publish ke <code>ladet/data</code>;
            jarak diambil dari config RX (default 100 m), SF dari paket TX.
          </p>
        )}

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">Rumus yang Digunakan</p>
          <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
            {FORMULAS.map(f => (
              <div key={f.name} className="bg-white rounded-xl border border-gray-200 p-3">
                <p className="text-xs font-bold text-gray-600 mb-1">{f.name}</p>
                <p className="text-xs font-mono bg-gray-50 rounded p-2 text-gray-700">{f.formula}</p>
              </div>
            ))}
          </div>
        </section>

        <section>
          <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">Filter Spreading Factor</p>
          <div className="flex gap-2 flex-wrap">
            <button onClick={() => setActiveSF(null)}
              className={`px-3 py-1.5 rounded-xl text-xs font-bold border transition-all
                ${!activeSF ? 'bg-gray-800 text-white border-gray-800' : 'bg-white text-gray-600 border-gray-200 hover:bg-gray-50'}`}>
              Semua ({rows.length})
            </button>
            {SF_GROUPS.map(sf => (
              <button key={sf} onClick={() => setActiveSF(activeSF === sf ? null : sf)}
                className={`px-3 py-1.5 rounded-xl text-xs font-bold border transition-all
                  ${activeSF === sf ? SF_COLORS[sf] + ' border-transparent' : 'bg-white text-gray-600 border-gray-200 hover:bg-gray-50'}`}>
                  {sf} (4 jarak)
              </button>
            ))}
          </div>
        </section>

        <section>
          <div className="flex items-center justify-between mb-2">
            <p className="text-xs font-bold text-gray-400 uppercase tracking-wider">
              Perbandingan Skenario ({filtered.length}/{rows.length})
            </p>
            <button onClick={exportCSV}
              className="text-xs font-semibold text-blue-600 px-3 py-1.5 rounded-lg border border-blue-200 hover:bg-blue-50 transition-colors">
              ⬇ Export CSV
            </button>
          </div>
          <div className="rounded-2xl border border-gray-200 bg-white shadow-sm overflow-hidden">
            <div className="overflow-x-auto">
              <table className="w-full text-xs">
                <thead>
                  <tr className="bg-gray-50 text-gray-500 text-[10px] uppercase tracking-wide">
                    <th className="px-3 py-2 text-left font-semibold">Skenario</th>
                    <th className="px-3 py-2 text-center font-semibold">SF</th>
                    <th className="px-3 py-2 text-right font-semibold">Jarak</th>
                    <th className="px-3 py-2 text-right font-semibold">RSSI</th>
                    <th className="px-3 py-2 text-right font-semibold">SNR</th>
                    <th className="px-3 py-2 text-right font-semibold">Delay</th>
                    <th className="px-3 py-2 text-right font-semibold">PL%</th>
                  </tr>
                </thead>
                <tbody>
                  {filtered.map((s, i) => (
                    <tr key={s.key}
                      onClick={() => setSelected(selected === s.key ? null : s.key)}
                      className={`border-t border-gray-100 cursor-pointer transition-colors
                        ${i % 2 === 0 ? 'bg-white' : 'bg-gray-50/40'}
                        ${selected === s.key ? 'ring-2 ring-inset ring-blue-300 bg-blue-50' : 'hover:bg-blue-50/30'}`}>
                      <td className="px-3 py-2 font-medium text-gray-700 whitespace-nowrap text-[11px]">
                        {s.label}
                        {s.packetRecv > 0 && (
                          <span className="ml-1 text-[10px] text-gray-400">({s.packetRecv}/{s.packetSent})</span>
                        )}
                      </td>
                      <td className="px-3 py-2 text-center">
                        <span className={`px-1.5 py-0.5 rounded-md text-[10px] font-bold ${SF_COLORS[s.sf]}`}>{s.sf}</span>
                      </td>
                      <td className="px-3 py-2 text-right text-gray-600 font-mono">{s.distance_m} m</td>
                      <StatCell v={s.rssiAvg}    unit=" dBm" g={-70} w={-90} />
                      <StatCell v={s.snrAvg}     unit=" dB"  g={10}  w={5}   />
                      <StatCell v={s.delayAvg}   unit=" ms"  g={150} w={300} />
                      <StatCell v={s.packetLoss} unit="%"    g={5}   w={15}  />
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
          <p className="text-[10px] text-gray-400 mt-1 text-center">Klik baris untuk detail · 🟢 Baik · 🟡 Sedang · 🔴 Buruk</p>
        </section>

        {detail && (
          <section>
            <p className="text-xs font-bold text-gray-400 uppercase tracking-wider mb-2">Detail: {detail.label}</p>
            <div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
              {[
                { label: 'RSSI Avg',  value: detail.rssiAvg,  unit: ' dBm', color: 'text-blue-600' },
                { label: 'SNR Avg',   value: detail.snrAvg,   unit: ' dB',  color: 'text-purple-600' },
                { label: 'Delay Avg', value: detail.delayAvg, unit: ' ms',  color: 'text-yellow-600' },
                { label: 'Pkt Loss',  value: detail.packetLoss, unit: '%',  color: 'text-red-600' },
              ].map(item => (
                <div key={item.label} className="bg-white rounded-2xl border border-gray-200 p-4 text-center">
                  <p className="text-[10px] text-gray-400 uppercase tracking-wide mb-1">{item.label}</p>
                  <p className={`text-xl font-black ${item.color}`}>
                    {item.value === null ? '—' : `${item.value}${item.unit}`}
                  </p>
                </div>
              ))}
            </div>
            <p className="text-[10px] text-gray-400 mt-2 text-center">
              {detail.packetRecv} dari {detail.packetSent} paket diterima RX
            </p>
          </section>
        )}

        <div className="bg-white rounded-2xl border border-gray-200 p-4">
          <p className="text-xs font-bold text-gray-600 mb-2">Keterangan Ambang Batas</p>
          <div className="flex flex-col gap-1 text-xs text-gray-600">
            <div className="flex items-center gap-2"><span className="w-2 h-2 rounded-full bg-green-500 flex-shrink-0" />RSSI &gt; −70 dBm · SNR &gt; 10 dB · Delay &lt; 150 ms · PL &lt; 5%</div>
            <div className="flex items-center gap-2"><span className="w-2 h-2 rounded-full bg-yellow-500 flex-shrink-0" />RSSI −70~−90 · SNR 5–10 dB · Delay 150–300 ms · PL 5–15%</div>
            <div className="flex items-center gap-2"><span className="w-2 h-2 rounded-full bg-red-500 flex-shrink-0" />RSSI &lt; −90 dBm · SNR &lt; 5 dB · Delay &gt; 300 ms · PL &gt; 15%</div>
          </div>
        </div>
      </div>
    </main>
  );
}
