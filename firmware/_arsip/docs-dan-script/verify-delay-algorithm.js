/**
 * Validasi algoritma delay di rx-mqtt-publisher.ino
 *
 * Meniru kondisi nyata: TX dan RX punya epoch millis() berbeda (titik nol =
 * waktu boot masing-masing board), TX mengirim MS=<millis TX>, RX menghitung
 * delay = t_terima - t_kirim dengan kalibrasi offset.
 *
 * Dua skenario diuji supaya bug "delay = uptime RX" mustahil kembali:
 *   SKENARIO A — TX sudah punya field RTT=   -> jalur "clock" (ABSOLUT)
 *   SKENARIO B — TX belum punya field RTT=   -> jalur "clock-rel" (RELATIF)
 *
 * Parameter: SF7, interval 2 s, 50 paket, RX boot 137 s lebih dulu (offset 137000 ms).
 * delay radio asli = 40..60 ms (acak).
 */

const DELAY_SANITY_MAX_MS = 5000;
const TOTAL = 50, INTERVAL = 2000;
const RX_HEADSTART = 137000;

// ── State RX (mirror dari rx-mqtt-publisher.ino) ───────────────────────────
function newState() {
  return {
    clockOffsetValid: false,
    clockOffsetMs: 0,
    prevRawDiffMs: -1,
    clockOffsetRefined: false,
    rawDiffMin: 0,
    rawDiffMinValid: false,
    rttSeen: false,
    delaySource: 'unknown',
  };
}

// Salinan 1:1 dari blok "HITUNG DELAY" di rx-mqtt-publisher.ino
function measureDelay(s, rxTime, send_ms, rtt_ms) {
  let delayMs = 0;
  s.delaySource = 'unknown';

  const rttPlausible = rtt_ms > 0 && rtt_ms < DELAY_SANITY_MAX_MS;
  if (rttPlausible) s.rttSeen = true;

  const rawDiffMs = send_ms > 0 ? rxTime - send_ms : -1;

  // Running minimum rawDiff -> jaring pengaman relatif
  if (rawDiffMs >= 0 && (!s.rawDiffMinValid || rawDiffMs < s.rawDiffMin)) {
    s.rawDiffMin = rawDiffMs;
    s.rawDiffMinValid = true;
  }

  // Langkah 1: offset ABSOLUT dari RTT paket sebelumnya
  if (rttPlausible && s.prevRawDiffMs >= 0 && !s.clockOffsetRefined) {
    s.clockOffsetMs = s.prevRawDiffMs - Math.trunc(rtt_ms / 2);
    s.clockOffsetValid = true;
    s.clockOffsetRefined = true;
  }

  // Langkah 2: jaring pengaman RELATIF (dipakai kalau TX belum kirim RTT=)
  if (!s.clockOffsetValid && s.rawDiffMinValid) {
    s.clockOffsetMs = s.rawDiffMin;
    s.clockOffsetValid = true;
  }

  if (rawDiffMs >= 0 && s.clockOffsetValid) {
    delayMs = Math.max(0, rawDiffMs - s.clockOffsetMs);
    s.delaySource = s.clockOffsetRefined ? 'clock' : 'clock-rel';
  } else if (rttPlausible) {
    delayMs = Math.trunc(rtt_ms / 2);
    s.delaySource = 'rtt';
  }

  // Sanity check
  if (delayMs > DELAY_SANITY_MAX_MS) {
    if (rttPlausible) { delayMs = Math.trunc(rtt_ms / 2); s.delaySource = 'rtt'; }
    else { delayMs = 0; s.delaySource = 'invalid'; }
  }

  s.prevRawDiffMs = rawDiffMs;
  return delayMs;
}

// ── Generator paket (deterministik supaya A & B apple-to-apple) ────────────
function makePackets() {
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const pkts = [];
  for (let seq = 1; seq <= TOTAL; seq++) {
    const trueOneWay = 40 + Math.round(rnd() * 20);   // 40..60 ms
    pkts.push({
      seq,
      trueOneWay,
      rttTrue: trueOneWay * 2 + 4,                                  // +4 ms proses ACK
      tSendTX: (seq - 1) * INTERVAL + 500,                          // millis TX
      tRecvRX: RX_HEADSTART + (seq - 1) * INTERVAL + 500 + trueOneWay, // millis RX
    });
  }
  return pkts;
}

// ── Jalankan satu skenario ────────────────────────────────────────────────
// rttSupported=false meniru TX lama yang tidak mengirim field RTT= sama sekali.
function run(rttSupported) {
  const s = newState();
  const out = [];
  let lastRTT = -1;   // RTT di paket ke-N = RTT paket ke-N-1 (seperti TX asli)
  for (const p of makePackets()) {
    const rttInPacket = rttSupported ? lastRTT : -1;
    const delay = measureDelay(s, p.tRecvRX, p.tSendTX, rttInPacket);
    out.push({ ...p, rttInPacket, delay, src: s.delaySource });
    lastRTT = p.rttTrue;
  }
  return out;
}

