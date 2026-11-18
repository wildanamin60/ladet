'use client';

import { useEffect, useRef, useState } from 'react';
import type { MqttClient } from 'mqtt';
import { publishSF, publishDistance, publishCmd, type ConfigStatus } from '@/lib/mqtt';

export type SF = 'SF7' | 'SF9' | 'SF12';

/** Perubahan yang menunggu konfirmasi user (tidak langsung di-apply) */
type PendingChange = { kind: 'sf'; value: SF } | { kind: 'dist'; value: number } | { kind: 'run' };

interface ScenarioSelectorProps {
  sf: SF;
  distanceM: number;
  packetSent: number;
  packetRecv: number;
  onSFChange: (sf: SF) => void;
  onDistanceChange: (d: number) => void;
  mqttClient?: MqttClient | null;
  /** Konfirmasi config dari RX firmware (applied/queued/tx_unconfirmed/…) */
  configStatus?: ConfigStatus | null;
  /** Kehadiran perangkat ASLI: null = tidak diketahui (RX lama/off belum pernah konek) */
  rxOnline?: boolean | null;
  /** true = RX benar-benar menerima paket LoRa dari TX (30 dtk terakhir) */
  txOnline?: boolean | null;
  /** Waktu (ms) RX terakhir terbukti hidup — untuk info "terakhir terlihat" */
  rxSeenAt?: number | null;
  /** Ubah SF + jarak SEKALIGUS (hindari stale state saat diset berurutan) */
  onScenarioChange: (sf: SF, distanceM: number) => void;
}

const SF_INFO: Record<SF, { color: string; desc: string }> = {
  SF7:  { color: 'bg-blue-500',   desc: 'Cepat, jangkauan pendek' },
  SF9:  { color: 'bg-purple-500', desc: 'Seimbang' },
  SF12: { color: 'bg-indigo-600', desc: 'Lambat, jangkauan jauh' },
};

const PRESET_DISTANCES = [100, 300, 700, 1000];

