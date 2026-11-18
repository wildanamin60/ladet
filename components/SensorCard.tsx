'use client';

type Level = 'aman' | 'hati-hati' | 'waspada' | 'bahaya';

interface SensorCardProps {
  type: 'moisture' | 'vibration' | 'tilt';
  value: number;
  level: Level;
}

const SENSOR_CONFIG = {
  moisture: {
    icon: '💧',
    label: 'Kelembapan Tanah',
    unit: '%',
    sensor: 'SEN0308',
    maxVal: 100,
    description: {
      aman: 'Kering — normal',
      'hati-hati': 'Mulai jenuh air',
      waspada: 'Jenuh tinggi',
      bahaya: 'Kritis',
    },
  },
  vibration: {
    icon: '📳',
    label: 'Getaran',
    unit: ' Δg',
    sensor: 'ADXL345',
    maxVal: 20,
    description: {
      aman: 'Tidak ada getaran',
      'hati-hati': 'Getaran sangat ringan',
      waspada: 'Getaran terdeteksi',
      bahaya: 'Getaran kuat',
    },
  },
  tilt: {
    icon: '📐',
    label: 'Sudut Kemiringan',
    unit: '°',
    sensor: 'BNO055',
    maxVal: 45,
    description: {
      aman: 'Normal — stabil',
      'hati-hati': 'Sedikit miring',
      waspada: 'Miring signifikan',
      bahaya: 'Berbahaya',
    },
  },
};

const LEVEL_COLORS: Record<Level, string> = {
  aman:        'border-green-400 bg-green-50 text-green-700',
  'hati-hati': 'border-yellow-400 bg-yellow-50 text-yellow-800',
  waspada:     'border-orange-400 bg-orange-50 text-orange-700',
  bahaya:      'border-red-500 bg-red-50 text-red-700',
};

const VALUE_COLORS: Record<Level, string> = {
  aman:        'text-green-600',
  'hati-hati': 'text-yellow-600',
  waspada:     'text-orange-600',
  bahaya:      'text-red-600',
};

const FILL_COLORS: Record<Level, string> = {
  aman:        'bg-green-500',
  'hati-hati': 'bg-yellow-400',
  waspada:     'bg-orange-500',
  bahaya:      'bg-red-500',
};

export default function SensorCard({ type, value, level }: SensorCardProps) {
  const cfg       = SENSOR_CONFIG[type];
  const fillPct   = Math.min(100, (value / cfg.maxVal) * 100);
  // Getaran Δg nilainya bisa sangat kecil (mis. 0.0033). Kalau dipaksa 2 desimal
  // akan tampil "0.00" seolah sensor tidak terbaca, jadi nilai < 1 Δg pakai
  // 3 desimal supaya variasi kecil tetap terlihat.
  const formatted = type === 'moisture'  ? value.toFixed(1)
                  : type === 'vibration' ? (value < 1 ? value.toFixed(3) : value.toFixed(2))
                  : value.toFixed(2);

  return (
    <div className={`rounded-2xl border-2 p-4 shadow-sm flex flex-col gap-2 ${LEVEL_COLORS[level]}`}>
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <span className="text-3xl">{cfg.icon}</span>
          <div>
            <p className="text-xs font-semibold uppercase tracking-wide opacity-70">{cfg.label}</p>
            <p className="text-[10px] opacity-50 font-mono">{cfg.sensor}</p>
          </div>
        </div>
      </div>

      <div className={`text-4xl font-black tabular-nums leading-none ${VALUE_COLORS[level]}`}>
        {formatted}
        <span className="text-base font-medium ml-0.5">{cfg.unit}</span>
      </div>

      <p className="text-[11px] opacity-70 font-medium">{cfg.description[level]}</p>

      <div className="w-full h-2 bg-white/60 rounded-full overflow-hidden">
        <div
          className={`h-full rounded-full transition-all duration-500 ${FILL_COLORS[level]}`}
          style={{ width: `${fillPct}%` }}
        />
      </div>
    </div>
  );
}

