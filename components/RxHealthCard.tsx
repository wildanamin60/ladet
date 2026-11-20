'use client';

import type { RxHealth } from '@/lib/mqtt';

interface RxHealthCardProps {
  health: RxHealth | null;
  /** Kapan laporan terakhir diterima (Date.now di sisi browser) */
  updatedAt: number | null;
  /** Jam browser dari ticker dashboard (bukan Date.now saat render) */
  nowMs: number;
  connected: boolean;
}

/** Penyebab reset → kalimat yang bisa dibaca penguji. */
const RESET_LABEL: Record<string, string> = {
  POWERON: 'Colokan / listrik',
  EXT_RESET: 'Tombol reset',
  SW_RESTART: 'Restart sendiri',
  PANIC: 'Crash firmware',
  TASK_WDT: 'Watchdog (loop macet)',
  INT_WDT: 'Watchdog (interrupt)',
  WDT: 'Watchdog',
  BROWNOUT: 'Tegangan drop',
  DEEPSLEEP: 'Deep sleep',
  UNKNOWN: 'Tidak diketahui',
};

/** Reset yang menandakan ada masalah nyata, bukan sekadar colok listrik. */
const PROBLEM_RESETS = new Set(['PANIC', 'TASK_WDT', 'INT_WDT', 'WDT', 'BROWNOUT']);

function formatUptime(seconds?: number): string {
  if (seconds == null || !Number.isFinite(seconds)) return '—';
  const s = Math.max(0, Math.floor(seconds));
  if (s < 60) return `${s} dtk`;
  if (s < 3600) return `${Math.floor(s / 60)} mnt`;
  if (s < 86400) return `${Math.floor(s / 3600)} j ${Math.floor((s % 3600) / 60)} mnt`;
  return `${Math.floor(s / 86400)} hr ${Math.floor((s % 86400) / 3600)} j`;
}

function wifiQuality(rssi?: number): { label: string; className: string } {
  if (rssi == null || !Number.isFinite(rssi)) return { label: '—', className: 'text-gray-400' };
  if (rssi >= -60) return { label: 'Kuat', className: 'text-green-600' };
  if (rssi >= -70) return { label: 'Cukup', className: 'text-yellow-600' };
  return { label: 'Lemah', className: 'text-red-600' };
}

export default function RxHealthCard({ health, updatedAt, nowMs, connected }: RxHealthCardProps) {
  // Laporan dikirim tiap 1 menit → dianggap basi setelah 2,5 menit.
  const ageMs = updatedAt == null || nowMs === 0 ? null : Math.max(0, nowMs - updatedAt);
  const stale = ageMs != null && ageMs > 150_000;

  const reset = health?.reset ?? '';
  const resetIsProblem = PROBLEM_RESETS.has(reset);
  const bootCount = health?.boot_count;
  const manyBoots = bootCount != null && bootCount >= 5;
  const wifi = wifiQuality(health?.wifi_rssi);

  const statusPill = !connected
    ? { text: 'Broker terputus', className: 'bg-gray-100 text-gray-500' }
    : updatedAt == null
      ? { text: 'Belum ada laporan', className: 'bg-gray-100 text-gray-500' }
      : stale
        ? { text: `Basi — ${formatUptime((ageMs ?? 0) / 1000)} lalu`, className: 'bg-amber-100 text-amber-700' }
        : { text: 'Segar', className: 'bg-green-100 text-green-700' };

  return (
    <div className="rounded-2xl border border-gray-200 bg-white shadow-sm overflow-hidden">
      <div className="flex items-center justify-between px-4 py-3 border-b border-gray-100 bg-gray-50">
        <div>
          <h3 className="text-sm font-bold text-gray-700">🩺 Diagnostik Gateway RX</h3>
          <p className="text-[10px] text-gray-400">Kesehatan perangkat — dari topik ladet/rx/status</p>
        </div>
        <span className={`rounded-full px-2.5 py-1 text-[10px] font-bold ${statusPill.className}`}>
          {statusPill.text}
        </span>
      </div>

      <div className="grid grid-cols-2 sm:grid-cols-3 gap-px bg-gray-100">
        <div className="bg-white px-4 py-3">
          <p className="text-[10px] font-semibold uppercase tracking-wide text-gray-400">Reset terakhir</p>
          <p className={`mt-1 text-sm font-bold ${resetIsProblem ? 'text-red-600' : 'text-gray-700'}`}>
            {reset ? (RESET_LABEL[reset] ?? reset) : '—'}
          </p>
          {resetIsProblem && (
            <p className="text-[10px] text-red-500 leading-tight">pernah mati sendiri</p>
          )}
        </div>

        <div className="bg-white px-4 py-3">
          <p className="text-[10px] font-semibold uppercase tracking-wide text-gray-400">Boot ke-</p>
          <p className={`mt-1 text-sm font-bold tabular-nums ${manyBoots ? 'text-amber-600' : 'text-gray-700'}`}>
            {bootCount != null ? bootCount : '—'}
          </p>
          {manyBoots && (
            <p className="text-[10px] text-amber-500 leading-tight">sering reboot — cek catu daya</p>
          )}
        </div>

        <div className="bg-white px-4 py-3">
          <p className="text-[10px] font-semibold uppercase tracking-wide text-gray-400">Uptime</p>
          <p className="mt-1 text-sm font-bold tabular-nums text-gray-700">{formatUptime(health?.uptime_s)}</p>
          <p className="text-[10px] text-gray-400 leading-tight">sejak boot terakhir</p>
        </div>

        <div className="bg-white px-4 py-3">
          <p className="text-[10px] font-semibold uppercase tracking-wide text-gray-400">WiFi</p>
          <p className={`mt-1 text-sm font-bold tabular-nums ${wifi.className}`}>
            {health?.wifi_rssi != null ? `${health.wifi_rssi} dBm` : '—'}
          </p>
          <p className="text-[10px] text-gray-400 leading-tight">{wifi.label}</p>
        </div>

        <div className="bg-white px-4 py-3">
          <p className="text-[10px] font-semibold uppercase tracking-wide text-gray-400">Memori bebas</p>
          <p className="mt-1 text-sm font-bold tabular-nums text-gray-700">
            {health?.heap != null ? `${Math.round(health.heap / 1024)} KB` : '—'}
          </p>
          <p className="text-[10px] text-gray-400 leading-tight">heap ESP32</p>
        </div>

        <div className="bg-white px-4 py-3">
          <p className="text-[10px] font-semibold uppercase tracking-wide text-gray-400">SF aktif</p>
          <p className="mt-1 text-sm font-bold tabular-nums text-gray-700">
            {health?.sf != null ? `SF${health.sf}` : '—'}
          </p>
          <p className="text-[10px] text-gray-400 leading-tight">harus sama dengan TX</p>
        </div>
      </div>

      {updatedAt == null && connected && (
        <p className="px-4 py-3 text-[11px] leading-relaxed text-gray-400 border-t border-gray-100">
          Menunggu laporan pertama. RX mengirim diagnostik tepat setelah connect, lalu tiap 1 menit.
          Kalau firmware RX belum diperbarui, kartu ini akan tetap kosong (data sensor tetap jalan normal).
        </p>
      )}
    </div>
  );
}
