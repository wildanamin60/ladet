'use client';

import { useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { formatTimeWIB } from '@/lib/calculations';
import { fetchAlertHistory, SUPABASE_ENABLED, type AlertRow } from '@/lib/supabase';
import {
  ALERT_MESSAGE,
  HISTORY_EVENT,
  HISTORY_MAX,
  loadAlertHistory,
  type AlertEntry,
  type AlertLevel,
} from '@/lib/alertHistory';

const LEVEL_ACCENT: Record<string, string> = {
  aman: '#4CAF50', 'hati-hati': '#F9A825', waspada: '#E65100', bahaya: '#B71C1C',
};
const LEVEL_TITLE: Record<string, string> = {
  aman: 'AMAN', 'hati-hati': 'HATI-HATI', waspada: 'WASPADA', bahaya: 'BAHAYA',
};
const LEVEL_ICON: Record<string, string> = {
  aman: '✅', 'hati-hati': '⚠️', waspada: '‼️', bahaya: '🚨',
};

function levelFromRow(n: number): AlertLevel {
  if (n === 3) return 'bahaya';
  if (n === 2) return 'waspada';
  return 'hati-hati';
}

function rowToEntry(r: AlertRow): AlertEntry {
  const level = levelFromRow(r.level);
  return { level, message: ALERT_MESSAGE[level], created_at: r.created_at };
}

function dayKey(d: Date): string {
  return d.toLocaleDateString('id-ID', { timeZone: 'Asia/Jakarta', year: 'numeric', month: '2-digit', day: '2-digit' });
}

function dayLabel(d: Date): string {
  const today = new Date();
  const yest = new Date(Date.now() - 24 * 3600 * 1000);
  if (dayKey(d) === dayKey(today)) return 'Hari ini';
  if (dayKey(d) === dayKey(yest)) return 'Kemarin';
  return d.toLocaleDateString('id-ID', {
    timeZone: 'Asia/Jakarta',
    weekday: 'long',
    day: 'numeric',
    month: 'long',
    year: 'numeric',
  });
}

function addDays(d: Date, n: number): Date {
  const x = new Date(d);
  x.setDate(x.getDate() + n);
  return x;
}

function toInputValue(d: Date): string {
  const opt = { timeZone: 'Asia/Jakarta' } as const;
  const y = d.toLocaleString('id-ID', { ...opt, year: 'numeric' });
  const m = d.toLocaleString('id-ID', { ...opt, month: '2-digit' });
  const day = d.toLocaleString('id-ID', { ...opt, day: '2-digit' });
  return `${y}-${m}-${day}`;
}

function fromInputValue(s: string): Date {
  // s = 'YYYY-MM-DD' → that calendar day at 12:00 WIB
  return new Date(`${s}T12:00:00+07:00`);
}

export default function RiwayatPage() {
  const [alerts, setAlerts] = useState<AlertEntry[]>([]);
  const [ready, setReady] = useState(false);
  const [source, setSource] = useState<'db' | 'local'>('local');
  const [selected, setSelected] = useState<Date>(new Date());
  const inited = useRef(false);

  // Sumber utama: database (diisi firmware RX, jadi tetap lengkap walau web tutup).
  useEffect(() => {
    if (!SUPABASE_ENABLED) return;
    let cancelled = false;
    // Semua setState dibungkus fungsi (bukan langsung di badan effect) supaya
    // tidak memicu lint react-hooks/set-state-in-effect. Perilaku tetap sama.
    function applyRows(rows: AlertEntry[]) {
      if (cancelled) return;
      setSource('db');
      if (rows.length > 0) setAlerts(rows);
      setReady(true);
    }
    async function load() {
      applyRows((await fetchAlertHistory()).map(rowToEntry));
    }
    load();
    const t = setInterval(load, 30000);
    return () => {
      cancelled = true;
      clearInterval(t);
    };
  }, []);

  // Cadangan tanpa database: riwayat yang terkumpul di perangkat ini.
  useEffect(() => {
    if (SUPABASE_ENABLED) return;
    const reload = () => {
      setAlerts(loadAlertHistory());
      setReady(true);
    };
    reload();

    window.addEventListener(HISTORY_EVENT, reload);
    window.addEventListener('storage', reload);
    return () => {
      window.removeEventListener(HISTORY_EVENT, reload);
      window.removeEventListener('storage', reload);
    };
  }, []);

  // Pilih tanggal terbaru sekali saja setelah data pertama masuk.
  useEffect(() => {
    function selectLatestDay() {
      if (inited.current || alerts.length === 0) return;
      const latest = alerts.reduce((m, a) => {
        const t = new Date(a.created_at).getTime();
        return t > m ? t : m;
      }, 0);
      if (latest > 0) {
        setSelected(new Date(latest));
        inited.current = true;
      }
    }
    selectLatestDay();
  }, [alerts]);

  const dayItems = useMemo(
    () =>
      alerts
        .filter((a) => dayKey(new Date(a.created_at)) === dayKey(selected))
        .sort((x, y) => new Date(y.created_at).getTime() - new Date(x.created_at).getTime()),
    [alerts, selected]
  );

  function exportCSV() {
    if (alerts.length === 0) return;
    const sorted = [...alerts].sort(
      (a, b) => new Date(b.created_at).getTime() - new Date(a.created_at).getTime()
    );
    const header = 'Waktu (WIB),Level,Keterangan';
    const lines = sorted.map((a) => {
      const waktu = new Date(a.created_at).toLocaleString('id-ID', {
        timeZone: 'Asia/Jakarta',
        year: 'numeric', month: '2-digit', day: '2-digit',
        hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
      });
      const pesan = a.message.replace(/"/g, '""');
      return `${waktu},"${LEVEL_TITLE[a.level]}","${pesan}"`;
    });
    const csv = '\ufeff' + [header, ...lines].join('\r\n');
    const link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }));
    link.download = `ladet-riwayat-${new Date().toISOString().slice(0, 10)}.csv`;
    link.click();
    URL.revokeObjectURL(link.href);
  }

  return (
    <main className="flex flex-col min-h-screen text-white" style={{ background: 'linear-gradient(180deg, #0f172a 0%, #1e293b 100%)' }}>
      <header className="bg-black/20 backdrop-blur border-b border-white/10 px-4 py-3 sticky top-0 z-40">
        <div className="flex items-center justify-between gap-3 max-w-lg mx-auto">
          <Link href="/" className="inline-flex items-center gap-1.5 text-xs font-semibold text-white/80 hover:text-white transition-colors">
            ← Kembali
          </Link>
          <span className="text-sm font-black tracking-tight">Riwayat Peringatan</span>
          <span className="w-12" aria-hidden />
        </div>
      </header>

      <div className="flex-1 px-4 py-4 flex flex-col max-w-lg mx-auto w-full gap-4">
        <div className="flex flex-col gap-2 bg-white/10 border border-white/15 rounded-2xl px-3 py-2.5 backdrop-blur">
          <div className="flex items-center justify-between gap-3">
            <button
              type="button"
              onClick={() => setSelected((s) => addDays(s, -1))}
              className="w-9 h-9 flex items-center justify-center rounded-xl bg-white/10 hover:bg-white/25 text-white text-xl font-bold transition-colors shrink-0"
              aria-label="Hari sebelumnya"
            >
              ‹
            </button>
            <span className="text-sm font-bold text-center flex-1">{dayLabel(selected)}</span>
            <button
              type="button"
              onClick={() => setSelected((s) => addDays(s, 1))}
              className="w-9 h-9 flex items-center justify-center rounded-xl bg-white/10 hover:bg-white/25 text-white text-xl font-bold transition-colors shrink-0"
              aria-label="Hari berikutnya"
            >
              ›
            </button>
          </div>
          <div className="flex items-center gap-2">
            <span className="text-[11px] font-semibold text-white/70 shrink-0">Pilih tanggal</span>
            <input
              type="date"
              value={toInputValue(selected)}
              max={toInputValue(new Date())}
              onChange={(e) => { if (e.target.value) setSelected(fromInputValue(e.target.value)); }}
              className="flex-1 bg-white/10 border border-white/20 rounded-xl px-3 py-1.5 text-sm font-semibold text-white focus:outline-none focus:ring-2 focus:ring-[#0088cc]"
              style={{ colorScheme: 'dark' }}
            />
          </div>
          <button
            type="button"
            onClick={exportCSV}
            disabled={alerts.length === 0}
            className="w-full rounded-xl bg-white/15 hover:bg-white/25 disabled:opacity-40 disabled:cursor-not-allowed px-3 py-2 text-sm font-bold text-white transition-colors"
          >
            ⬇ Unduh Riwayat (CSV)
          </button>
        </div>

        {!ready ? (
          <p className="text-center text-white/50 py-10">Memuat…</p>
        ) : dayItems.length === 0 ? (
          <div className="text-center text-white/60 py-10 flex flex-col gap-2">
            <p>
              {alerts.length === 0
                ? 'Belum ada peringatan tercatat.'
                : `Tidak ada peringatan pada ${dayLabel(selected).toLowerCase()}.`}
            </p>
            {alerts.length === 0 && (
              <p className="text-[11px] text-white/40 px-6 leading-relaxed">
                {source === 'db'
                  ? 'Database tersambung, tetapi belum ada peringatan. RX akan mengisi riwayat begitu status berubah.'
                  : 'Riwayat dicatat selama situs ini dibuka dan dari log terakhir RX. Agar tercatat 24/7, aktifkan database (lihat docs/SUPABASE-SETUP.md).'}
              </p>
            )}
          </div>
        ) : (
          <section
            className="backdrop-blur-md border border-white/15 shadow-2xl text-white"
            style={{ background: 'rgba(0,0,0,0.25)', borderRadius: '16px', padding: '14px 16px' }}
          >
            <ul className="flex flex-col gap-2">
              {dayItems.map((a, i) => (
                <li
                  key={`${a.created_at}-${i}`}
                  className="flex items-center gap-3 bg-white/10 rounded-xl px-3 py-2 border border-white/10"
                  style={{ borderLeft: `3px solid ${LEVEL_ACCENT[a.level as AlertLevel]}` }}
                >
                  <span className="text-lg leading-none" style={{ color: LEVEL_ACCENT[a.level as AlertLevel] }}>
                    {LEVEL_ICON[a.level as AlertLevel]}
                  </span>
                  <div className="flex-1 min-w-0">
                    <p className="text-sm font-bold leading-tight text-white">{LEVEL_TITLE[a.level as AlertLevel]}</p>
                    <p className="text-[11px] text-white/70 leading-tight truncate">{a.message}</p>
                  </div>
                  <span className="text-[10px] text-white/50 font-mono shrink-0 whitespace-nowrap">
                    {formatTimeWIB(a.created_at)}
                  </span>
                </li>
              ))}
            </ul>
          </section>
        )}

        <p className="text-center text-[10px] text-white/60 pt-1">
          {source === 'db'
            ? 'Tersimpan di database — lengkap walau situs tidak dibuka.'
            : `Tersimpan di perangkat ini, maksimal ${HISTORY_MAX} peringatan terakhir.`}
        </p>
        <p className="text-center text-[10px] text-white/60">
          LADET — Landslide Early Detector Tool<br />
          Teknik Elektro · Universitas Jember
        </p>
      </div>
    </main>
  );
}
