/**
 * Riwayat peringatan LADET.
 *
 * Tanpa database, riwayat disimpan di localStorage perangkat yang membuka
 * situs. Supaya riwayat tetap ada walau tidak ada browser yang terbuka saat
 * kejadian, RX mengirim log terakhirnya ke topik retained `ladet/alert/log`
 * dan `AlertRecorder` menyalinnya ke sini begitu ada halaman yang dibuka.
 */

export type AlertLevel = 'aman' | 'hati-hati' | 'waspada' | 'bahaya';

export interface AlertEntry {
  level: AlertLevel;
  message: string;
  /** ISO 8601 */
  created_at: string;
}

const HISTORY_KEY = 'ladet_alert_history_v2';
const LAST_LEVEL_KEY = 'ladet_alert_last_level';

export const HISTORY_MAX = 200;

/** Event lokal supaya semua halaman ikut ter-update tanpa reload. */
export const HISTORY_EVENT = 'ladet-alert-history';

export const ALERT_MESSAGE: Record<AlertLevel, string> = {
  aman: 'Kondisi aman.',
  'hati-hati': 'Kelembapan tanah meningkat.',
  waspada: 'Getaran terdeteksi pada lereng.',
  bahaya: 'LERENG BERGERAK — segera evakuasi!',
};

const LEVELS: AlertLevel[] = ['aman', 'hati-hati', 'waspada', 'bahaya'];

function isEntry(v: unknown): v is AlertEntry {
  const o = v as AlertEntry;
  return !!o && LEVELS.includes(o.level) && typeof o.created_at === 'string';
}

function dedupeKey(e: AlertEntry): string {
  return `${e.level}|${e.created_at}`;
}

export function loadAlertHistory(): AlertEntry[] {
  if (typeof window === 'undefined') return [];
  try {
    const raw = localStorage.getItem(HISTORY_KEY);
    if (!raw) return [];
    const arr = JSON.parse(raw);
    return Array.isArray(arr) ? arr.filter(isEntry) : [];
  } catch {
    return [];
  }
}

export function saveAlertHistory(list: AlertEntry[]): void {
  if (typeof window === 'undefined') return;
  const sorted = [...list].sort(
    (a, b) => new Date(b.created_at).getTime() - new Date(a.created_at).getTime()
  ).slice(0, HISTORY_MAX);
  try {
    localStorage.setItem(HISTORY_KEY, JSON.stringify(sorted));
  } catch {
    /* kuota penuh / mode privat — daftar tetap tampil di sesi ini */
  }
  window.dispatchEvent(new CustomEvent(HISTORY_EVENT));
}

/**
 * Catat peringatan dari data live. Hanya level yang berubah yang dicatat,
 * supaya status BAHAYA selama 10 menit tidak menjadi ratusan baris.
 * Mengembalikan true kalau ada entri baru.
 */
export function recordLevel(level: AlertLevel, createdAt = new Date().toISOString()): boolean {
  if (typeof window === 'undefined') return false;
  if (localStorage.getItem(LAST_LEVEL_KEY) === level) return false;
  localStorage.setItem(LAST_LEVEL_KEY, level);
  if (level === 'aman') return false;
  saveAlertHistory([{ level, message: ALERT_MESSAGE[level], created_at: createdAt }, ...loadAlertHistory()]);
  return true;
}

/**
 * Gabungkan log dari RX (topik `ladet/alert/log`) ke riwayat perangkat ini.
 * Entri yang sudah ada dilewati, jadi aman dipanggil berulang.
 */
export function ingestAlerts(entries: AlertEntry[]): number {
  if (typeof window === 'undefined' || entries.length === 0) return 0;
  const existing = loadAlertHistory();
  const seen = new Set(existing.map(dedupeKey));
  const added = entries.filter((e) => !seen.has(dedupeKey(e)));
  if (added.length === 0) return 0;
  saveAlertHistory([...added, ...existing]);
  // Samakan penanda level supaya data live berikutnya tidak mencatat ulang
  // peringatan yang sama dengan yang baru diimpor.
  const newest = [...added, ...existing].sort(
    (a, b) => new Date(b.created_at).getTime() - new Date(a.created_at).getTime()
  )[0];
  if (newest) localStorage.setItem(LAST_LEVEL_KEY, newest.level);
  return added.length;
}

export function clearAlertHistory(): void {
  if (typeof window === 'undefined') return;
  localStorage.removeItem(HISTORY_KEY);
  localStorage.removeItem(LAST_LEVEL_KEY);
  window.dispatchEvent(new CustomEvent(HISTORY_EVENT));
}

/** 1 = HATI-HATI, 2 = WASPADA, 3 = BAHAYA (format log dari RX). */
export function levelFromNumber(n: number): AlertLevel | null {
  if (n === 1) return 'hati-hati';
  if (n === 2) return 'waspada';
  if (n === 3) return 'bahaya';
  return null;
}

/** "2026-11-26 18:42:11" (WIB dari RX) → ISO 8601. */
export function wibToIso(s: string): string {
  const m = s.match(/^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})$/);
  if (!m) return s;
  return `${m[1]}-${m[2]}-${m[3]}T${m[4]}:${m[5]}:${m[6]}+07:00`;
}
