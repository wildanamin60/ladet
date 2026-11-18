'use client';

interface QoSCardProps {
  rssi: number;
  snr: number;
  delay: number;
  packetLoss: number;
  connected: boolean;
  /** Status perangkat ASLI (null = belum diketahui / firmware RX lama) */
  rxOnline?: boolean | null;
  /** true = RX menerima paket LoRa dari TX (30 dtk terakhir) */
  txOnline?: boolean | null;
}

function SignalBar({ value, max, color }: { value: number; max: number; color: string }) {
  const pct = Math.min(100, Math.max(0, (value / max) * 100));
  return (
    <div className="w-full h-1.5 bg-gray-100 rounded-full overflow-hidden">
      <div className={`h-full rounded-full transition-all duration-500 ${color}`} style={{ width: `${pct}%` }} />
    </div>
  );
}

export default function QoSCard({ rssi, snr, delay, packetLoss, connected, rxOnline, txOnline }: QoSCardProps) {
  // Badge = status perangkat ASLI (RX online via LWT, TX via paket LoRa yang
  // diterima RX) — BUKAN sekadar koneksi browser↔broker.
  let badge = '○ STATUS PERANGKAT?';
  let badgeCls = 'bg-gray-100 text-gray-600';
  if (!connected) {
    badge = '○ BROKER TERPUTUS';
    badgeCls = 'bg-red-100 text-red-700';
  } else if (rxOnline === false) {
    badge = '○ RX OFFLINE';
    badgeCls = 'bg-red-100 text-red-700';
  } else if (rxOnline === true && txOnline === true) {
    badge = '● DATA TERKINI';
    badgeCls = 'bg-green-100 text-green-700';
  } else if (rxOnline === true) {
    badge = '○ MENUNGGU DATA TX';
    badgeCls = 'bg-yellow-100 text-yellow-700';
  }

  // RSSI: -40 (excellent) to -120 (poor) → invert for bar
  const rssiPct = Math.min(100, Math.max(0, ((rssi + 120) / 80) * 100));
  const rssiColor = rssi > -70 ? 'bg-green-500' : rssi > -90 ? 'bg-yellow-500' : 'bg-red-500';
  const rssiLabel = rssi > -70 ? 'Kuat' : rssi > -90 ? 'Sedang' : 'Lemah';

  // SNR: > 10 bagus, < 0 buruk
  const snrColor = snr > 10 ? 'bg-green-500' : snr > 5 ? 'bg-yellow-500' : 'bg-red-500';
  const snrPct = Math.min(100, Math.max(0, ((snr + 5) / 20) * 100));

  // Delay
  const delayColor = delay < 150 ? 'bg-green-500' : delay < 300 ? 'bg-yellow-500' : 'bg-red-500';
  const delayLabel = delay < 150 ? 'Cepat' : delay < 300 ? 'Normal' : 'Lambat';

  // Packet loss
  const lossColor = packetLoss < 5 ? 'bg-green-500' : packetLoss < 15 ? 'bg-yellow-500' : 'bg-red-500';

  return (
    <div className="rounded-2xl border border-gray-200 bg-white shadow-sm p-4">
      {/* Header */}
      <div className="flex items-center justify-between mb-4">
        <h3 className="font-bold text-gray-700 text-sm">📡 Kualitas Sinyal LoRa</h3>
        <span className={`text-xs font-bold px-2 py-0.5 rounded-full ${badgeCls}`}>
          {badge}
        </span>
      </div>

      <div className="grid grid-cols-2 gap-3">
        {/* RSSI */}
        <div className="flex flex-col gap-1.5">
          <div className="flex justify-between items-baseline">
            <span className="text-xs text-gray-500 font-medium">RSSI</span>
            <span className="text-xs text-gray-400">{rssiLabel}</span>
          </div>
          <p className={`text-2xl font-black tabular-nums ${rssi > -70 ? 'text-green-600' : rssi > -90 ? 'text-yellow-600' : 'text-red-600'}`}>
            {rssi} <span className="text-xs font-normal text-gray-400">dBm</span>
          </p>
          <SignalBar value={rssiPct} max={100} color={rssiColor} />
        </div>

        {/* SNR */}
        <div className="flex flex-col gap-1.5">
          <div className="flex justify-between items-baseline">
            <span className="text-xs text-gray-500 font-medium">SNR</span>
            <span className="text-xs text-gray-400">{snr > 10 ? 'Bagus' : snr > 5 ? 'Sedang' : 'Buruk'}</span>
          </div>
          <p className={`text-2xl font-black tabular-nums ${snr > 10 ? 'text-green-600' : snr > 5 ? 'text-yellow-600' : 'text-red-600'}`}>
            {snr.toFixed(1)} <span className="text-xs font-normal text-gray-400">dB</span>
          </p>
          <SignalBar value={snrPct} max={100} color={snrColor} />
        </div>

        {/* Delay */}
        <div className="flex flex-col gap-1.5">
          <div className="flex justify-between items-baseline">
            <span className="text-xs text-gray-500 font-medium">Delay</span>
            <span className="text-xs text-gray-400">{delayLabel}</span>
          </div>
          <p className={`text-2xl font-black tabular-nums ${delay < 150 ? 'text-green-600' : delay < 300 ? 'text-yellow-600' : 'text-red-600'}`}>
            {delay} <span className="text-xs font-normal text-gray-400">ms</span>
          </p>
          <SignalBar value={Math.min(100, 100 - (delay / 5))} max={100} color={delayColor} />
        </div>

        {/* Packet Loss */}
        <div className="flex flex-col gap-1.5">
          <div className="flex justify-between items-baseline">
            <span className="text-xs text-gray-500 font-medium">Packet Loss</span>
            <span className="text-xs text-gray-400">{packetLoss < 5 ? 'Baik' : packetLoss < 15 ? 'Sedang' : 'Tinggi'}</span>
          </div>
          <p className={`text-2xl font-black tabular-nums ${packetLoss < 5 ? 'text-green-600' : packetLoss < 15 ? 'text-yellow-600' : 'text-red-600'}`}>
            {packetLoss.toFixed(1)} <span className="text-xs font-normal text-gray-400">%</span>
          </p>
          <SignalBar value={100 - packetLoss} max={100} color={lossColor} />
        </div>
      </div>
    </div>
  );
}
