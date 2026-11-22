// fastmd.mjs — fast, byte-identical Markdown renderer (single pass, no tokens).
import { escapeHtml, uniqueSlug } from './fastlib.mjs';

function isSpaceCode(c) {
  return c === 32 || c === 9 || c === 10 || c === 11 || c === 12 || c === 13 ||
    c === 0xA0 || c === 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
    c === 0x2028 || c === 0x2029 || c === 0x202F || c === 0x205F ||
    c === 0x3000 || c === 0xFEFF;
}

function isBlank(line) {
  for (let i = 0; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

function trimLine(line) {
  let s = 0, e = line.length;
  while (s < e && (line.charCodeAt(s) === 32 || line.charCodeAt(s) === 9)) s++;
  while (e > s && (line.charCodeAt(e - 1) === 32 || line.charCodeAt(e - 1) === 9)) e--;
  return line.slice(s, e);
}

// FENCE_OPEN: ``` + lang[A-Za-z0-9_+-]* + spaces/tabs only. Returns lang or null.
function fenceOpen(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return null;
  let i = 3;
  while (i < line.length) {
    const c = line.charCodeAt(i);
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) i++;
    else break;
  }
  const lang = line.slice(3, i);
  while (i < line.length) {
    const c = line.charCodeAt(i);
    if (c === 32 || c === 9) i++;
    else return null;
  }
  return lang;
}

function isFenceClose(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

// HEADING: 1-6 '#' + spaces/tabs + text (trailing spaces/tabs stripped). Null if no text.
function headingOf(line) {
  let i = 0;
  while (i < line.length && line.charCodeAt(i) === 35) i++;
  if (i === 0 || i > 6) return null;
  if (i >= line.length) return null;
  let c = line.charCodeAt(i);
  if (c !== 32 && c !== 9) return null;
  let j = i;
  while (j < line.length && (line.charCodeAt(j) === 32 || line.charCodeAt(j) === 9)) j++;
  let e = line.length;
  while (e > j && (line.charCodeAt(e - 1) === 32 || line.charCodeAt(e - 1) === 9)) e--;
  if (e === j) return null;
  return { depth: i, text: line.slice(j, e) };
}

function isHR(line) {
  if (line.length < 3) return false;
  const c0 = line.charCodeAt(0), c1 = line.charCodeAt(1), c2 = line.charCodeAt(2);
  if (!((c0 === 45 && c1 === 45 && c2 === 45) || (c0 === 42 && c1 === 42 && c2 === 42))) return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

// BULLET: '- ' or '* ' + rest. Returns rest or null.
function bulletMatch(line) {
  if (line.length < 2) return null;
  const c0 = line.charCodeAt(0);
  if ((c0 === 45 || c0 === 42) && line.charCodeAt(1) === 32) return line.slice(2);
  return null;
}

// ORDERED: 1-9 digits + '. ' + rest. Returns {num, content} or null.
function orderedMatch(line) {
  let i = 0;
  while (i < line.length && i < 9) {
    const c = line.charCodeAt(i);
    if (c >= 48 && c <= 57) i++;
    else break;
  }
  if (i === 0) return null;
  if (i + 1 >= line.length) return null;
  if (line.charCodeAt(i) !== 46 || line.charCodeAt(i + 1) !== 32) return null;
  return { num: line.slice(0, i), content: line.slice(i + 2) };
}

// CONTINUED: 2+ spaces then a non-whitespace char.
function isContinued(line) {
  let i = 0;
  while (i < line.length && line.charCodeAt(i) === 32) i++;
  if (i < 2) return false;
  if (i >= line.length) return false;
  return !isSpaceCode(line.charCodeAt(i));
}

function endsParagraph(line) {
  return isBlank(line) || fenceOpen(line) !== null || headingOf(line) !== null || isHR(line);
}

// ---------- inline ----------
const ESCAPABLE = new Set([92, 96, 42, 95, 91, 93, 40, 41, 35, 43, 45, 46, 33, 62]); // char codes of \ ` * _ [ ] ( ) # + - . ! >
function isEscapableCode(c) { return ESCAPABLE.has(c); }

// First index >= from where `what` occurs outside a backslash escape, or -1.
// Equivalent to the reference's findUnescaped (forward scan skipping \X pairs).
function findUnescaped(s, what, from) {
  let j = s.indexOf(what, from);
  while (j !== -1) {
    let k = j - 1, n = 0;
    while (k >= from && s.charCodeAt(k) === 92) { n++; k--; }
    if (n % 2 === 0) return j;
    j = s.indexOf(what, j + 1);
  }
  return -1;
}

// First single '*' from `from` (one whose run is not '**'), outside an escape, or -1.
function findSingleStar(s, from) {
  let j = s.indexOf('*', from);
  while (j !== -1) {
    let k = j - 1, n = 0;
    while (k >= from && s.charCodeAt(k) === 92) { n++; k--; }
    if (n % 2 === 1) { j = s.indexOf('*', j + 1); continue; } // escaped
    if (s.charCodeAt(j + 1) === 42) { j = s.indexOf('*', j + 2); continue; } // part of '**'
    return j;
  }
  return -1;
}

function hasWhitespace(s) {
  for (let i = 0; i < s.length; i++) if (isSpaceCode(s.charCodeAt(i))) return true;
  return false;
}

/**
 * Append the inline rendering of `s` to `out` (array of strings).
 * Regex-driven: a single /g scan finds the next special char (C++), instead of
 * dispatching every character in JS. A fresh regex per call (recursion-safe:
 * strong/em/link content recurses into inlineInto).
 * Failure of a construct leaves its chars in the pending plain run (they are
 * emitted by the next slice), matching the reference's literal push.
 */
export function inlineInto(s, env, out) {
  const re = /[\\`*\[&<>"']/g;
  let lastIndex = 0;
  let m;
  while ((m = re.exec(s)) !== null) {
    const i = m.index;
    const c = s.charCodeAt(i);
    if (c === 92) { // backslash
      if (i > lastIndex) out.push(s.slice(lastIndex, i));
      if (i + 1 < s.length && isEscapableCode(s.charCodeAt(i + 1))) {
        const nc = s.charCodeAt(i + 1);
        out.push(nc === 62 ? '&gt;' : s[i + 1]);
        re.lastIndex = i + 2;
        lastIndex = i + 2;
      } else {
        out.push('\\');
        re.lastIndex = i + 1;
        lastIndex = i + 1;
      }
      continue;
    }
    if (c === 96) { // backtick
      const j = s.indexOf('`', i + 1);
      if (j <= i + 1) { re.lastIndex = i + 1; continue; } // literal ` stays in plain run
      if (i > lastIndex) out.push(s.slice(lastIndex, i));
      out.push('<code>', escapeHtml(s.slice(i + 1, j)), '</code>');
      re.lastIndex = j + 1;
      lastIndex = j + 1;
      continue;
    }
    if (c === 42) { // *
      if (s.charCodeAt(i + 1) === 42) { // **
        const j = findUnescaped(s, '**', i + 2);
        if (j <= i + 2) { re.lastIndex = i + 2; continue; } // literal ** stays in plain run
        if (i > lastIndex) out.push(s.slice(lastIndex, i));
        out.push('<strong>');
        inlineInto(s.slice(i + 2, j), env, out);
        out.push('</strong>');
        re.lastIndex = j + 2;
        lastIndex = j + 2;
        continue;
      }
      const j = findSingleStar(s, i + 1);
      if (j <= i + 1) { re.lastIndex = i + 1; continue; } // literal * stays in plain run
      if (i > lastIndex) out.push(s.slice(lastIndex, i));
      out.push('<em>');
      inlineInto(s.slice(i + 1, j), env, out);
      out.push('</em>');
      re.lastIndex = j + 1;
      lastIndex = j + 1;
      continue;
    }
    if (c === 91) { // [
      const k = findUnescaped(s, ']', i + 1);
      const mm = (k !== -1 && s.charCodeAt(k + 1) === 40) ? s.indexOf(')', k + 2) : -1;
      const target = mm === -1 ? '' : s.slice(k + 2, mm);
      if (target === '' || hasWhitespace(target)) { re.lastIndex = i + 1; continue; } // literal [ stays in plain run
      if (i > lastIndex) out.push(s.slice(lastIndex, i));
      out.push('<a href="', escapeHtml(env.link(target)), '">');
      inlineInto(s.slice(i + 1, k), env, out);
      out.push('</a>');
      re.lastIndex = mm + 1;
      lastIndex = mm + 1;
      continue;
    }
    // HTML-escape char (& < > " ')
    if (i > lastIndex) out.push(s.slice(lastIndex, i));
    out.push(c === 38 ? '&amp;' : c === 60 ? '&lt;' : c === 62 ? '&gt;' : c === 34 ? '&quot;' : '&#39;');
    re.lastIndex = i + 1;
    lastIndex = i + 1;
  }
  if (s.length > lastIndex) out.push(s.slice(lastIndex));
}

export function inline(s, env) {
  const out = [];
  inlineInto(s, env, out);
  return out.join('');
}

// Count the words (runs of non-whitespace, JS whitespace) in `lines`.
// A word never spans lines ('\n' is whitespace), so this equals wordsOf
// of the joined body. Used instead of a separate wordsOf pass over the body:
// the lines are already in memory from the split.
function countWordsLines(lines) {
  let words = 0;
  for (let li = 0; li < lines.length; li++) {
    const line = lines[li];
    let inWord = false;
    for (let i = 0; i < line.length; i++) {
      const c = line.charCodeAt(i);
      const sp = c < 128 ? WS_SPLIT[c] === 1 : isSpaceNonAscii(c);
      if (sp) inWord = false;
      else if (!inWord) { inWord = true; words++; }
    }
  }
  return words;
}

// ASCII whitespace lookup for the word-count loop (same set as wordsOf).
const WS_SPLIT = new Uint8Array(128);
for (const c of [32, 9, 10, 11, 12, 13]) WS_SPLIT[c] = 1;
function isSpaceNonAscii(c) {
  return c === 0xA0 || c === 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
    c === 0x2028 || c === 0x2029 || c === 0x202F || c === 0x205F ||
    c === 0x3000 || c === 0xFEFF;
}

/**
 * Render Markdown src -> { pieces, toc, excerpt, words }, byte-identical to the reference.
 * `pieces` is the output array (NOT joined) so the caller can splice it into a
 * shared sink instead of paying a full join. `slugify` is the (memoized) slug function.
 */
export function renderMarkdown(src, env, slugify) {
  const lines = src.split('\n');
  const words = countWordsLines(lines);
  const out = [];
  const toc = [];
  const ids = new Set();
  let excerpt = null;
  let i = 0;
  const n = lines.length;
  while (i < n) {
    const line = lines[i];
    if (isBlank(line)) { i++; continue; }
    const lang = fenceOpen(line);
    if (lang !== null) {
      i++;
      const bodyLines = [];
      while (i < n && !isFenceClose(lines[i])) bodyLines.push(lines[i++]);
      i++;
      out.push('<pre><code');
      if (lang !== '') { out.push(' class="language-', escapeHtml(lang), '"'); }
      out.push('>');
      for (const l of bodyLines) { out.push(escapeHtml(l), '\n'); }
      out.push('</code></pre>\n');
      continue;
    }
    const hd = headingOf(line);
    if (hd !== null) {
      const id = uniqueSlug(slugify(hd.text), ids);
      if (hd.depth === 2 || hd.depth === 3) {
        const hOut = [];
        inlineInto(hd.text, env, hOut);
        const html = hOut.join('');
        out.push('<h', String(hd.depth), ' id="', id, '">', html, '</h', String(hd.depth), '>\n');
        toc.push({ level: hd.depth, id, html });
      } else {
        out.push('<h', String(hd.depth), ' id="', id, '">');
        inlineInto(hd.text, env, out);
        out.push('</h', String(hd.depth), '>\n');
      }
      i++;
      continue;
    }
    if (isHR(line)) { out.push('<hr>\n'); i++; continue; }
    if (line.charCodeAt(0) === 62) { // '>'
      const parts = [];
      while (i < n && lines[i].charCodeAt(0) === 62) {
        let l = lines[i++];
        l = l.charCodeAt(1) === 32 ? l.slice(2) : l.slice(1);
        l = trimLine(l);
        if (l !== '') parts.push(l);
      }
      if (parts.length === 0) {
        out.push('<blockquote>\n</blockquote>\n');
      } else {
        out.push('<blockquote>\n<p>');
        inlineInto(parts.join('\n'), env, out);
        out.push('</p>\n</blockquote>\n');
      }
      continue;
    }
    const bullet = bulletMatch(line);
    const ordered = bullet === null ? orderedMatch(line) : null;
    if (bullet !== null || ordered !== null) {
      const isOrdered = ordered !== null;
      const items = [];
      let start = 1;
      if (isOrdered) start = Number(ordered.num);
      items.push([trimLine(isOrdered ? ordered.content : bullet)]);
      i++;
      while (i < n) {
        const l2 = lines[i];
        if (isOrdered) {
          const m2 = orderedMatch(l2);
          if (m2 !== null) { items.push([trimLine(m2.content)]); i++; continue; }
        } else {
          const m2 = bulletMatch(l2);
          if (m2 !== null) { items.push([trimLine(m2)]); i++; continue; }
        }
        if (isContinued(l2)) { items[items.length - 1].push(trimLine(l2)); i++; }
        else break;
      }
      const tag = isOrdered ? 'ol' : 'ul';
      if (isOrdered && start !== 1) out.push('<ol start="', String(start), '">\n');
      else out.push('<', tag, '>\n');
      for (const parts of items) {
        out.push('<li>');
        inlineInto(parts.join('\n'), env, out);
        out.push('</li>\n');
      }
      out.push('</', tag, '>\n');
      continue;
    }
    // paragraph (only the first needs its HTML as a string, for the excerpt)
    const parts = [trimLine(line)];
    i++;
    while (i < n && !endsParagraph(lines[i])) parts.push(trimLine(lines[i++]));
    if (excerpt === null) {
      const pOut = [];
      inlineInto(parts.join('\n'), env, pOut);
      const html = pOut.join('');
      excerpt = html;
      out.push('<p>', html, '</p>\n');
    } else {
      out.push('<p>');
      inlineInto(parts.join('\n'), env, out);
      out.push('</p>\n');
    }
  }
  return { pieces: out, toc, excerpt: excerpt ?? '', words };
}
