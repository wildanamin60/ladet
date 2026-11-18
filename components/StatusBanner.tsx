'use client';

interface StatusBannerProps {
  level: 'aman' | 'hati-hati' | 'waspada' | 'bahaya';
  lastUpdate?: string;
  connected?: boolean;
}

const STATUS_CONFIG = {
  'aman': {
    bg: 'bg-green-500', textColor: 'text-white',
    icon: '✅', title: 'AMAN',
    subtitle: 'Kondisi tanah normal, tidak ada ancaman longsor',
    pulse: false,
  },
  'hati-hati': {
    bg: 'bg-yellow-400', textColor: 'text-yellow-900',
    icon: '⚠️', title: 'HATI-HATI',
    subtitle: 'Kelembapan tanah tinggi. Pantau kondisi secara berkala.',
    pulse: false,
  },
  'waspada': {
    bg: 'bg-orange-500', textColor: 'text-white',
    icon: '‼️', title: 'WASPADA',
    subtitle: 'Terdeteksi getaran tanah. Siapkan jalur evakuasi!',
    pulse: false,
  },
  'bahaya': {
    bg: 'bg-red-600', textColor: 'text-white',
    icon: '🚨', title: 'BAHAYA',
    subtitle: 'SEGERA EVAKUASI! Kemiringan tanah melebihi batas aman!',
    pulse: true,
  },
};

export default function StatusBanner({ level, lastUpdate, connected = true }: StatusBannerProps) {
  const cfg = STATUS_CONFIG[level] ?? STATUS_CONFIG['aman'];

  return (
    <div className={`relative w-full rounded-2xl overflow-hidden shadow-lg ${cfg.bg} ${cfg.pulse ? 'animate-pulse' : ''}`}>
      {!connected && (
        <div className="absolute inset-0 bg-gray-800/80 z-10 flex items-center justify-center rounded-2xl">
          <div className="text-center text-white">
            <div className="text-3xl mb-1">📡</div>
            <p className="text-sm font-semibold">Koneksi ke broker terputus</p>
            <p className="text-xs opacity-75">Menunggu koneksi ulang ke server MQTT…</p>
          </div>
        </div>
      )}
      <div className={`flex flex-col items-center justify-center py-10 px-6 text-center ${cfg.textColor}`}>
        <span className="text-xs font-bold uppercase tracking-widest opacity-75 mb-2">
          Status Peringatan
        </span>
        <div className={`text-7xl mb-3 drop-shadow-md ${cfg.pulse ? 'animate-bounce' : ''}`}>
          {cfg.icon}
        </div>
        <h2 className="text-5xl font-black tracking-tight mb-3">{cfg.title}</h2>
        <p className="text-lg font-medium max-w-xs leading-snug opacity-90">{cfg.subtitle}</p>
        {lastUpdate && (
          <p className="mt-4 text-xs opacity-60 font-mono">Terakhir diperbarui: {lastUpdate}</p>
        )}
      </div>
    </div>
  );
}

