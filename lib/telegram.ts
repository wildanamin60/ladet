/**
 * LADET — Telegram Bot helper
 * Kirim notifikasi ke Telegram via Bot API
 */

function formatWIBParts(d: Date): { date: string; time: string } {
  const date = new Intl.DateTimeFormat('id-ID', {
    timeZone: 'Asia/Jakarta',
    day: 'numeric', month: 'long', year: 'numeric',
  }).format(d);
  const time = new Intl.DateTimeFormat('en-GB', {
    timeZone: 'Asia/Jakarta',
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  }).format(d);
  return { date, time: `${time} WIB` };
}

export interface TelegramConfig {
  botToken: string;
  chatId: string;
}

export interface SensorPayload {
  soil_saturation: number;   // %
  vibration_freq: number;    // Δg — ADXL345 (delta getaran)
  tilt_angle: number;        // °
  rssi: number;
  snr: number;
  delay_ms: number;
  spreading_factor?: string;
  distance_m?: number;
  status_level: 'aman' | 'hati-hati' | 'waspada' | 'bahaya';
}

const LEVEL_EMOJI: Record<string, string> = {
  'aman':       '🟢',
  'hati-hati':  '🟡',
  'waspada':    '🟠',
  'bahaya':     '🔴',
};

const LEVEL_LABEL: Record<string, string> = {
  'aman':       'AMAN',
  'hati-hati':  'HATI-HATI',
  'waspada':    'WASPADA',
  'bahaya':     'BAHAYA',
};

const LEVEL_ACTION: Record<string, string> = {
  'aman':       'Kondisi tanah normal. Tidak ada tindakan diperlukan.',
  'hati-hati':  'Kelembapan tanah mulai tinggi. Pantau kondisi lereng secara berkala.',
  'waspada':    'Terdeteksi getaran tanah. Siapkan jalur evakuasi dan pantau terus menerus.',
  'bahaya':     'SEGERA EVAKUASI! Kemiringan tanah melebihi batas aman. Tinggalkan area sekarang!',
};

export function buildTelegramMessage(payload: SensorPayload): string {
  const { date, time } = formatWIBParts(new Date());
  const emoji  = LEVEL_EMOJI[payload.status_level]  ?? '⚪';
  const label  = LEVEL_LABEL[payload.status_level]  ?? payload.status_level.toUpperCase();
  const action = LEVEL_ACTION[payload.status_level] ?? '';
  const prefix = payload.status_level === 'aman' ? '✅' : '⚠️';

  return (
    `${emoji} *PERINGATAN LADET — ${label}*\n` +
    `${prefix} ${action}\n\n` +
    `📍 Lokasi: Desa Klungkung, Sukorambi, Jember\n` +
    `🕐 Waktu: ${date}\n` +
    `${time}\n\n` +
    `📊 Data Sensor:\n` +
    `• Kelembapan Tanah  : ${payload.soil_saturation.toFixed(1)}%\n` +
    `• Getaran (Δg)      : ${payload.vibration_freq.toFixed(3)}\n` +
    `• Sudut Kemiringan  : ${payload.tilt_angle.toFixed(2)}°\n\n` +
    `━━━━━━━━━━━━━━━━━━━\n` +
    `📡 Landslide Early Detector Tool (LADET)`
  );
}

/**
 * Kirim pesan ke Telegram Bot
 * Returns { ok, error } agar UI bisa tampilkan pesan spesifik
 */
export async function sendTelegramMessage(
  config: TelegramConfig,
  text: string
): Promise<{ ok: boolean; error?: string }> {
  if (!config.botToken || !config.chatId) {
    return { ok: false, error: 'Bot Token atau Chat ID belum diisi.' };
  }
  try {
    const url = `https://api.telegram.org/bot${config.botToken}/sendMessage`;
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ chat_id: config.chatId, text, parse_mode: 'Markdown' }),
    });
    const json = await res.json();
    if (!json.ok) {
      const desc: string = json.description ?? 'Unknown error';
      let friendly = desc;
      if (desc.includes('chat not found')) {
        friendly = 'Chat tidak ditemukan.\n• Bot sudah di-START (/start) oleh user/grup?\n• Chat ID sudah benar? Cek via @userinfobot atau @getidsbot';
      } else if (desc.includes('Unauthorized')) {
        friendly = 'Bot Token tidak valid. Periksa kembali token dari @BotFather.';
      } else if (desc.includes('blocked')) {
        friendly = 'Bot diblokir oleh pengguna. Buka Telegram dan unblock bot.';
      } else if (desc.includes('group chat was upgraded')) {
        friendly = 'Grup diupgrade ke supergroup. Gunakan Chat ID baru via @getidsbot.';
      }
      console.warn('[Telegram] API error:', desc);
      return { ok: false, error: friendly };
    }
    return { ok: true };
  } catch (err) {
    console.warn('[Telegram] Fetch error:', err);
    return { ok: false, error: 'Gagal terhubung ke Telegram. Periksa koneksi internet.' };
  }
}

export async function sendLadetAlert(
  config: TelegramConfig,
  payload: SensorPayload
): Promise<boolean> {
  const message = buildTelegramMessage(payload);
  const result  = await sendTelegramMessage(config, message);
  return result.ok;
}



