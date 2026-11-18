'use client';

import { useState } from 'react';
import { useRouter } from 'next/navigation';
import Link from 'next/link';
import { login } from '@/lib/auth';

export default function LoginPage() {
  const router = useRouter();
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [showPassword, setShowPassword] = useState(false);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);

  function handleSubmit(e: React.FormEvent) {
    e.preventDefault();
    setLoading(true);
    setError('');
    // sedikit delay agar terasa seperti proses autentikasi
    setTimeout(() => {
      const ok = login(username, password);
      if (!ok) {
        setError('Username atau kata sandi salah.');
        setLoading(false);
        return;
      }
      router.replace('/dashboard');
    }, 400);
  }

  return (
    <main className="min-h-screen bg-gray-50 flex items-center justify-center px-4">
      <div className="w-full max-w-sm">
        <div className="text-center mb-6">
          <div className="text-4xl mb-2">🏔</div>
          <h1 className="text-xl font-black text-gray-800">LADET</h1>
          <p className="text-xs text-gray-400">Panel Engineer — Login</p>
        </div>

        <form onSubmit={handleSubmit}
          className="bg-white rounded-2xl border border-gray-200 shadow-sm p-6 flex flex-col gap-4">
          <div className="flex flex-col gap-1">
            <label className="text-xs font-semibold text-gray-600">Username</label>
            <input type="text" value={username} onChange={e => setUsername(e.target.value)} required
              autoComplete="username"
              placeholder="Username"
              className="rounded-xl border border-gray-200 bg-gray-50 px-3 py-2.5 text-sm
                         focus:outline-none focus:ring-2 focus:ring-green-400 transition" />
          </div>

          <div className="flex flex-col gap-1">
            <label className="text-xs font-semibold text-gray-600">Kata Sandi</label>
            <div className="relative">
              <input type={showPassword ? 'text' : 'password'} value={password} onChange={e => setPassword(e.target.value)} required
                autoComplete="current-password"
                placeholder="••••••••"
                className="w-full rounded-xl border border-gray-200 bg-gray-50 pl-3 pr-12 py-2.5 text-sm
                           focus:outline-none focus:ring-2 focus:ring-green-400 transition" />
              <button type="button" onClick={() => setShowPassword(v => !v)}
                className="absolute right-2 top-1/2 -translate-y-1/2 px-2 py-1 text-gray-400 hover:text-gray-700 transition-colors"
                aria-label={showPassword ? 'Sembunyikan kata sandi' : 'Tampilkan kata sandi'}>
                {showPassword ? '🙈' : '👁️'}
              </button>
            </div>
          </div>

          {error && (
            <div className="bg-red-50 border border-red-200 rounded-xl p-3 text-sm text-red-700">{error}</div>
          )}

          <button type="submit" disabled={loading}
            className="py-3 rounded-xl bg-green-600 text-white font-bold text-sm shadow-sm
                       disabled:opacity-60 hover:bg-green-700 active:scale-95 transition">
            {loading ? 'Memproses...' : 'Masuk'}
          </button>
        </form>

        <p className="text-center text-[11px] text-gray-400 mt-4">
          <Link href="/" className="hover:underline">← Kembali ke Beranda Warga</Link>
        </p>
      </div>
    </main>
  );
}
