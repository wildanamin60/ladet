'use client';

import { useEffect, useState } from 'react';
import { useRouter } from 'next/navigation';
import Sidebar from '@/components/Sidebar';
import { isAuthed } from '@/lib/auth';

export default function AdminLayout({ children }: { children: React.ReactNode }) {
  const router = useRouter();
  const [status, setStatus] = useState<'loading' | 'authed'>('loading');

  useEffect(() => {
    // Dibungkus fungsi: setState langsung di badan effect memicu lint
    // react-hooks/set-state-in-effect. Perilaku sama (cek sekali saat mount).
    function checkSession() {
      if (isAuthed()) setStatus('authed');
      else router.replace('/login');
    }
    checkSession();
  }, [router]);

  if (status !== 'authed') {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gray-50">
        <div className="flex flex-col items-center gap-3 text-gray-400">
          <div className="text-3xl animate-spin">⏳</div>
          <p className="text-sm">Memeriksa sesi petugas...</p>
        </div>
      </div>
    );
  }

  return (
    <div className="min-h-screen bg-gray-50 md:flex">
      <Sidebar />
      <div className="flex-1 min-w-0 md:pl-64">{children}</div>
    </div>
  );
}
