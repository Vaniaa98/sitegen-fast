// fastlib.mjs — fast, byte-identical re-implementations of the makefun-site
// primitives (text.js, frontmatter.js, markdown.js inline/block). Used to
// differentially test against the reference before wiring into solve.js.

export const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };

// ---------- escapeHtml ----------
const ESC_RE = /[&<>"']/;
export function escapeHtml(t) {
  if (!ESC_RE.test(t)) return t;
  let out = null;
  let last = 0;
  for (let i = 0; i < t.length; i++) {
    const c = t.charCodeAt(i);
    let rep;
    if (c === 38) rep = '&amp;';
    else if (c === 60) rep = '&lt;';
    else if (c === 62) rep = '&gt;';
    else if (c === 34) rep = '&quot;';
    else if (c === 39) rep = '&#39;';
    else continue;
    if (out === null) out = [];
    out.push(t.slice(last, i), rep);
    last = i + 1;
  }
  if (out === null) return t;
  out.push(t.slice(last));
  return out.join('');
}

// ---------- slugify ----------
const NON_ASCII = /[^\x00-\x7F]/;
const SLUG_MAX = 64;
function slugAscii(text) {
  let out = '';
  let gap = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    let keep = 0;
    if (c >= 65 && c <= 90) keep = c + 32; // A-Z -> a-z
    else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) keep = c;
    if (keep === 0) { gap = out.length > 0; continue; }
    if (gap) out += '-';
    gap = false;
    out += String.fromCharCode(keep);
  }
  out = out.slice(0, SLUG_MAX);
  // drop trailing '-'
  let e = out.length;
  while (e > 0 && out.charCodeAt(e - 1) === 45) e--;
  out = out.slice(0, e);
  return out === '' ? 'untitled' : out;
}
function slugGeneral(text) {
  // reference logic, for non-ASCII input
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (let i = 0; i < plain.length; i++) {
    const c = plain.charCodeAt(i);
    let keep = 0;
    if (c >= 65 && c <= 90) keep = c + 32;
    else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) keep = c;
    if (keep === 0) { gap = out.length > 0; continue; }
    if (gap) out += '-';
    gap = false;
    out += String.fromCharCode(keep);
  }
  out = out.slice(0, SLUG_MAX);
  let e = out.length;
  while (e > 0 && out.charCodeAt(e - 1) === 45) e--;
  out = out.slice(0, e);
  return out === '' ? 'untitled' : out;
}
export function makeSlugify() {
  const cache = new Map();
  return function slugify(text) {
    let v = cache.get(text);
    if (v !== undefined) return v;
    v = NON_ASCII.test(text) ? slugGeneral(text) : slugAscii(text);
    cache.set(text, v);
    return v;
  };
}

export function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
export function longDate(date) {
  const m = (date.charCodeAt(5) - 48) * 10 + (date.charCodeAt(6) - 48);
  const d = (date.charCodeAt(8) - 48) * 10 + (date.charCodeAt(9) - 48);
  return d + ' ' + MONTHS[m - 1] + ' ' + date.slice(0, 4);
}

