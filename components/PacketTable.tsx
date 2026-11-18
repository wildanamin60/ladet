'use client';

import { formatWIB } from '@/lib/calculations';
import type { SensorData } from '@/lib/supabase';
import { levelToNumber } from '@/lib/thresholds';

interface PacketTableProps {
  data: SensorData[];
  onExportCSV?: () => void;
}

const LEVEL_BADGE: Record<number, string> = {
  0: 'bg-green-100 text-green-700',
  1: 'bg-yellow-100 text-yellow-800',
  2: 'bg-orange-100 text-orange-700',
  3: 'bg-red-100 text-red-700',
};
const LEVEL_LABEL: Record<number, string> = {
  0: 'Aman', 1: 'Waspada', 2: 'Siaga', 3: 'Bahaya',
};

export default function PacketTable({ data, onExportCSV }: PacketTableProps) {
  return (
    <div className="rounded-2xl border border-gray-200 bg-white shadow-sm overflow-hidden">
      {/* Header */}
      <div className="flex items-center justify-between px-4 py-3 border-b border-gray-100">
        <h3 className="font-bold text-gray-700 text-sm">📋 Histori Data Sensor</h3>
        {onExportCSV && (
          <button
            onClick={onExportCSV}
            className="text-xs font-semibold text-blue-600 hover:text-blue-700 flex items-center gap-1
              px-3 py-1.5 rounded-lg border border-blue-200 hover:bg-blue-50 transition-colors"
          >
            ⬇ Export CSV
          </button>
        )}
      </div>

      {/* Table wrapper — scrollable on mobile */}
      <div className="overflow-x-auto">
        <table className="w-full text-xs">
          <thead>
            <tr className="bg-gray-50 text-gray-500 uppercase tracking-wide text-[10px]">
              <th className="px-3 py-2 text-left font-semibold whitespace-nowrap">Waktu (WIB)</th>
              <th className="px-3 py-2 text-right font-semibold">Paket</th>
              <th className="px-3 py-2 text-right font-semibold">Lembab (%)</th>
              <th className="px-3 py-2 text-right font-semibold">Tilt (°)</th>
              <th className="px-3 py-2 text-right font-semibold">Getaran (Δg)</th>
              <th className="px-3 py-2 text-right font-semibold">RSSI</th>
              <th className="px-3 py-2 text-right font-semibold">SNR</th>
              <th className="px-3 py-2 text-right font-semibold">Delay (ms)</th>
              <th className="px-3 py-2 text-center font-semibold">Status</th>
            </tr>
          </thead>
          <tbody>
            {data.length === 0 ? (
              <tr>
                <td colSpan={9} className="text-center text-gray-400 py-8">Belum ada data</td>
              </tr>
            ) : (
              data.map((row, i) => (
                <tr key={`${row.created_at}-${row.packet_id}-${i}`} className={`border-t border-gray-50 ${i % 2 === 0 ? 'bg-white' : 'bg-gray-50/50'}`}>
                  <td className="px-3 py-2 font-mono whitespace-nowrap text-gray-600">
                    {row.created_at ? formatWIB(row.created_at) : '—'}
                  </td>
                  <td className="px-3 py-2 text-right tabular-nums font-mono text-gray-500">#{row.packet_id}</td>
                  <td className="px-3 py-2 text-right tabular-nums font-medium">{row.soil_saturation.toFixed(1)}</td>
                  <td className="px-3 py-2 text-right tabular-nums font-medium">{row.tilt_angle.toFixed(2)}</td>
                  <td className="px-3 py-2 text-right tabular-nums font-medium">{row.vibration_freq.toFixed(3)}</td>
                  <td className="px-3 py-2 text-right tabular-nums font-medium">{row.rssi.toFixed(1)}</td>
                  <td className="px-3 py-2 text-right tabular-nums font-medium">{row.snr.toFixed(1)}</td>
                  <td className="px-3 py-2 text-right tabular-nums font-medium">{row.delay_ms.toFixed(0)}</td>
                  <td className="px-3 py-2 text-center">
                    <span className={`px-2 py-0.5 rounded-full font-semibold text-[10px] ${LEVEL_BADGE[levelToNumber(row.status_level)]}`}>
                      {LEVEL_LABEL[levelToNumber(row.status_level)]}
                    </span>
                  </td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
