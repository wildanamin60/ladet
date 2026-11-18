'use client';

import { useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { supabase } from '@/lib/supabase';
import { formatTimeWIB } from '@/lib/calculations';

type Level = 'aman' | 'hati-hati' | 'waspada' | 'bahaya';
type AlertItem = { level: Level; message: string; created_at: string };

const LEVEL_ACCENT: Record<string, string> = {
  aman: '#4CAF50', 'hati-hati': '#F9A825', waspada: '#E65100', bahaya: '#B71C1C',
};
const LEVEL_TITLE: Record<string, string> = {
  aman: 'AMAN', 'hati-hati': 'HATI-HATI', waspada: 'WASPADA', bahaya: 'BAHAYA',
};
const LEVEL_ICON: Record<string, string> = {
  aman: '✅', 'hati-hati': '⚠️', waspada: '‼️', bahaya: '🚨',
};
const ALERT_MESSAGE: Record<string, string> = {
  aman: 'Kondisi aman.',
  'hati-hati': 'Kelembapan tanah meningkat.',
  waspada: 'Getaran terdeteksi pada lereng.',
  bahaya: 'LERENG BERGERAK — segera evakuasi!',
};

const SUPABASE_ENABLED =
  process.env.NEXT_PUBLIC_DEMO_MODE !== 'true' &&
  !!process.env.NEXT_PUBLIC_SUPABASE_URL &&
  !!process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY &&
  !process.env.NEXT_PUBLIC_SUPABASE_URL.includes('your-project') &&
  !process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY.includes('your-anon-key');

async function fetchAlertsSupabase(): Promise<AlertItem[]> {
  const { data, error } = await supabase
    .from('sensor_data')
    .select('status_level, created_at')
    .neq('status_level', 'aman')
    .order('created_at', { ascending: false })
    .limit(50);
  if (error || !data) return [];
  return (data as { status_level: Level; created_at: string }[]).map((r) => ({
    level: r.status_level,
    message: ALERT_MESSAGE[r.status_level],
    created_at: r.created_at,
  }));
}

function buildDemoAlerts(): AlertItem[] {
  const seq: Level[] = ['waspada', 'hati-hati', 'bahaya', 'waspada', 'hati-hati', 'aman'];
  return seq.map((lv, i) => ({
    level: lv,
    message: ALERT_MESSAGE[lv],
    created_at: new Date(Date.now() - i * 41 * 60000).toISOString(),
  }));
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
  const [alerts, setAlerts] = useState<AlertItem[]>([]);
  const [loading, setLoading] = useState(true);
  const [selected, setSelected] = useState<Date>(new Date());
  const inited = useRef(false);

  useEffect(() => {
    let cancelled = false;
    async function load() {
      const list = SUPABASE_ENABLED ? await fetchAlertsSupabase() : buildDemoAlerts();
      if (!cancelled) {
        setAlerts(list);
        setLoading(false);
      }
    }
    load();
    const t = setInterval(load, 15000);
    return () => {
      cancelled = true;
      clearInterval(t);
    };
  }, []);

  useEffect(() => {
    if (!inited.current && alerts.length > 0) {
      const latest = alerts.reduce((m, a) => {
        const t = new Date(a.created_at).getTime();
        return t > m ? t : m;
      }, 0);
      if (latest > 0) {
        setSelected(new Date(latest));
        inited.current = true;
      }
    }
  }, [alerts]);

  const dayItems = useMemo(
    () =>
      alerts
        .filter((a) => dayKey(new Date(a.created_at)) === dayKey(selected))
        .sort((x, y) => new Date(y.created_at).getTime() - new Date(x.created_at).getTime()),
    [alerts, selected]
  );

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
        </div>

        {loading && alerts.length === 0 ? (
          <p className="text-center text-white/50 py-10">Memuat…</p>
        ) : dayItems.length === 0 ? (
          <p className="text-center text-white/50 py-10">
            Tidak ada riwayat peringatan pada {dayLabel(selected).toLowerCase()}.
          </p>
        ) : (
          <section
            className="backdrop-blur-md border border-white/15 shadow-2xl text-white"
            style={{ background: 'rgba(0,0,0,0.25)', borderRadius: '16px', padding: '14px 16px' }}
          >
            <ul className="flex flex-col gap-2">
              {dayItems.map((a, i) => (
                <li
                  key={i}
                  className="flex items-center gap-3 bg-white/10 rounded-xl px-3 py-2 border border-white/10"
                  style={{ borderLeft: `3px solid ${LEVEL_ACCENT[a.level]}` }}
                >
                  <span className="text-lg leading-none" style={{ color: LEVEL_ACCENT[a.level] }}>{LEVEL_ICON[a.level]}</span>
                  <div className="flex-1 min-w-0">
                    <p className="text-sm font-bold leading-tight text-white">{LEVEL_TITLE[a.level]}</p>
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
          LADET — Landslide Early Detector Tool<br />
          Teknik Elektro · Universitas Jember
        </p>
      </div>
    </main>
  );
}