export default function ScenarioSelector({
  sf, distanceM, packetSent, packetRecv,
  onSFChange, onDistanceChange, onScenarioChange, mqttClient, configStatus, rxOnline, txOnline, rxSeenAt,
}: ScenarioSelectorProps) {
  const inputRef = useRef<HTMLInputElement>(null);

  // ── Konfirmasi dulu, baru kirim (jangan langsung set saat tombol diklik) ──
  const [pending, setPending] = useState<PendingChange | null>(null);
  const [sentMsg, setSentMsg] = useState<string | null>(null);

  const targetSF = pending ? (pending.kind === 'sf' ? pending.value : sf) : sf;
  const targetDist = pending ? (pending.kind === 'dist' ? pending.value : distanceM) : distanceM;

  function confirmPending() {
    if (!pending) return;
    // ── Perintah mulai run ──
    if (pending.kind === 'run') {
      if (mqttClient?.connected) publishCmd(mqttClient, 'RUN');
      setSentMsg('Perintah RUN dikirim ke TX (via RX) — menunggu konfirmasi');
      setPending(null);
      return;
    }
    if (pending.kind === 'sf') onSFChange(pending.value);
    else onDistanceChange(pending.value);
    setSentMsg(
      pending.kind === 'sf'
        ? `Config ${pending.value} / ${distanceM} m dikirim ke RX — menunggu konfirmasi`
        : `Config ${sf} / ${pending.value} m dikirim ke RX — menunggu konfirmasi`
    );
    setPending(null);
  }

  // Toast "terkirim" hilang otomatis
  useEffect(() => {
    if (!sentMsg) return;
    const t = setTimeout(() => setSentMsg(null), 4000);
    return () => clearTimeout(t);
  }, [sentMsg]);

  // ── Deteksi TIDAK SINKRON: pilihan web ≠ nilai firmware yang dilaporkan RX ──
  // Firmware = sumber kebenaran. Contoh: web pilih SF9 tapi handshake gagal
  // (TX mati) → RX tetap SF7 → web harus terbuka jujur + tawarkan sinkron.
  const fwSF: SF | null =
    configStatus?.sf === 7 ? 'SF7' : configStatus?.sf === 9 ? 'SF9' : configStatus?.sf === 12 ? 'SF12' : null;
  const fwDist: number | null = configStatus?.distance ?? null;
  const notSynced =
    rxOnline === true && configStatus != null &&
    ((fwSF !== null && fwSF !== sf) || (fwDist !== null && fwDist !== distanceM));

  // Web mengikuti firmware — SATU pemanggilan gabungan (bukan dua setState
  // berurutan, yang kedua pakai stale closure dan membalikkan SF ke lama).
  function adoptFirmware() {
    onScenarioChange(fwSF ?? sf, fwDist ?? distanceM);
    setSentMsg('Web disinkronkan mengikuti firmware: ' + (fwSF ?? sf) + ' / ' + (fwDist ?? distanceM) + ' m');
  }

  // Sync ke ESP32 via MQTT setiap kali SF atau jarak berubah
  useEffect(() => {
    if (!mqttClient?.connected) return;
    publishSF(mqttClient, sf);
  }, [sf, mqttClient]);

  useEffect(() => {
    if (!mqttClient?.connected) return;
    publishDistance(mqttClient, distanceM);
  }, [distanceM, mqttClient]);

  const packetLoss = packetSent > 0
    ? (((packetSent - packetRecv) / packetSent) * 100).toFixed(1)
    : '0.0';

  return (
    <div className="bg-white rounded-2xl border border-gray-200 shadow-sm overflow-hidden">
      <div className="px-4 py-3 border-b border-gray-100 bg-gray-50">
        <h2 className="text-sm font-bold text-gray-700">🔬 Skenario Pengujian Aktif</h2>
        <p className="text-[10px] text-gray-400">
          Ubah SF/Jarak → MQTT → RX → LoRa CONF → TX (dengan konfirmasi)
        </p>
        <p className="text-[10px]">
          {/* Status ASLI perangkat — bukan status browser↔broker */}
          {!mqttClient?.connected ? (
            <span className="text-red-500 font-semibold">○ Browser tidak tersambung ke broker</span>
          ) : rxOnline === true ? (
            <span className="text-green-600 font-semibold">● RX online</span>
          ) : rxOnline === false ? (
            <span className="text-red-500 font-semibold">
              ○ RX OFFLINE (mati/listrik putus){rxSeenAt ? ` — terakhir terlihat ${Math.round((Date.now() - rxSeenAt) / 1000)} dtk lalu` : ' — belum pernah terlihat sejak halaman dibuka'}
            </span>
          ) : (
            <span className="text-gray-400">○ RX: status tidak diketahui (upload firmware RX terbaru)</span>
          )}
          {' · '}
          {txOnline === true ? (
            <span className="text-green-600 font-semibold">● TX terlihat oleh RX</span>
          ) : txOnline === false ? (
            <span className="text-amber-600 font-semibold" title="TX bisa saja menyala, tetapi belum ada beacon atau paket LoRa yang diterima RX dalam 35 detik.">
              ○ TX belum terdeteksi via LoRa (cek firmware, wiring, SF, dan jangkauan)
            </span>
          ) : (
            <span className="text-gray-400">○ TX: belum ada bukti LoRa yang diterima RX</span>
          )}
          {rxOnline === true && configStatus?.state === 'applied' && (
            configStatus.tx_confirmed ? (
              <span className="ml-1 text-green-600 font-semibold"> · SF{configStatus.sf}/{configStatus.distance}m aktif (TX+RX ✓)</span>
            ) : (
              <span className="ml-1 text-amber-600 font-semibold"> · SF{configStatus.sf}/{configStatus.distance}m aktif di RX (TX tidak dikonfirmasi)</span>
            )
          )}
          {rxOnline === true && configStatus?.state === 'queued' && (
            <span className="ml-1 text-amber-600 font-semibold"> · config diantri (kanal masih dipakai)…</span>
          )}
          {rxOnline === true && configStatus?.state === 'tx_unconfirmed' && (
            <span className="ml-1 text-red-500 font-semibold"> · TX tidak konfirmasi config — set manual!</span>
          )}
          {rxOnline === true && configStatus?.state === 'run_queued' && (
            <span className="ml-1 text-amber-600 font-semibold"> · perintah RUN dikirim ke TX…</span>
          )}
          {rxOnline === true && configStatus?.state === 'run_resynced' && (
            <span className="ml-1 text-green-600 font-semibold"> · RUN diterima setelah auto-resync SF (kini SF{configStatus.sf}) ✓</span>
          )}
          {rxOnline === true && configStatus?.state === 'run_started' && (
            <span className="ml-1 text-green-600 font-semibold"> · RUN diterima TX — pengujian dimulai ✓</span>
          )}
          {rxOnline === true && configStatus?.state === 'run_busy' && (
            <span className="ml-1 text-amber-600 font-semibold"> · masih ada run berjalan — tunggu selesai dulu</span>
          )}
          {rxOnline === true && configStatus?.state === 'run_unconfirmed' && (
            <span className="ml-1 text-amber-600 font-semibold" title="RX hidup, tetapi TX belum membalas ACK. Board TX dapat tetap menyala tanpa link LoRa yang benar.">
              · TX belum membalas ACK RUN (bukan bukti TX mati)
            </span>
          )}
        </p>
      </div>

      <div className="p-4 flex flex-col gap-4">
        {/* ── Banner TIDAK SINKRON: web ≠ firmware ── */}
        {notSynced && (
          <div className="rounded-xl border border-amber-300 bg-amber-50 p-3">
            <p className="text-xs font-bold text-amber-800 mb-1">⚠ Web tidak sinkron dengan firmware</p>
            <p className="text-[11px] text-amber-700 mb-2">
              Web: <b>{sf} · {distanceM} m</b> — Firmware (RX): <b>{fwSF ?? sf} · {fwDist ?? distanceM} m</b>.
              Data yang berjalan mengikuti firmware.
            </p>
            <div className="flex gap-2">
              <button onClick={adoptFirmware}
                className="rounded-lg px-3 py-1.5 text-[11px] font-bold bg-amber-600 text-white hover:bg-amber-700 transition">
                Samakan web dengan firmware ({fwSF ?? sf} · {fwDist ?? distanceM} m)
              </button>
              {(fwSF !== null && fwSF !== sf) && (
                <button onClick={() => setPending({ kind: 'sf', value: sf })}
                  className="rounded-lg px-3 py-1.5 text-[11px] font-bold bg-white border border-amber-400 text-amber-800 hover:bg-amber-100 transition">
                  Kirim {sf} ke firmware
                </button>
              )}
            </div>
          </div>
        )}

        {/* SF Selector */}
        <div>
          <p className="text-xs font-semibold text-gray-600 mb-2">Spreading Factor (SF)</p>
          <div className="grid grid-cols-3 gap-2">
            {(['SF7', 'SF9', 'SF12'] as SF[]).map(s => (
              <button key={s} onClick={() => { if (s !== sf) setPending({ kind: 'sf', value: s }); }}
                className={`rounded-xl py-2.5 px-3 text-sm font-bold transition-all
                  ${sf === s
                    ? `${SF_INFO[s].color} text-white shadow-md scale-105`
                    : 'bg-gray-100 text-gray-600 hover:bg-gray-200'}`}>
                {s}
                <p className="text-[9px] font-normal mt-0.5 opacity-80">{SF_INFO[s].desc}</p>
              </button>
            ))}
          </div>
        </div>

        {/* Jarak Selector */}
        <div>
          <p className="text-xs font-semibold text-gray-600 mb-2">Jarak Pengujian</p>
          <div className="flex gap-2 flex-wrap mb-2">
            {PRESET_DISTANCES.map(d => (
              <button key={d} onClick={() => { if (d !== distanceM) setPending({ kind: 'dist', value: d }); }}
                className={`rounded-xl px-4 py-2 text-sm font-bold transition-all
                  ${distanceM === d
                    ? 'bg-gray-800 text-white shadow-md'
                    : 'bg-gray-100 text-gray-600 hover:bg-gray-200'}`}>
                {d} m
              </button>
            ))}
          </div>
          <div className="flex items-center gap-2">
            <input ref={inputRef} type="number" min={1} max={9999}
              defaultValue={distanceM}
              placeholder="Jarak custom (m)"
              className="flex-1 rounded-xl border border-gray-200 bg-gray-50 px-3 py-2 text-sm
                text-gray-800 focus:outline-none focus:ring-2 focus:ring-gray-400 transition" />
            <button
              onClick={() => {
                const val = parseInt(inputRef.current?.value ?? '');
                if (!isNaN(val) && val > 0 && val !== distanceM) setPending({ kind: 'dist', value: val });
              }}
              className="rounded-xl px-3 py-2 bg-gray-800 text-white text-sm font-semibold
                hover:bg-gray-700 transition">
              Set
            </button>
          </div>
        </div>

        {/* Stats paket */}
        <div className="grid grid-cols-3 gap-2 pt-1">
          {[
            { label: 'Terkirim', value: packetSent, color: 'text-blue-600' },
            { label: 'Diterima', value: packetRecv, color: 'text-green-600' },
            { label: 'Loss',     value: `${packetLoss}%`, color: 'text-red-500' },
          ].map(item => (
            <div key={item.label} className="bg-gray-50 rounded-xl p-2 text-center">
              <p className="text-[10px] text-gray-400 uppercase tracking-wide">{item.label}</p>
              <p className={`text-lg font-black ${item.color}`}>{item.value}</p>
            </div>
          ))}
        </div>

        {/* ── Mulai run baru (perintah ke TX) ── */}
        <button onClick={() => setPending({ kind: 'run' })}
          className="w-full rounded-xl py-2.5 bg-green-600 text-white text-sm font-bold
            hover:bg-green-700 active:scale-[0.99] transition">
          ▶ Mulai Run Baru — kirim perintah ke TX
        </button>

        <p className="text-[10px] text-gray-400 text-center">
          50 paket per skenario · interval 2 detik · {sf} / {distanceM} m
        </p>
      </div>

      {/* ── Dialog konfirmasi (muncul dulu sebelum config dikirim) ── */}
      {pending && (
        <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/40 px-4" onClick={() => setPending(null)}>
          <div className="bg-white rounded-2xl shadow-xl border border-gray-200 p-5 w-full max-w-xs" onClick={(e) => e.stopPropagation()}>
            <h3 className="text-sm font-bold text-gray-800 mb-1">
              {pending.kind === 'run' ? 'Mulai run baru sekarang?' : 'Terapkan skenario ini?'}
            </h3>
            <p className="text-xs text-gray-500 mb-3">
              {pending.kind === 'run'
                ? 'TX akan mengirim #0 kalibrasi lalu data #1..#50 pada SF/jarak aktif, dari posisi node sekarang (baseline lama dipakai).'
                : 'Config dikirim ke RX via MQTT lalu dikonfirmasi ke TX via LoRa — diterapkan hanya saat kanal kosong (tidak mengganggu run berjalan).'}
            </p>
            <div className="flex items-center justify-center gap-3 bg-gray-50 rounded-xl p-3 mb-4">
              <span className="text-sm font-black text-gray-800">{targetSF}</span>
              <span className="text-gray-300">·</span>
              <span className="text-sm font-black text-gray-800">{targetDist} m</span>
            </div>
            <div className="flex gap-2">
              <button onClick={() => setPending(null)}
                className="flex-1 rounded-xl py-2 text-sm font-bold bg-gray-100 text-gray-600 hover:bg-gray-200 transition">
                Batal
              </button>
              <button onClick={confirmPending}
                className="flex-1 rounded-xl py-2 text-sm font-bold bg-blue-600 text-white hover:bg-blue-700 transition">
                Ya, terapkan
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Toast: config terkirim (hilang sendiri ±4 detik) ── */}
      {sentMsg && (
        <div className="fixed bottom-4 left-1/2 -translate-x-1/2 z-50 bg-gray-900 text-white text-xs font-semibold px-4 py-2.5 rounded-xl shadow-lg">
          📤 {sentMsg}
        </div>
      )}
    </div>
  );
}
