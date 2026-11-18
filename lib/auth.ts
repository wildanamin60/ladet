// Auth sederhana berbasis kredensial tetap (demo skripsi).
// CATATAN: password disimpan di sisi client — hanya untuk demo, BUKAN untuk produksi.
const AUTH_KEY = 'ladet_auth';

export const VALID_USERNAME = '231910201014';
export const VALID_PASSWORD = 'Ladetlulusskrips1';

export function login(username: string, password: string): boolean {
  const ok = username.trim() === VALID_USERNAME && password === VALID_PASSWORD;
  if (ok && typeof window !== 'undefined') {
    window.localStorage.setItem(AUTH_KEY, '1');
  }
  return ok;
}

export function logout(): void {
  if (typeof window !== 'undefined') {
    window.localStorage.removeItem(AUTH_KEY);
  }
}

export function isAuthed(): boolean {
  if (typeof window === 'undefined') return false;
  return window.localStorage.getItem(AUTH_KEY) === '1';
}
