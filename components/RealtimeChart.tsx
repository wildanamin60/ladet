'use client';

import {
  LineChart, Line, BarChart, Bar,
  XAxis, YAxis, CartesianGrid, Tooltip,
  ResponsiveContainer, ReferenceLine, Legend,
} from 'recharts';

export interface ChartPoint {
  time: string;
  label?: string;          // '#SEQ' — label per paket, dipakai sebagai sumbu X chart delay
  packet_id?: number;
  rssi?: number;
  snr?: number;
  delay?: number;
  soil_moisture?: number;
  tilt_angle?: number;
  vibration?: number;
  packet_loss?: number;
}

interface RealtimeChartProps {
  data: ChartPoint[];
  type: 'rssi' | 'snr' | 'delay' | 'sensor' | 'packetLoss';
  height?: number;
}

const CHART_CONFIG = {
  rssi: {
    title: 'RSSI vs Waktu',
    unit: 'dBm',
    xKey: 'time',
    lines: [{ key: 'rssi', color: '#3b82f6', label: 'RSSI' }],
    refLines: [{ y: -70, color: '#22c55e', label: 'Kuat' }, { y: -90, color: '#ef4444', label: 'Lemah' }],
    yDomain: [-120, -40] as [number, number],
  },
  snr: {
    title: 'SNR vs Waktu',
    unit: 'dB',
    xKey: 'time',
    lines: [{ key: 'snr', color: '#8b5cf6', label: 'SNR' }],
    refLines: [{ y: 10, color: '#22c55e', label: 'Bagus' }, { y: 0, color: '#ef4444', label: 'Buruk' }],
    yDomain: [-5, 20] as [number, number],
  },
  delay: {
    // 1 batang = 1 paket. Delay = t_terima(RX) − t_kirim(TX) untuk paket itu,
    // BUKAN nilai kumulatif — jadi tidak boleh naik terus sepanjang pengujian.
    title: 'Delay per Paket (1 batang = 1 paket)',
    unit: 'ms',
    xKey: 'label',
    lines: [{ key: 'delay', color: '#f59e0b', label: 'Delay' }],
    refLines: [{ y: 300, color: '#ef4444', label: 'Batas' }],
    yDomain: [0, 500] as [number, number],
  },
  sensor: {
    title: 'Data Sensor',
    unit: '',
    xKey: 'time',
    lines: [
      { key: 'soil_moisture', color: '#06b6d4', label: 'Kelembaban (%)' },
      { key: 'vibration', color: '#ef4444', label: 'Getaran (Δg)' },
      { key: 'tilt_angle', color: '#f97316', label: 'Kemiringan (°)' },
    ],
    refLines: [],
    yDomain: [0, 100] as [number, number],
  },
  packetLoss: {
    title: 'Packet Loss (%)',
    unit: '%',
    xKey: 'time',
    lines: [{ key: 'packet_loss', color: '#ef4444', label: 'Packet Loss' }],
    refLines: [{ y: 5, color: '#ef4444', label: 'Ambang' }],
    yDomain: [0, 100] as [number, number],
  },
};

// Custom tooltip
const CustomTooltip = ({ active, payload, label, unit }: any) => {
  if (!active || !payload?.length) return null;
  const pt: ChartPoint | undefined = payload[0]?.payload;
  return (
    <div className="bg-gray-900 text-white text-xs rounded-lg p-2 shadow-lg border border-gray-700">
      <p className="font-semibold mb-1 text-gray-300">
        {pt?.packet_id ? `Paket #${pt.packet_id}` : label}
      </p>
      {pt?.time && pt.time !== label && (
        <p className="text-gray-400 mb-1">Diterima: {pt.time}</p>
      )}
      {payload.map((p: any) => (
        <p key={p.dataKey} style={{ color: p.color }}>
          {p.name}: <span className="font-bold">{p.value?.toFixed(2)}{unit}</span>
        </p>
      ))}
    </div>
  );
};

export default function RealtimeChart({ data, type, height = 180 }: RealtimeChartProps) {
  const cfg = CHART_CONFIG[type];
  const isBar = type === 'delay';

  return (
    <div className="rounded-2xl border border-gray-200 bg-white shadow-sm p-4">
      <h3 className="font-bold text-gray-700 text-sm mb-3">{cfg.title}</h3>
      <div style={{ overflowX: 'auto' }}>
        <div style={{ minWidth: Math.max(data.length * 12, 280), height }}>
          <ResponsiveContainer width="100%" height="100%">
            {isBar ? (
              <BarChart data={data} margin={{ top: 4, right: 8, left: -20, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#f0f0f0" />
                <XAxis dataKey={cfg.xKey} tick={{ fontSize: 9 }} interval="preserveStartEnd" />
                <YAxis domain={cfg.yDomain} tick={{ fontSize: 9 }} />
                <Tooltip content={<CustomTooltip unit={cfg.unit} />} />
                {cfg.refLines.map(r => (
                  <ReferenceLine key={r.y} y={r.y} stroke={r.color} strokeDasharray="4 4"
                    label={{ value: r.label, fill: r.color, fontSize: 9 }} />
                ))}
                {cfg.lines.map(l => (
                  <Bar key={l.key} dataKey={l.key} name={l.label} fill={l.color} radius={[2, 2, 0, 0]} />
                ))}
              </BarChart>
            ) : (
              <LineChart data={data} margin={{ top: 4, right: 8, left: -20, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#f0f0f0" />
                <XAxis dataKey="time" tick={{ fontSize: 9 }} interval="preserveStartEnd" />
                <YAxis domain={cfg.yDomain} tick={{ fontSize: 9 }} />
                <Tooltip content={<CustomTooltip unit={cfg.unit} />} />
                <Legend iconSize={8} wrapperStyle={{ fontSize: 10 }} />
                {cfg.refLines.map(r => (
                  <ReferenceLine key={r.y} y={r.y} stroke={r.color} strokeDasharray="4 4"
                    label={{ value: r.label, fill: r.color, fontSize: 9 }} />
                ))}
                {cfg.lines.map(l => (
                  <Line
                    key={l.key}
                    type="monotone"
                    dataKey={l.key}
                    name={l.label}
                    stroke={l.color}
                    strokeWidth={2}
                    dot={false}
                    activeDot={{ r: 3 }}
                    isAnimationActive={false}
                  />
                ))}
              </LineChart>
            )}
          </ResponsiveContainer>
        </div>
      </div>
    </div>
  );
}
