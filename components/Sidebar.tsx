'use client';

import Link from 'next/link';
import { usePathname, useRouter } from 'next/navigation';
import { logout } from '@/lib/auth';
import { getMqttClient, releaseEngineerSession, DEFAULT_MQTT_CONFIG } from '@/lib/mqtt';

const NAV = [
  { href: '/dashboard',  label: 'Dashboard',  icon: '📊', desc: 'Data real-time' },
  { href: '/analisis',   label: 'Analisis',   icon: '📈', desc: 'QoS & skenario' },
  { href: '/pengaturan', label: 'Pengaturan', icon: '⚙️', desc: 'MQTT & alert' },
];

export default function Sidebar() {
  const pathname = usePathname();
  const router = useRouter();

  function handleLogout() {
    releaseEngineerSession(getMqttClient(DEFAULT_MQTT_CONFIG));
    logout();
    router.replace('/login');
  }

  return (
    <aside className="md:fixed md:inset-y-0 md:left-0 md:w-64 bg-gray-900 text-gray-100 flex flex-col z-40
                      border-b md:border-b-0 md:border-r border-gray-800">
      <div className="px-5 py-4 flex items-center gap-3 border-b border-gray-800">
        <span className="text-2xl">🏔</span>
        <div>
          <p className="text-sm font-black tracking-tight">LADET</p>
          <p className="text-[10px] text-gray-400">Panel Engineer</p>
        </div>
      </div>

      <nav className="flex md:flex-col flex-row overflow-x-auto md:overflow-visible gap-1 p-2 md:p-3 flex-1">
        {NAV.map(item => {
          const active = pathname === item.href || pathname.startsWith(item.href + '/');
          return (
            <Link key={item.href} href={item.href}
              onClick={() => releaseEngineerSession(getMqttClient(DEFAULT_MQTT_CONFIG))}
              className={`flex items-center gap-3 px-3 py-2.5 rounded-xl whitespace-nowrap transition-colors
                ${active ? 'bg-green-600 text-white' : 'text-gray-300 hover:bg-gray-800 hover:text-white'}`}>
              <span className="text-xl leading-none">{item.icon}</span>
              <span className="flex flex-col">
                <span className="text-sm font-semibold leading-tight">{item.label}</span>
                <span className="text-[10px] opacity-60 leading-tight hidden md:block">{item.desc}</span>
              </span>
            </Link>
          );
        })}
      </nav>

      <div className="p-3 border-t border-gray-800 flex md:flex-col flex-row gap-2">
        <Link href="/" onClick={() => releaseEngineerSession(getMqttClient(DEFAULT_MQTT_CONFIG))}
          className="flex-1 md:flex-none flex items-center justify-center gap-2 px-3 py-2 rounded-xl text-sm
                     bg-gray-800 text-gray-200 hover:bg-gray-700 transition-colors">
          🏠 Beranda Warga
        </Link>
        <button onClick={handleLogout}
          className="flex-1 md:flex-none flex items-center justify-center gap-2 px-3 py-2 rounded-xl text-sm
                     bg-red-600 text-white hover:bg-red-500 transition-colors">
          🚪 Keluar
        </button>
      </div>
    </aside>
  );
}