// ---------- wordsOf ----------
// ASCII whitespace lookup (single array access); non-ASCII falls back to the full set.
const WS_ASCII = new Uint8Array(128);
for (const c of [32, 9, 10, 11, 12, 13]) WS_ASCII[c] = 1;
function isSpaceNonAscii(c) {
  return c === 0xA0 || c === 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
    c === 0x2028 || c === 0x2029 || c === 0x202F || c === 0x205F ||
    c === 0x3000 || c === 0xFEFF;
}
export function wordsOf(text) {
  let n = 0;
  let inWord = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    const sp = c < 128 ? WS_ASCII[c] === 1 : isSpaceNonAscii(c);
    if (sp) inWord = false;
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

// ---------- front matter ----------
export const KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const KEYSET = new Set(KEYS);

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

function trimEndST(s) {
  let e = s.length;
  while (e > 0 && (s.charCodeAt(e - 1) === 32 || s.charCodeAt(e - 1) === 9)) e--;
  return s.slice(0, e);
}
function trimJS(s) {
  // String.prototype.trim
  return s.trim();
}

// string value: returns {value} or {error}
function stringOf(raw) {
  if (raw.charCodeAt(0) === 34) { // '"'
    if (raw.length < 2 || raw.charCodeAt(raw.length - 1) !== 34) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' }; // '['
  return { value: raw };
}

function isDigit(c) { return c >= 48 && c <= 57; }

function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      // ^(\d{4})-(\d{2})-(\d{2})$
      let ok = raw.length === 10;
      if (ok) {
        for (let i = 0; i < 4; i++) if (!isDigit(raw.charCodeAt(i))) { ok = false; break; }
        if (ok && raw.charCodeAt(4) !== 45) ok = false;
        if (ok) for (let i = 5; i < 7; i++) if (!isDigit(raw.charCodeAt(i))) { ok = false; break; }
        if (ok && raw.charCodeAt(7) !== 45) ok = false;
        if (ok) for (let i = 8; i < 10; i++) if (!isDigit(raw.charCodeAt(i))) { ok = false; break; }
      }
      const y = ok ? (raw.charCodeAt(0) - 48) * 1000 + (raw.charCodeAt(1) - 48) * 100 + (raw.charCodeAt(2) - 48) * 10 + (raw.charCodeAt(3) - 48) : 0;
      const mo = ok ? (raw.charCodeAt(5) - 48) * 10 + (raw.charCodeAt(6) - 48) : 0;
      const d = ok ? (raw.charCodeAt(8) - 48) * 10 + (raw.charCodeAt(9) - 48) : 0;
      if (!ok || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (raw.length < 2 || raw.charCodeAt(0) !== 91 || raw.charCodeAt(raw.length - 1) !== 93) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (trimJS(inner) === '') return { value: [] };
      const tags = inner.split(',').map((t) => trimJS(t));
      for (const t of tags) {
        if (t === '') return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
        for (let i = 0; i < t.length; i++) {
          const c = t.charCodeAt(i);
          if (c === 34 || c === 91 || c === 93) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
        }
      }
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout': {
      if (raw.length === 0) return { error: 'key "layout": not a template name' };
      for (let i = 0; i < raw.length; i++) {
        const c = raw.charCodeAt(i);
        if (!((c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 45)) return { error: 'key "layout": not a template name' };
      }
      return { value: raw };
    }
    case 'slug': {
      // /^[a-z0-9]+(?:-[a-z0-9]+)*$/ and length <= 64
      if (raw.length === 0 || raw.length > 64) return { error: 'key "slug": not a slug' };
      let i = 0;
      const n = raw.length;
      while (i < n) {
        // one segment [a-z0-9]+
        let j = i;
        while (j < n) {
          const c = raw.charCodeAt(j);
          if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) j++;
          else break;
        }
        if (j === i) return { error: 'key "slug": not a slug' };
        i = j;
        if (i === n) break;
        if (raw.charCodeAt(i) !== 45) return { error: 'key "slug": not a slug' };
        i++;
        if (i === n) return { error: 'key "slug": not a slug' }; // trailing '-'
      }
      return { value: raw };
    }
    default:
      return { error: `unknown key "${key}"` };
  }
}

// Parse one front-matter line "key: value" (no \n). Returns {key, raw} or null (LINE fails).
function parseFMline(line) {
  let i = 0;
  const n = line.length;
  while (i < n) {
    const c = line.charCodeAt(i);
    if ((c >= 97 && c <= 122) || c === 95) i++; // a-z or _
    else break;
  }
  if (i === 0) return null;
  const key = line.slice(0, i);
  if (i >= n || line.charCodeAt(i) !== 58) return null; // ':'
  let raw;
  if (i + 1 < n && line.charCodeAt(i + 1) === 32) raw = line.slice(i + 2);
  else if (i + 1 === n) raw = '';
  else return null;
  return { key, raw: trimEndST(raw) };
}

function isBlankLine(line) {
  for (let i = 0; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** Parse front matter. `text` has "\n" line ends. Returns {data, body} or {error}. */
export function parseFrontMatter(text) {
  // first line must be '---'
  const nl0 = text.indexOf('\n');
  const first = nl0 === -1 ? text : text.slice(0, nl0);
  if (first !== '---') return { error: 'no front matter' };
  // find closing '---' line: scan lines starting after the first line
  let pos = nl0 === -1 ? text.length : nl0 + 1; // start of line 1 (0-based)
  let lineNo = 1; // 0-based index of current line
  let closePos = -1; // start index of the closing '---' line
  let closeLineNo = -1;
  const data = {};
  // We need to parse lines 1..close-1 for keys. Do a single scan:
  // first find the close line, then parse the lines in between.
  // Find close: a line exactly '---'.
  let p = pos;
  while (p <= text.length) {
    const nl = text.indexOf('\n', p);
    const lineEnd = nl === -1 ? text.length : nl;
    const line = text.slice(p, lineEnd);
    if (line === '---') { closePos = p; closeLineNo = lineNo; break; }
    lineNo++;
    if (nl === -1) break;
    p = nl + 1;
  }
  if (closePos === -1) return { error: 'the front matter is not closed' };
  // parse lines 1 .. closeLineNo-1
  p = pos;
  for (let ln = 1; ln < closeLineNo; ln++) {
    const nl = text.indexOf('\n', p);
    const lineEnd = nl === -1 ? text.length : nl;
    const line = text.slice(p, lineEnd);
    if (isBlankLine(line) || (line.length > 0 && line.charCodeAt(0) === 35)) { p = lineEnd + 1; continue; } // '#' comment
    const kv = parseFMline(line);
    if (kv === null) return { error: `line ${ln + 1} is not "key: value"` };
    const { key, raw } = kv;
    if (!KEYSET.has(key)) return { error: `unknown key "${key}"` };
    if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = valueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
    p = lineEnd + 1;
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  // body = lines after close line
  const closeNl = text.indexOf('\n', closePos);
  const body = closeNl === -1 ? '' : text.slice(closeNl + 1);
  return { data, body };
}
