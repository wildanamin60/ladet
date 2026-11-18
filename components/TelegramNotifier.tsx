'use client';

import { useState } from 'react';
import { sendTelegramMessage, buildTelegramMessage } from '@/lib/telegram';
import type { SensorPayload } from '@/lib/telegram';

interface TelegramNotifierProps {
  botToken: string;
  chatId: string;
  currentPayload?: SensorPayload;
}

export default function TelegramNotifier({ botToken, chatId, currentPayload }: TelegramNotifierProps) {
  const [status, setStatus] = useState<'idle' | 'sending' | 'ok' | 'error'>('idle');
  const [msg, setMsg] = useState('');

  async function handleTest() {
    if (!botToken || !chatId) {
      setStatus('error');
      setMsg('Bot Token atau Chat ID belum diisi di halaman Pengaturan.');
      return;
    }
    setStatus('sending');
    const payload: SensorPayload = currentPayload ?? {
      soil_saturation: 72.5,
      tilt_angle: 4.3,
      vibration_freq: 0.12,
      rssi: -87,
      snr: 6.5,
      delay_ms: 234,
      status_level: 'waspada',
    };
    const text = buildTelegramMessage(payload);
    const result = await sendTelegramMessage({ botToken, chatId }, text);
    if (result.ok) {
      setStatus('ok');
      setMsg('Pesan berhasil dikirim ke Telegram!');
    } else {
      setStatus('error');
      setMsg(result.error ?? 'Gagal mengirim pesan.');
    }
    setTimeout(() => setStatus('idle'), 6000);
  }

  return (
    <div className="flex flex-col gap-2">
      <button
        onClick={handleTest}
        disabled={status === 'sending'}
        className="flex items-center justify-center gap-2 w-full py-2.5 rounded-xl font-semibold text-sm
          bg-blue-500 hover:bg-blue-600 active:scale-95 text-white transition-all disabled:opacity-50"
      >
        {status === 'sending' ? (
          <><span className="animate-spin inline-block">⟳</span> Mengirim...</>
        ) : (
          <><span>📨</span> Test Kirim Notifikasi</>
        )}
      </button>

      {status !== 'idle' && status !== 'sending' && (
        <div className={`text-xs font-medium rounded-xl py-2 px-3 whitespace-pre-line
          ${status === 'ok' ? 'bg-green-50 text-green-700 border border-green-200'
            : 'bg-red-50 text-red-700 border border-red-200'}`}>
          {status === 'ok' ? '✅ ' : '❌ '}{msg}
          {status === 'error' && (
            <p className="mt-2 text-[10px] text-red-500 font-normal">
              Tips: Pastikan bot sudah ditambahkan sebagai <strong>Admin</strong> di channel,
              lalu gunakan @nama_channel (publik) atau numeric ID dari @getidsbot (privat).
            </p>
          )}
        </div>
      )}

      {/* Panduan Channel */}
      <div className="bg-gray-50 rounded-xl p-3 text-[10px] text-gray-500 space-y-1">
        <p className="font-semibold text-gray-600 text-xs">📢 Cara setup Telegram Channel:</p>
        <p>1. Buat bot via <span className="font-mono">@BotFather</span> → salin token</p>
        <p>2. Buat channel Telegram (publik atau privat)</p>
        <p>3. Tambahkan bot sebagai <span className="font-semibold">Admin</span> di channel</p>
        <p>4. Channel publik: pakai <span className="font-mono">@nama_channel</span> sebagai ID</p>
        <p>5. Channel privat: forward pesan ke <span className="font-mono">@getidsbot</span> untuk dapat numeric ID</p>
        <p className="pt-1 text-blue-500 font-medium">Semua subscriber channel otomatis terima notifikasi 🔔</p>
      </div>
    </div>
  );
}

