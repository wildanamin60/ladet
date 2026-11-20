'use client';

import { useEffect, useState } from 'react';
import TelegramNotifier from '@/components/TelegramNotifier';
import { loadThresholds, saveThresholds, DEFAULT_THRESHOLDS } from '@/lib/thresholds';

const KEYS = {
  brokerUrl: 'mqtt_broker_url', topic: 'mqtt_topic', port: 'mqtt_port',
  tgToken: 'tg_bot_token', tgChatId: 'tg_chat_id', tgEnabled: 'tg_enabled',
};

const LADET_TOPIC_LIST = [
  'ladet/data',              // RX → dashboard (JSON nested)
  'ladet/config/sf',         // web → RX (retained)
  'ladet/config/distance',   // web → RX (retained)
  'ladet/config/status',     // RX → web (konfirmasi config)
  'ladet/config/cmd',        // web → RX (ENGINEER / RUN / MONITOR)
  'ladet/config/public_mode',// web → RX & halaman warga
  'ladet/rx/online',         // LWT / heartbeat RX
  'ladet/rx/status',         // RX → web (diagnostik: reset, boot count, uptime)
  'ladet/tx/online',         // bukti TX terlihat RX via LoRa
];

function Field({ label, value, onChange, type = 'text', placeholder = '', unit = '' }:
  { label: string; value: string; onChange: (v: string) => void; type?: string; placeholder?: string; unit?: string }) {
  return (
    <div className="flex flex-col gap-1">
      <label className="text-xs font-semibold text-gray-600">{label}</label>
      <div className="flex items-center gap-2">
        <input type={type} value={value} placeholder={placeholder}
          onChange={e => onChange(e.target.value)}
          className="flex-1 rounded-xl border border-gray-200 bg-gray-50 px-3 py-2.5 text-sm
            text-gray-800 focus:outline-none focus:ring-2 focus:ring-gray-400 transition" />
        {unit && <span className="text-xs text-gray-400 font-semibold w-8">{unit}</span>}
      </div>
    </div>
  );
}


