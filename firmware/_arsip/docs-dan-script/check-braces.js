// Pemeriksa keseimbangan tanda kurung pada file .ino
// Mengabaikan komentar (// dan /* */) serta string/char literal.
const fs = require('fs');
const path = process.argv[2];
const src = fs.readFileSync(path, 'utf8');

let i = 0, line = 1;
const stack = [];
const pairs = { ')': '(', '}': '{', ']': '[' };
const opens = { '(': ')', '{': '}', '[': ']' };
let inLine = false, inBlock = false, inStr = false, inChr = false, esc = false;
const errors = [];

while (i < src.length) {
  const c = src[i], n = src[i + 1];
  const atLineStart = () => false;

  if (c === '\n') { line++; inLine = false; i++; continue; }
  if (inLine) { i++; continue; }
  if (inBlock) { if (c === '*' && n === '/') { inBlock = false; i += 2; continue; } i++; continue; }
  if (inStr || inChr) {
    if (esc) { esc = false; i++; continue; }
    if (c === '\\') { esc = true; i++; continue; }
    if ((inStr && c === '"') || (inChr && c === "'")) { inStr = false; inChr = false; i++; continue; }
    i++; continue;
  }

  if (c === '/' && n === '/') { inLine = true; i += 2; continue; }
  if (c === '/' && n === '*') { inBlock = true; i += 2; continue; }
  if (c === '"') { inStr = true; i++; continue; }
  if (c === "'") { inChr = true; i++; continue; }

  if (opens[c]) { stack.push({ ch: c, line }); i++; continue; }
  if (pairs[c]) {
    const top = stack.pop();
    if (!top || top.ch !== pairs[c]) {
      errors.push(`baris ${line}: '${c}' tidak berpasangan${top ? ` (puncak stack: '${top.ch}' dari baris ${top.line})` : ''}`);
    }
    i++; continue;
  }
  i++;
}

for (const s of stack) errors.push(`baris ${s.line}: '${s.ch}' tidak pernah ditutup`);

if (errors.length) {
  console.log('❌ MASALAH STRUKTUR:');
  for (const e of errors) console.log('   -', e);
  process.exit(1);
}
console.log('✅ Seimbang: kurung, brace, dan bracket cocok (komentar & string diabaikan)');
process.exit(0);