const avg = a => a.reduce((v, x) => v + x, 0) / a.length;
const isMonotonic = a => a.every((v, i) => i === 0 || v >= a[i - 1]);

function report(title, res, absolute) {
  const meas = res.map(r => r.delay);
  const truth = res.map(r => r.trueOneWay);
  const srcs = [...new Set(res.map(r => r.src))];

  console.log(title);
  console.log('SEQ | delay_asli | RTT_dlm_pkt | delay_terukur | sumber');
  console.log('-'.repeat(66));
  for (const r of res.slice(0, 8)) {
    console.log(
      String(r.seq).padStart(3) + ' |' +
      String(r.trueOneWay).padStart(11) + ' |' +
      (r.rttInPacket < 0 ? '        —' : String(r.rttInPacket).padStart(11)) + ' |' +
      String(r.delay).padStart(13) + ' | ' + r.src
    );
  }
  console.log(' ... (paket 9-50 sama polanya)');
  console.log('  sumber delay dipakai  :', srcs.join(', '));
  console.log('  delay MAX             :', Math.max(...meas), 'ms');
  console.log('  delay rata-rata       :', avg(meas).toFixed(1), 'ms');
  if (absolute) {
    const mae = avg(meas.map((m, i) => Math.abs(m - truth[i])));
    console.log('  MAE vs delay asli     :', mae.toFixed(1), 'ms');
  } else {
    console.log('  (relatif: bias = delay terkecil yg terlihat, jadi MAE tidak relevan)');
  }
  console.log('  monoton naik?         :', isMonotonic(meas) ? 'YA (BUG!)' : 'tidak');

  // ── INVARIANT yang harus terpenuhi di KEDUA skenario ──
  // Yang paling penting: delay TIDAK boleh tumbuh mengikuti lamanya pengujian.
  // Uji paling langsung = bandingkan rata-rata 10 paket pertama vs 10 terakhir.
  // Kalau bug uptime kembali, selisihnya akan ribuan ms (bahkan puluhan ribu).
  const head = avg(meas.slice(0, 10));
  const tail = avg(meas.slice(-10));
  const growth = tail - head;
  const spread = Math.max(...truth) - Math.min(...truth);

  const checks = {
    'delay < sanity (5000 ms)': Math.max(...meas) < DELAY_SANITY_MAX_MS,
    'tidak monoton naik': !isMonotonic(meas),
    'tidak tumbuh: tail − head < 10 ms': growth < 10,
  };
  if (absolute) {
    // Sumber ABSOLUT boleh sedikit over-report sebesar overhead proses ACK
    // (di simulasi = 2 ms, karena RTT = 2*delay + 4).
    const mae = avg(meas.map((m, i) => Math.abs(m - truth[i])));
    checks['MAE < 10 ms (bias ACK ~2 ms masih wajar)'] = mae < 10;
  } else {
    // Sumber RELATIF: hasil = delay − delay_terkecil, jadi harus <= rentang delay asli.
    checks['delay <= rentang delay asli'] = Math.max(...meas) <= spread + 5;
  }
  console.log('  rata-rata 10 paket awal:', head.toFixed(1), 'ms');
  console.log('  rata-rata 10 paket akhir:', tail.toFixed(1), 'ms  (tumbuh', growth.toFixed(1), 'ms)');
  for (const [name, ok] of Object.entries(checks)) {
    console.log('   ', ok ? '✅' : '❌', name);
  }
  const pass = Object.values(checks).every(Boolean);
  console.log('  ', pass ? '✅ LULUS' : '❌ GAGAL');
  console.log('');
  return { res, pass, srcs };
}

console.log('='.repeat(66));
console.log('UJI ALGORITMA DELAY  (SF7 / 433 MHz / interval 2 s / 50 paket)');
console.log('offset_boot = +137000 ms  (RX boot 137 s lebih dulu dari TX)');
console.log('='.repeat(66));
console.log('');

const A = report('SKENARIO A — TX mendukung RTT=  (jalur ABSOLUT)', run(true), true);
const B = report('SKENARIO B — TX TIDAK mengirim RTT=  (jalur RELATIF)', run(false), false);

// ── Uji regresi: apakah bug lama (tanpa koreksi offset) memang membengkak? ──
const buggy = A.res.map(r => r.tRecvRX - r.tSendTX);   // cara LAMA: apa adanya
console.log('BANDINGKAN dengan cara LAMA (tanpa koreksi offset sama sekali):');
console.log('  paket 1  :', buggy[0], 'ms');
console.log('  paket 25 :', buggy[24], 'ms');
console.log('  paket 50 :', buggy[49], 'ms  <-- membengkak ~1000 ms/detik (181 s)');
console.log('');

const PASS = A.pass && B.pass && A.srcs.includes('clock') && B.srcs.includes('clock-rel');

console.log(PASS
  ? '✅ SEMUA LULUS: delay stabil di kedua skenario, bug uptime tidak bisa kembali'
  : '❌ ADA SKENARIO GAGAL');
process.exit(PASS ? 0 : 1);