export default function PengaturanPage() {
  const [brokerUrl, setBrokerUrl] = useState('wss://broker.hivemq.com:8884/mqtt');
  const [topic,     setTopic]     = useState('ladet/data');
  const [port,      setPort]      = useState('8884');
  const [tgToken,   setTgToken]   = useState('');
  const [tgChatId,  setTgChatId]  = useState('');
  const [tgEnabled, setTgEnabled] = useState(false);
  const [saved,     setSaved]     = useState(false);

  // Threshold states
  const [moistWarn,   setMoistWarn]   = useState(String(DEFAULT_THRESHOLDS.soil_saturation.hatiHati));
  const [moistAlert,  setMoistAlert]  = useState(String(DEFAULT_THRESHOLDS.soil_saturation.waspada));
  const [moistDanger, setMoistDanger] = useState(String(DEFAULT_THRESHOLDS.soil_saturation.bahaya));
  const [vibWarn,     setVibWarn]     = useState(String(DEFAULT_THRESHOLDS.vibration_freq.hatiHati));
  const [vibAlert,    setVibAlert]    = useState(String(DEFAULT_THRESHOLDS.vibration_freq.waspada));
  const [vibDanger,   setVibDanger]   = useState(String(DEFAULT_THRESHOLDS.vibration_freq.bahaya));
  const [tiltWarn,    setTiltWarn]    = useState(String(DEFAULT_THRESHOLDS.tilt_angle.hatiHati));
  const [tiltAlert,   setTiltAlert]   = useState(String(DEFAULT_THRESHOLDS.tilt_angle.waspada));
  const [tiltDanger,  setTiltDanger]  = useState(String(DEFAULT_THRESHOLDS.tilt_angle.bahaya));

  useEffect(() => {
    setBrokerUrl(localStorage.getItem(KEYS.brokerUrl) ?? 'wss://broker.hivemq.com:8884/mqtt');
    setTopic(    localStorage.getItem(KEYS.topic)     ?? 'ladet/data');
    setPort(     localStorage.getItem(KEYS.port)      ?? '8884');
    setTgToken(  localStorage.getItem(KEYS.tgToken)   ?? '');
    setTgChatId( localStorage.getItem(KEYS.tgChatId)  ?? '');
    setTgEnabled(localStorage.getItem(KEYS.tgEnabled) === 'true');
    const t = loadThresholds();
    setMoistWarn(String(t.soil_saturation.hatiHati));
    setMoistAlert(String(t.soil_saturation.waspada));
    setMoistDanger(String(t.soil_saturation.bahaya));
    setVibWarn(String(t.vibration_freq.hatiHati));
    setVibAlert(String(t.vibration_freq.waspada));
    setVibDanger(String(t.vibration_freq.bahaya));
    setTiltWarn(String(t.tilt_angle.hatiHati));
    setTiltAlert(String(t.tilt_angle.waspada));
    setTiltDanger(String(t.tilt_angle.bahaya));
  }, []);

  function handleSave() {
    localStorage.setItem(KEYS.brokerUrl, brokerUrl);
    localStorage.setItem(KEYS.topic,     topic);
    localStorage.setItem(KEYS.port,      port);
    localStorage.setItem(KEYS.tgToken,   tgToken);
    localStorage.setItem(KEYS.tgChatId,  tgChatId);
    localStorage.setItem(KEYS.tgEnabled, String(tgEnabled));
    saveThresholds({
      soil_saturation: { hatiHati: +moistWarn, waspada: +moistAlert, bahaya: +moistDanger },
      vibration_freq:  { hatiHati: +vibWarn,   waspada: +vibAlert,   bahaya: +vibDanger   },
      tilt_angle:      { hatiHati: +tiltWarn,  waspada: +tiltAlert,  bahaya: +tiltDanger  },
    });
    setSaved(true);
    setTimeout(() => setSaved(false), 3000);
  }


  return (
    <main className="min-h-screen bg-gray-50 pb-8">
      <header className="bg-white border-b border-gray-200 px-4 py-3 sticky top-0 z-40 shadow-sm">
        <div className="max-w-lg mx-auto">
          <h1 className="text-base font-black text-gray-800">⚙️ Pengaturan</h1>
          <p className="text-[10px] text-gray-400">LADET — Konfigurasi MQTT, Ambang Batas & Telegram</p>
        </div>
      </header>
      <div className="px-4 py-4 flex flex-col gap-5 max-w-lg mx-auto">

        {/* MQTT */}
        <section className="bg-white rounded-2xl border border-gray-200 shadow-sm overflow-hidden">
          <div className="px-4 py-3 border-b border-gray-100 bg-gray-50">
            <h2 className="text-sm font-bold text-gray-700">📡 Konfigurasi MQTT Broker</h2>
            <p className="text-[10px] text-gray-400">Koneksi WebSocket ke broker MQTT</p>
          </div>
          <div className="p-4 flex flex-col gap-3">
            <Field label="URL Broker" value={brokerUrl} onChange={setBrokerUrl}
              placeholder="wss://broker.hivemq.com:8884/mqtt" />
            <div className="grid grid-cols-2 gap-3">
              <Field label="Port" value={port} onChange={setPort} placeholder="8884" />
              <Field label="Topic Utama" value={topic} onChange={setTopic} placeholder="ladet/data" />
            </div>
            <div className="bg-gray-50 rounded-xl p-3 text-[10px] font-mono text-gray-600 space-y-0.5">
              <p className="font-sans font-semibold text-xs mb-1 text-gray-700">📋 Topics aktif:</p>
              {LADET_TOPIC_LIST.map(t => <p key={t}>{t}</p>)}
            </div>
          </div>
        </section>

        {/* Ambang Batas */}
        <section className="bg-white rounded-2xl border border-gray-200 shadow-sm overflow-hidden">
          <div className="px-4 py-3 border-b border-gray-100 bg-gray-50">
            <h2 className="text-sm font-bold text-gray-700">📏 Kalibrasi Ambang Batas Sensor</h2>
            <p className="text-[10px] text-gray-400">Hanya menaikkan level — status dasar tetap dari firmware TX</p>
          </div>
          <div className="p-4 flex flex-col gap-4">
            <div>
              <p className="text-xs font-bold text-gray-600 mb-2">💧 Kelembapan Tanah (SEN0308)</p>
              <div className="grid grid-cols-3 gap-2">
                <Field label="🟡 Hati-hati" value={moistWarn}   onChange={setMoistWarn}   type="number" unit="%" />
                <Field label="🟠 Waspada"   value={moistAlert}  onChange={setMoistAlert}  type="number" unit="%" />
                <Field label="🔴 Bahaya"    value={moistDanger} onChange={setMoistDanger} type="number" unit="%" />
              </div>
            </div>
            <div>
              <p className="text-xs font-bold text-gray-600 mb-2">📳 Getaran (ADXL345)</p>
              <div className="grid grid-cols-3 gap-2">
                <Field label="🟡 Hati-hati" value={vibWarn}   onChange={setVibWarn}   type="number" unit="Δg" />
                <Field label="🟠 Waspada"   value={vibAlert}  onChange={setVibAlert}  type="number" unit="Δg" />
                <Field label="🔴 Bahaya"    value={vibDanger} onChange={setVibDanger} type="number" unit="Δg" />
              </div>
            </div>
            <div>
              <p className="text-xs font-bold text-gray-600 mb-2">📐 Sudut Kemiringan (BNO055)</p>
              <div className="grid grid-cols-3 gap-2">
                <Field label="🟡 Hati-hati" value={tiltWarn}   onChange={setTiltWarn}   type="number" unit="°" />
                <Field label="🟠 Waspada"   value={tiltAlert}  onChange={setTiltAlert}  type="number" unit="°" />
                <Field label="🔴 Bahaya"    value={tiltDanger} onChange={setTiltDanger} type="number" unit="°" />
              </div>
            </div>

            <p className="text-[10px] leading-relaxed text-gray-400 bg-gray-50 rounded-xl p-3">
              Ambang default sama dengan firmware TX (tanah 60 %, getaran 0,50 Δg, kemiringan 5°).
              Nilai di sini hanya bisa <b>menaikkan</b> level; kalau dibuat lebih longgar dari firmware,
              status dari firmware tetap dipakai. Tiap sensor dibatasi tupoksinya: tanah maks
              HATI-HATI, getaran maks WASPADA, kemiringan maks BAHAYA.
            </p>
          </div>
        </section>

        {/* Telegram */}
        <section className="bg-white rounded-2xl border border-gray-200 shadow-sm overflow-hidden">
          <div className="px-4 py-3 border-b border-gray-100 bg-gray-50 flex items-center justify-between">
            <div>
              <h2 className="text-sm font-bold text-gray-700">📨 Notifikasi Telegram</h2>
              <p className="text-[10px] text-gray-400">Kirim peringatan ke grup/channel</p>
            </div>
            <button onClick={() => setTgEnabled(e => !e)}
              className={`relative inline-flex h-7 w-12 items-center rounded-full transition-colors ${tgEnabled ? 'bg-blue-500' : 'bg-gray-200'}`}>
              <span className={`inline-block h-5 w-5 transform rounded-full bg-white shadow transition-transform ${tgEnabled ? 'translate-x-6' : 'translate-x-1'}`} />
            </button>
          </div>
          <div className="p-4 flex flex-col gap-3">
            <Field label="Bot Token" value={tgToken} onChange={setTgToken} type="password"
              placeholder="110201543:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw" />
            <Field label="Channel ID / Username" value={tgChatId} onChange={setTgChatId}
              placeholder="@nama_channel atau -1001234567890" />
            {tgEnabled && <TelegramNotifier botToken={tgToken} chatId={tgChatId} />}
          </div>
        </section>

        {/* Payload format */}
        <section className="bg-white rounded-2xl border border-gray-200 shadow-sm overflow-hidden">
          <div className="px-4 py-3 border-b border-gray-100 bg-gray-50">
            <h2 className="text-sm font-bold text-gray-700">📄 Format Payload MQTT</h2>
          </div>
          <div className="p-4">
            <pre className="text-[10px] font-mono bg-gray-900 text-green-400 rounded-xl p-3 overflow-x-auto leading-5">{`{
  "node_id": "LADET-NODE-01",
  "timestamp": 1234567890,
  "soil_saturation": 72.5,
  "vibration_freq": 0.35,
  "tilt_angle": 4.3,
  "rssi": -87,
  "snr": 6.5,
  "delay_ms": 234,
  "packet_id": 105,
  "spreading_factor": "SF9",
  "distance_m": 100,
  "level": "waspada"
}`}</pre>
          </div>
        </section>

        <button onClick={handleSave}
          className="w-full py-3.5 rounded-2xl bg-gray-800 text-white font-bold text-sm shadow-md active:scale-95 transition-transform">
          💾 Simpan Semua Pengaturan
        </button>

        {saved && (
          <div className="bg-green-50 border border-green-200 rounded-xl p-3 text-center text-sm text-green-700 font-semibold">
            ✅ Pengaturan berhasil disimpan!
          </div>
        )}

        <p className="text-center text-[10px] text-gray-400 pb-2">
          LADET v1.0 · Teknik Elektro · Universitas Jember
        </p>
      </div>
    </main>
  );
}

