/* main.c — site-generator-speed v10 (ground-up rewrite, single file).
 * Freestanding C -> WASM (<20 KB module target). Mirrors makefun-site
 * 1.0.0 semantics exactly (verified byte-identical vs the reference).
 *
 * Differences vs v9 (the point of the rewrite):
 *  - ONE translation unit: no per-module headers, everything static/inlined.
 *  - Fused Markdown: no Token structs, blocks render straight to the HTML
 *    buffer; paragraph/quote/list lines are joined once in the arena.
 *  - Templates compile to FLAT instructions (no Node trees, no recursion
 *    in the interpreter) with field names interned to integer ids at
 *    compile time (no string compares in the hot loop).
 *  - NO fragment cache, NO dependency analysis (direct-slot rendering +
 *    interned fields make re-rendering cheap; the cache cost ~400 lines).
 *  - No Sink abstraction: three plain byte buffers (arena / html / out)
 *    with direct put/escape helpers (no runtime kind dispatch).
 *  - Compact NFKD table (57 entries, generator alphabet closure only).
 */
#include <stdint.h>
#include <stddef.h>
#include "nfkd.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ------------------------------------------------------------------ */
/* Str + Buf                                                          */
/* ------------------------------------------------------------------ */
typedef struct { const uint8_t *p; size_t len; } Str;
static inline Str S(const uint8_t *p, size_t n) { Str s; s.p = p; s.len = n; return s; }
static inline Str SC(const char *c) {
    Str s; s.p = (const uint8_t *)c; size_t n = 0; while (c[n]) n++; s.len = n; return s;
}
static inline int seq(Str a, Str b) {
    if (a.len != b.len) return 0;
    if (a.len == 0) return 1;
    for (size_t i = 0; i < a.len; i++) if (a.p[i] != b.p[i]) return 0;
    return 1;
}
static inline int sstarts(Str s, const char *pfx) {
    size_t n = 0; while (pfx[n]) n++;
    if (s.len < n) return 0;
    for (size_t i = 0; i < n; i++) if (s.p[i] != (uint8_t)pfx[i]) return 0;
    return 1;
}
static inline int sends(Str s, const char *sfx) {
    size_t n = 0; while (sfx[n]) n++;
    if (s.len < n) return 0;
    for (size_t i = 0; i < n; i++) if (s.p[s.len - n + i] != (uint8_t)sfx[i]) return 0;
    return 1;
}
static inline size_t sfindbyte(Str s, size_t from, uint8_t c) {
    for (size_t i = from; i < s.len; i++) if (s.p[i] == c) return i;
    return (size_t)-1;
}
static inline size_t sfind(Str s, size_t from, const uint8_t *nd, size_t nlen) {
    if (nlen == 0) return from <= s.len ? from : (size_t)-1;
    if (nlen > s.len) return (size_t)-1;
    for (size_t i = from; i + nlen <= s.len; i++) {
        size_t k = 0;
        while (k < nlen && s.p[i + k] == nd[k]) k++;
        if (k == nlen) return i;
    }
    return (size_t)-1;
}

/* A plain append buffer (arena / html / out are all just Bufs). */
typedef struct { uint8_t *buf; size_t len; size_t cap; } Buf;
static inline void *b_alloc(Buf *b, size_t n) {
    size_t a = (b->len + 7) & ~(size_t)7;
    if (a + n > b->cap) __builtin_trap();
    b->len = a + n;
    return b->buf + a;
}
static inline void b_put(Buf *b, const uint8_t *p, size_t n) {
    if (b->len + n > b->cap) __builtin_trap();
    for (size_t i = 0; i < n; i++) b->buf[b->len + i] = p[i];
    b->len += n;
}
static inline void b_byte(Buf *b, uint8_t c) {
    if (b->len + 1 > b->cap) __builtin_trap();
    b->buf[b->len++] = c;
}
static inline void b_str(Buf *b, Str s) { b_put(b, s.p, s.len); }
static inline void b_cstr(Buf *b, const char *c) {
    size_t n = 0; while (c[n]) n++;
    b_put(b, (const uint8_t *)c, n);
}
static inline void b_i64(Buf *b, int64_t v) {
    uint8_t tmp[20];
    int neg = v < 0;
    uint64_t m = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int i = 20;
    if (m == 0) tmp[--i] = '0';
    else while (m > 0) { tmp[--i] = (uint8_t)('0' + (m % 10)); m /= 10; }
    if (neg) tmp[--i] = '-';
    b_put(b, tmp + i, (size_t)(20 - i));
}
static inline Str b_span(Buf *b, size_t start) { return S(b->buf + start, b->len - start); }

/* Growable array in a Buf (ptr invalidated on growth; hold briefly). */
typedef struct { void *ptr; size_t len; size_t cap; } Vec;
static inline void *v_grow(Buf *b, Vec *v, size_t elem) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    void *np = b_alloc(b, nc * elem);
    if (v->ptr && v->len) {
        uint8_t *d = (uint8_t *)np, *s = (uint8_t *)v->ptr;
        for (size_t i = 0; i < v->len * elem; i++) d[i] = s[i];
    }
    v->ptr = np; v->cap = nc;
    return np;
}
static inline void *v_slot(Buf *b, Vec *v, size_t elem) {
    if (v->len == v->cap) v_grow(b, v, elem);
    void *slot = (uint8_t *)v->ptr + v->len * elem;
    v->len++;
    return slot;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                              */
/* ------------------------------------------------------------------ */
static inline uint32_t u8dec(const uint8_t *s, size_t len, size_t i, size_t *adv) {
    uint8_t c = s[i];
    if (c < 0x80) { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < len) {
        *adv = 2;
        return ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[i + 1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < len) {
        *adv = 3;
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) |
               (uint32_t)(s[i + 2] & 0x3F);
    }
    *adv = 4;
    return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) |
           ((uint32_t)(s[i + 2] & 0x3F) << 6) | (uint32_t)(s[i + 3] & 0x3F);
}
static inline size_t u8enc(uint32_t cp, uint8_t out[4]) {
    if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}
static inline size_t u16len(Str s) {
    size_t n = 0, i = 0;
    while (i < s.len) {
        size_t adv;
        uint32_t cp = u8dec(s.p, s.len, i, &adv);
        n += (cp >= 0x10000) ? 2 : 1;
        i += adv;
    }
    return n;
}
static int cmp_u16(Str a, Str b) {
    size_t ia = 0, ib = 0;
    uint16_t pa = 0, pb = 0;
    int ha = 0, hb = 0;
    for (;;) {
        uint16_t ua, ub;
        int ea, eb;
        if (ha) { ua = pa; ha = 0; ea = 1; }
        else if (ia < a.len) {
            size_t adv;
            uint32_t cp = u8dec(a.p, a.len, ia, &adv);
            ia += adv;
            if (cp < 0x10000) { ua = (uint16_t)cp; ea = 1; }
            else { uint32_t v = cp - 0x10000; pa = (uint16_t)(0xDC00 + (v & 0x3FF)); ha = 1; ua = (uint16_t)(0xD800 + (v >> 10)); ea = 1; }
        } else ea = 0;
        if (hb) { ub = pb; hb = 0; eb = 1; }
        else if (ib < b.len) {
            size_t adv;
            uint32_t cp = u8dec(b.p, b.len, ib, &adv);
            ib += adv;
            if (cp < 0x10000) { ub = (uint16_t)cp; eb = 1; }
            else { uint32_t v = cp - 0x10000; pb = (uint16_t)(0xDC00 + (v & 0x3FF)); hb = 1; ub = (uint16_t)(0xD800 + (v >> 10)); eb = 1; }
        } else eb = 0;
        if (!ea && !eb) return 0;
        if (!ea) return -1;
        if (!eb) return 1;
        if (ua != ub) return ua < ub ? -1 : 1;
    }
}
static int is_js_space(uint32_t cp) {
    return cp == '\t' || cp == '\n' || cp == 0x0B || cp == 0x0C || cp == '\r' ||
           cp == ' ' || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

/* ------------------------------------------------------------------ */
/* SIMD / SWAR scans                                                  */
/* ------------------------------------------------------------------ */
static inline int has_byte64(uint64_t x, uint8_t b) {
    uint64_t m = x ^ (uint64_t)b * 0x0101010101010101ULL;
    return ((m - 0x0101010101010101ULL) & ~m & 0x8080808080808080ULL) != 0;
}
static inline uint64_t load64(const uint8_t *p) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}
#ifdef __wasm__
#include <wasm_simd128.h>
static inline size_t find_a_or_b(const uint8_t *buf, size_t len, size_t from, uint8_t a, uint8_t b) {
    size_t i = from;
    v128_t va = wasm_u8x16_splat(a);
    v128_t vb = wasm_u8x16_splat(b);
    while (i + 16 <= len) {
        v128_t v = wasm_v128_load(buf + i);
        v128_t m = wasm_v128_or(wasm_i8x16_eq(v, va), wasm_i8x16_eq(v, vb));
        uint32_t mask = wasm_i8x16_bitmask(m);
        if (mask) return i + __builtin_ctz(mask);
        i += 16;
    }
    while (i < len) {
        if (buf[i] == a || buf[i] == b) return i;
        i++;
    }
    return (size_t)-1;
}
static inline size_t find_in_set(const uint8_t *buf, size_t len, size_t from, const uint8_t *set, size_t n) {
    size_t i = from;
    while (i + 16 <= len) {
        v128_t v = wasm_v128_load(buf + i);
        v128_t acc = wasm_i8x16_eq(v, wasm_u8x16_splat(set[0]));
        for (size_t k = 1; k < n; k++)
            acc = wasm_v128_or(acc, wasm_i8x16_eq(v, wasm_u8x16_splat(set[k])));
        uint32_t mask = wasm_i8x16_bitmask(acc);
        if (mask) return i + __builtin_ctz(mask);
        i += 16;
    }
    while (i < len) {
        for (size_t k = 0; k < n; k++) if (buf[i] == set[k]) return i;
        i++;
    }
    return (size_t)-1;
}
#elif defined(__SSE2__)
/* Native x86-64 (N-API .node): SSE2 translation of the WASM SIMD scans.
 * 1:1 mapping: wasm_v128_load -> _mm_loadu_si128, wasm_u8x16_splat ->
 * _mm_set1_epi8, wasm_i8x16_eq -> _mm_cmpeq_epi8, wasm_v128_or ->
 * _mm_or_si128, wasm_i8x16_bitmask -> _mm_movemask_epi8. SSE2 is baseline
 * on x86-64, so this is always available when compiling for x86-64. */
#include <emmintrin.h>
static inline size_t find_a_or_b(const uint8_t *buf, size_t len, size_t from, uint8_t a, uint8_t b) {
    size_t i = from;
    __m128i va = _mm_set1_epi8((int)a);
    __m128i vb = _mm_set1_epi8((int)b);
    while (i + 16 <= len) {
        __m128i v = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, va), _mm_cmpeq_epi8(v, vb));
        uint32_t mask = (uint32_t)_mm_movemask_epi8(m);
        if (mask) return i + __builtin_ctz(mask);
        i += 16;
    }
    while (i < len) {
        if (buf[i] == a || buf[i] == b) return i;
        i++;
    }
    return (size_t)-1;
}
static inline size_t find_in_set(const uint8_t *buf, size_t len, size_t from, const uint8_t *set, size_t n) {
    size_t i = from;
    while (i + 16 <= len) {
        __m128i v = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i acc = _mm_cmpeq_epi8(v, _mm_set1_epi8((int)set[0]));
        for (size_t k = 1; k < n; k++)
            acc = _mm_or_si128(acc, _mm_cmpeq_epi8(v, _mm_set1_epi8((int)set[k])));
        uint32_t mask = (uint32_t)_mm_movemask_epi8(acc);
        if (mask) return i + __builtin_ctz(mask);
        i += 16;
    }
    while (i < len) {
        for (size_t k = 0; k < n; k++) if (buf[i] == set[k]) return i;
        i++;
    }
    return (size_t)-1;
}
#else
static inline size_t find_a_or_b(const uint8_t *buf, size_t len, size_t from, uint8_t a, uint8_t b) {
    for (size_t i = from; i < len; i++) if (buf[i] == a || buf[i] == b) return i;
    return (size_t)-1;
}
static inline size_t find_in_set(const uint8_t *buf, size_t len, size_t from, const uint8_t *set, size_t n) {
    for (size_t i = from; i < len; i++)
        for (size_t k = 0; k < n; k++) if (buf[i] == set[k]) return i;
    return (size_t)-1;
}
#endif

/* ------------------------------------------------------------------ */
/* JSON parsing (targeted; borrows the input buffer, decodes in place) */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t *buf; size_t len; size_t i; } P;
static inline void p_ws(P *p) {
    while (p->i < p->len) {
        uint8_t c = p->buf[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}
static uint32_t p_hex4(P *p) {
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        uint8_t c = p->buf[p->i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else d = 0;
        v = v * 16 + d;
        p->i++;
    }
    return v;
}
static size_t p_put_char(P *p, size_t w, uint32_t cp) {
    uint8_t tmp[4];
    size_t n = u8enc(cp, tmp);
    for (size_t k = 0; k < n; k++) p->buf[w + k] = tmp[k];
    return w + n;
}
static Str p_string(P *p) {
    p->i++; /* opening quote */
    size_t start = p->i;
    size_t off = find_a_or_b(p->buf, p->len, p->i, '"', '\\');
    if (off != (size_t)-1) {
        if (p->buf[off] == '"') {
            Str s = S(p->buf + start, off - start);
            p->i = off + 1;
            return s;
        }
        p->i = off;
    } else {
        return S(p->buf + start, p->len - start);
    }
    size_t w = p->i;
    for (;;) {
        if (p->i >= p->len) return S(p->buf + start, w - start);
        uint8_t c = p->buf[p->i];
        if (c == '"') {
            Str s = S(p->buf + start, w - start);
            p->i++;
            return s;
        } else if (c == '\\') {
            p->i++;
            if (p->i >= p->len) break;
            uint8_t e = p->buf[p->i];
            switch (e) {
            case '"': p->buf[w++] = '"'; p->i++; break;
            case '\\': p->buf[w++] = '\\'; p->i++; break;
            case '/': p->buf[w++] = '/'; p->i++; break;
            case 'b': p->buf[w++] = 0x08; p->i++; break;
            case 'f': p->buf[w++] = 0x0C; p->i++; break;
            case 'n': p->buf[w++] = '\n'; p->i++; break;
            case 'r': p->buf[w++] = '\r'; p->i++; break;
            case 't': p->buf[w++] = '\t'; p->i++; break;
            case 'u': {
                p->i++;
                uint32_t cp = p_hex4(p);
                if (cp >= 0xD800 && cp < 0xDC00) {
                    if (p->i + 1 < p->len && p->buf[p->i] == '\\' && p->buf[p->i + 1] == 'u') {
                        p->i += 2;
                        uint32_t lo = p_hex4(p);
                        if (lo >= 0xDC00 && lo < 0xE000)
                            w = p_put_char(p, w, 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
                        else w = p_put_char(p, w, 0xFFFD);
                    } else {
                        w = p_put_char(p, w, 0xFFFD);
                    }
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    w = p_put_char(p, w, 0xFFFD);
                } else {
                    w = p_put_char(p, w, cp);
                }
                break;
            }
            default:
                p->buf[w++] = e;
                p->i++;
                break;
            }
        } else {
            if (w != p->i) p->buf[w] = c;
            w++;
            p->i++;
        }
    }
    return S(p->buf + start, w - start);
}
static int64_t p_i64(P *p) {
    p_ws(p);
    int neg = 0;
    if (p->i < p->len && p->buf[p->i] == '-') { neg = 1; p->i++; }
    int64_t v = 0;
    while (p->i < p->len && p->buf[p->i] >= '0' && p->buf[p->i] <= '9') {
        v = v * 10 + (p->buf[p->i] - '0');
        p->i++;
    }
    return neg ? -v : v;
}
static void p_skip(P *p) {
    p_ws(p);
    if (p->i >= p->len) return;
    uint8_t c = p->buf[p->i];
    if (c == '"') {
        (void)p_string(p);
    } else if (c == '{') {
        p->i++;
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == '}') { p->i++; return; }
        for (;;) {
            (void)p_string(p);
            p_ws(p);
            p->i++;
            p_skip(p);
            p_ws(p);
            if (p->i < p->len && p->buf[p->i] == ',') { p->i++; continue; }
            p->i++;
            return;
        }
    } else if (c == '[') {
        p->i++;
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == ']') { p->i++; return; }
        for (;;) {
            p_skip(p);
            p_ws(p);
            if (p->i < p->len && p->buf[p->i] == ',') { p->i++; continue; }
            p->i++;
            return;
        }
    } else if (c == 't') {
        p->i += 4;
    } else if (c == 'f') {
        p->i += 5;
    } else if (c == 'n') {
        p->i += 4;
    } else {
        while (p->i < p->len) {
            uint8_t d = p->buf[p->i];
            if (d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E' ||
                (d >= '0' && d <= '9')) p->i++;
            else break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* JSON escaping (into a Buf) — exactly the chars JSON.stringify escapes */
/* ------------------------------------------------------------------ */
static void esc_json(Buf *o, const uint8_t *b, size_t len) {
    static const char HEX[] = "0123456789abcdef";
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i + 8 <= len) {
            uint64_t w = load64(b + i);
            int has_special = has_byte64(w, '"') || has_byte64(w, '\\');
            uint64_t low = w & 0xE0E0E0E0E0E0E0E0ULL;
            int has_low = ((low - 0x0101010101010101ULL) & ~low & 0x8080808080808080ULL) != 0;
            if (has_special || has_low) break;
            i += 8;
        }
        while (i < len) {
            uint8_t c = b[i];
            if (c == '"' || c == '\\' || c < 0x20) break;
            i++;
        }
        b_put(o, b + start, i - start);
        if (i >= len) break;
        uint8_t c = b[i];
        switch (c) {
        case '"': b_put(o, (const uint8_t *)"\\\"", 2); break;
        case '\\': b_put(o, (const uint8_t *)"\\\\", 2); break;
        case '\n': b_put(o, (const uint8_t *)"\\n", 2); break;
        case '\r': b_put(o, (const uint8_t *)"\\r", 2); break;
        case '\t': b_put(o, (const uint8_t *)"\\t", 2); break;
        case 0x08: b_put(o, (const uint8_t *)"\\b", 2); break;
        case 0x0C: b_put(o, (const uint8_t *)"\\f", 2); break;
        default: {
            uint8_t esc[6] = {'\\', 'u', '0', '0', 0, 0};
            esc[4] = (uint8_t)HEX[(c >> 4) & 0xF];
            esc[5] = (uint8_t)HEX[c & 0xF];
            b_put(o, esc, 6);
            break;
        }
        }
        i++;
    }
}
static inline void o_json_str(Buf *o, Str s) {
    b_byte(o, '"');
    esc_json(o, s.p, s.len);
    b_byte(o, '"');
}

/* ------------------------------------------------------------------ */
/* Text: HTML escape, slugify, long_date, words_of                    */
/* ------------------------------------------------------------------ */
static void esc_html(Buf *o, Str s) {
    static const uint8_t SP[5] = {'&', '<', '>', '"', '\''};
    size_t i = 0;
    while (i < s.len) {
        size_t nx = find_in_set(s.p, s.len, i, SP, 5);
        if (nx == (size_t)-1) nx = s.len;
        if (nx > i) b_put(o, s.p + i, nx - i);
        if (nx >= s.len) break;
        switch (s.p[nx]) {
        case '&': b_cstr(o, "&amp;"); break;
        case '<': b_cstr(o, "&lt;"); break;
        case '>': b_cstr(o, "&gt;"); break;
        case '"': b_cstr(o, "&quot;"); break;
        default: b_cstr(o, "&#39;"); break;
        }
        i = nx + 1;
    }
}
/* HTML-escape then JSON-escape in ONE pass, straight into the output.
 * A byte is clean iff it is neither an HTML special nor a JSON special;
 * '"' takes the HTML entity (like HTML-then-JSON would). */
static void esc_html_json(Buf *o, Str s) {
    static const char HX[] = "0123456789abcdef";
    const uint8_t *b = s.p;
    size_t len = s.len, i = 0;
    while (i < len) {
        size_t start = i;
        while (i + 8 <= len) {
            uint64_t w = load64(b + i);
            int sp = has_byte64(w, '&') || has_byte64(w, '<') || has_byte64(w, '>') ||
                     has_byte64(w, '"') || has_byte64(w, '\'') || has_byte64(w, '\\');
            uint64_t low = w & 0xE0E0E0E0E0E0E0E0ULL;
            int hl = ((low - 0x0101010101010101ULL) & ~low & 0x8080808080808080ULL) != 0;
            if (sp || hl) break;
            i += 8;
        }
        while (i < len) {
            uint8_t c = b[i];
            if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'' || c == '\\' || c < 0x20) break;
            i++;
        }
        b_put(o, b + start, i - start);
        if (i >= len) break;
        uint8_t c = b[i];
        switch (c) {
        case '&': b_cstr(o, "&amp;"); break;
        case '<': b_cstr(o, "&lt;"); break;
        case '>': b_cstr(o, "&gt;"); break;
        case '"': b_cstr(o, "&quot;"); break;
        case '\'': b_cstr(o, "&#39;"); break;
        case '\\': b_cstr(o, "\\\\"); break;
        case '\n': b_cstr(o, "\\n"); break;
        case '\r': b_cstr(o, "\\r"); break;
        case '\t': b_cstr(o, "\\t"); break;
        case 0x08: b_cstr(o, "\\b"); break;
        case 0x0C: b_cstr(o, "\\f"); break;
        default: {
            uint8_t esc[6] = {'\\', 'u', '0', '0', 0, 0};
            esc[4] = (uint8_t)HX[(c >> 4) & 0xF];
            esc[5] = (uint8_t)HX[c & 0xF];
            b_put(o, esc, 6);
            break;
        }
        }
        i++;
    }
}

static const NfkdEnt *nfkd_lookup(uint32_t cp) {
    size_t lo = 0, hi = NFKD_N;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (NFKD[mid].cp == cp) return &NFKD[mid];
        if (NFKD[mid].cp < cp) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static int is_mark_cp(uint32_t cp) {
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE20 && cp <= 0xFE2F);
}
static inline void slug_char(uint8_t *buf, size_t *len, int *gap, uint32_t ch) {
    int keep = -1;
    if (ch >= 'A' && ch <= 'Z') keep = (int)(ch - 'A' + 'a');
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = (int)ch;
    if (keep >= 0) {
        if (*gap) { if (*len < 64) buf[(*len)++] = '-'; *gap = 0; }
        if (*len < 64) buf[(*len)++] = (uint8_t)keep;
    } else {
        if (*len > 0) *gap = 1;
    }
}
static Str slugify(Buf *arena, Str text) {
    uint8_t buf[64];
    size_t len = 0;
    int gap = 0;
    size_t i = 0;
    while (i < text.len) {
        if (len >= 64) break;
        size_t adv;
        uint32_t ch = u8dec(text.p, text.len, i, &adv);
        i += adv;
        if (ch < 0x80) {
            slug_char(buf, &len, &gap, ch);
        } else if (ch >= 0xFF01 && ch <= 0xFF5E) {
            slug_char(buf, &len, &gap, ch - 0xFEE0);
        } else if (is_mark_cp(ch)) {
            continue;
        } else {
            const NfkdEnt *d = nfkd_lookup(ch);
            if (d) {
                if (d->n == 0) { if (len > 0) gap = 1; }
                else for (int k = 0; k < d->n; k++) {
                    slug_char(buf, &len, &gap, (uint8_t)d->out[k]);
                    if (len >= 64) break;
                }
            } else {
                if (len > 0) gap = 1;
            }
        }
    }
    while (len > 0 && buf[len - 1] == '-') len--;
    if (len == 0) {
        uint8_t *dst = (uint8_t *)b_alloc(arena, 8);
        const char *u = "untitled";
        for (int k = 0; k < 8; k++) dst[k] = (uint8_t)u[k];
        return S(dst, 8);
    }
    uint8_t *dst = (uint8_t *)b_alloc(arena, len);
    for (size_t k = 0; k < len; k++) dst[k] = buf[k];
    return S(dst, len);
}
/* `slug` made unique against `taken` (Vec<Str>, linear) and added to it. */
static Str unique_slug(Buf *arena, Str slug, Vec *taken) {
    for (size_t k = 0; k < taken->len; k++)
        if (seq(((Str *)taken->ptr)[k], slug)) goto taken_found;
    {
        Str *slot = (Str *)v_slot(arena, taken, sizeof(Str));
        *slot = slug;
        return slug;
    }
taken_found:;
    int n = 2;
    for (;;) {
        size_t st = arena->len;
        b_str(arena, slug);
        b_byte(arena, '-');
        b_i64(arena, n);
        Str cand = b_span(arena, st);
        int found = 0;
        for (size_t k = 0; k < taken->len; k++)
            if (seq(((Str *)taken->ptr)[k], cand)) { found = 1; break; }
        if (!found) {
            Str *slot = (Str *)v_slot(arena, taken, sizeof(Str));
            *slot = cand;
            return cand;
        }
        n++;
    }
}
static const char *const MONTHS[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};
static Str long_date(Buf *arena, Str date) {
    size_t d1 = sfindbyte(date, 0, '-');
    size_t d2 = d1 == (size_t)-1 ? (size_t)-1 : sfindbyte(date, d1 + 1, '-');
    Str y = d1 == (size_t)-1 ? date : S(date.p, d1);
    Str ms = d1 == (size_t)-1 ? S(date.p, 0)
             : (d2 == (size_t)-1 ? S(date.p + d1 + 1, date.len - d1 - 1)
                                 : S(date.p + d1 + 1, d2 - d1 - 1));
    Str ds = d2 == (size_t)-1 ? S(date.p, 0)
             : S(date.p + d2 + 1, date.len - d2 - 1);
    long m = 0, d = 0;
    for (size_t k = 0; k < ms.len; k++) {
        if (ms.p[k] < '0' || ms.p[k] > '9') { m = 0; break; }
        m = m * 10 + (ms.p[k] - '0');
    }
    for (size_t k = 0; k < ds.len; k++) {
        if (ds.p[k] < '0' || ds.p[k] > '9') { d = 0; break; }
        d = d * 10 + (ds.p[k] - '0');
    }
    const char *month = (m >= 1 && m <= 12) ? MONTHS[m - 1] : "";
    size_t st = arena->len;
    b_i64(arena, d);
    b_byte(arena, ' ');
    b_cstr(arena, month);
    b_byte(arena, ' ');
    b_str(arena, y);
    return b_span(arena, st);
}
static size_t words_of(Str text) {
    size_t n = 0;
    int in_word = 0;
    size_t i = 0;
    while (i < text.len) {
        size_t adv;
        uint32_t ch = u8dec(text.p, text.len, i, &adv);
        i += adv;
        if (is_js_space(ch)) in_word = 0;
        else if (!in_word) { in_word = 1; n++; }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Front matter                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    Str title, date, layout, slug, summary;
    Str *tags; size_t ntags;
    int draft;
    uint8_t has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
} FmData;
typedef struct { int ok; FmData data; Str body; Str err; } FmResult;

static int64_t days_in(int64_t y, int64_t m) {
    if (m == 2) {
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) return 29;
        return 28;
    }
    if (m == 4 || m == 6 || m == 9 || m == 11) return 30;
    return 31;
}
/* trim a Str of JS-space on both ends (used by fm + template trim). */
static Str trim_js(Str s) {
    size_t a = 0, b = s.len;
    while (a < b) {
        size_t adv; uint32_t cp = u8dec(s.p, s.len, a, &adv);
        if (!is_js_space(cp)) break;
        a += adv;
    }
    while (b > a) {
        size_t k = b - 1;
        while (k > a && (s.p[k] & 0xC0) == 0x80) k--;
        size_t adv; uint32_t cp = u8dec(s.p, s.len, k, &adv);
        if (!is_js_space(cp)) break;
        b = k;
    }
    return S(s.p + a, b - a);
}
/* A string value: "..." with \" and \\ its only escapes, or bare. */
static int fm_string_of(Str raw, Buf *arena, Str *out, Str *err) {
    if (raw.len > 0 && raw.p[0] == '"') {
        if (raw.len < 2 || raw.p[raw.len - 1] != '"') {
            *err = SC("a quoted string is not closed");
            return 0;
        }
        Str inner = S(raw.p + 1, raw.len - 2);
        int has_bs = 0, has_quote = 0;
        for (size_t k = 0; k < inner.len; k++) {
            if (inner.p[k] == '\\') has_bs = 1;
            if (inner.p[k] == '"') has_quote = 1;
        }
        if (!has_bs) {
            if (has_quote) {
                *err = SC("a quoted string holds a stray quote");
                return 0;
            }
            *out = inner;
            return 1;
        }
        size_t st = arena->len;
        size_t i = 1;
        while (i < raw.len - 1) {
            uint8_t c = raw.p[i];
            if (c == '\\') {
                uint8_t n = raw.p[i + 1];
                if ((n != '"' && n != '\\') || i + 1 == raw.len - 1) {
                    *err = SC("a quoted string holds an escape other than \\\" or \\\\");
                    return 0;
                }
                b_byte(arena, n);
                i += 2;
            } else if (c == '"') {
                *err = SC("a quoted string holds a stray quote");
                return 0;
            } else {
                b_byte(arena, c);
                i++;
            }
        }
        *out = b_span(arena, st);
        return 1;
    } else if (raw.len > 0 && raw.p[0] == '[') {
        *err = SC("a bare string opens on [");
        return 0;
    } else {
        *out = raw;
        return 1;
    }
}
static int fm_value_of(Str key, Str raw, FmData *d, Buf *arena, Str *err) {
    if (seq(key, SC("title")) || seq(key, SC("summary"))) {
        Str v;
        if (!fm_string_of(raw, arena, &v, err)) {
            size_t st = arena->len;
            b_cstr(arena, "key \"");
            b_str(arena, key);
            b_cstr(arena, "\": ");
            b_str(arena, *err);
            *err = b_span(arena, st);
            return 0;
        }
        if (seq(key, SC("title"))) { d->title = v; d->has_title = 1; }
        else { d->summary = v; d->has_summary = 1; }
        return 1;
    }
    if (seq(key, SC("date"))) {
        int ok_shape = raw.len == 10 && raw.p[4] == '-' && raw.p[7] == '-' &&
            raw.p[0] >= '0' && raw.p[0] <= '9' && raw.p[1] >= '0' && raw.p[1] <= '9' &&
            raw.p[2] >= '0' && raw.p[2] <= '9' && raw.p[3] >= '0' && raw.p[3] <= '9' &&
            raw.p[5] >= '0' && raw.p[5] <= '9' && raw.p[6] >= '0' && raw.p[6] <= '9' &&
            raw.p[8] >= '0' && raw.p[8] <= '9' && raw.p[9] >= '0' && raw.p[9] <= '9';
        int64_t y = 0, mo = 0, dd = 0;
        if (ok_shape) {
            y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
            mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
            dd = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
        }
        if (!ok_shape || y < 1970 || y > 2099 || mo < 1 || mo > 12 || dd < 1 || dd > days_in(y, mo)) {
            *err = SC("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
            return 0;
        }
        d->date = raw; d->has_date = 1;
        return 1;
    }
    if (seq(key, SC("tags"))) {
        if (raw.len < 2 || raw.p[0] != '[' || raw.p[raw.len - 1] != ']') {
            *err = SC("key \"tags\": not a list [a, b]");
            return 0;
        }
        Str inner = S(raw.p + 1, raw.len - 2);
        Str tr = trim_js(inner);
        if (tr.len == 0) { d->tags = NULL; d->ntags = 0; d->has_tags = 1; return 1; }
        Vec tags = {NULL, 0, 0};
        size_t start = 0;
        for (size_t i = 0; i <= tr.len; i++) {
            if (i == tr.len || tr.p[i] == ',') {
                Str t = trim_js(S(tr.p + start, i - start));
                if (t.len == 0 || sfindbyte(t, 0, '"') != (size_t)-1 ||
                    sfindbyte(t, 0, '[') != (size_t)-1 ||
                    sfindbyte(t, 0, ']') != (size_t)-1) {
                    *err = SC("key \"tags\": an empty tag, or one holding \" [ or ]");
                    return 0;
                }
                Str *slot = (Str *)v_slot(arena, &tags, sizeof(Str));
                *slot = t;
                start = i + 1;
            }
        }
        d->tags = (Str *)tags.ptr;
        d->ntags = tags.len;
        d->has_tags = 1;
        return 1;
    }
    if (seq(key, SC("draft"))) {
        if (seq(raw, SC("true"))) { d->draft = 1; d->has_draft = 1; return 1; }
        if (seq(raw, SC("false"))) { d->draft = 0; d->has_draft = 1; return 1; }
        *err = SC("key \"draft\": true or false");
        return 0;
    }
    if (seq(key, SC("layout"))) {
        int ok = raw.len > 0;
        for (size_t k = 0; ok && k < raw.len; k++) {
            uint8_t c = raw.p[k];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
        }
        if (ok) { d->layout = raw; d->has_layout = 1; return 1; }
        *err = SC("key \"layout\": not a template name");
        return 0;
    }
    if (seq(key, SC("slug"))) {
        int ok = raw.len <= 64 && raw.len > 0 &&
            raw.p[0] != '-' && raw.p[raw.len - 1] != '-' &&
            sfind(raw, 0, (const uint8_t *)"--", 2) == (size_t)-1;
        for (size_t k = 0; ok && k < raw.len; k++) {
            uint8_t c = raw.p[k];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
        }
        if (ok) { d->slug = raw; d->has_slug = 1; return 1; }
        *err = SC("key \"slug\": not a slug");
        return 0;
    }
    {
        size_t st = arena->len;
        b_cstr(arena, "unknown key \"");
        b_str(arena, key);
        b_byte(arena, '"');
        *err = b_span(arena, st);
        return 0;
    }
}
static const char *const FM_KEYS[7] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};
static FmResult parse_fm(Str text, Buf *arena) {
    FmResult r;
    r.ok = 0; r.body = S(NULL, 0); r.err = S(NULL, 0);
    FmData *d = &r.data;
    d->title = S(NULL, 0); d->date = S(NULL, 0); d->layout = S(NULL, 0);
    d->slug = S(NULL, 0); d->summary = S(NULL, 0);
    d->tags = NULL; d->ntags = 0; d->draft = 0;
    d->has_title = 0; d->has_date = 0; d->has_tags = 0; d->has_draft = 0;
    d->has_layout = 0; d->has_slug = 0; d->has_summary = 0;
    /* split lines (borrowed Strs in the arena) */
    Vec lines = {NULL, 0, 0};
    size_t start = 0;
    for (size_t i = 0; i <= text.len; i++) {
        if (i == text.len || text.p[i] == '\n') {
            Str *slot = (Str *)v_slot(arena, &lines, sizeof(Str));
            *slot = S(text.p + start, i - start);
            start = i + 1;
        }
    }
    Str *ls = (Str *)lines.ptr;
    if (lines.len == 0 || !seq(ls[0], SC("---"))) {
        r.err = SC("no front matter");
        return r;
    }
    size_t close = (size_t)-1;
    for (size_t i = 1; i < lines.len; i++)
        if (seq(ls[i], SC("---"))) { close = i; break; }
    if (close == (size_t)-1) {
        r.err = SC("the front matter is not closed");
        return r;
    }
    uint8_t seen = 0;
    for (size_t i = 1; i < close; i++) {
        Str line = ls[i];
        int blank = 1;
        for (size_t k = 0; k < line.len; k++)
            if (line.p[k] != ' ' && line.p[k] != '\t') { blank = 0; break; }
        if (blank || (line.len > 0 && line.p[0] == '#')) continue;
        size_t j = 0;
        while (j < line.len && ((line.p[j] >= 'a' && line.p[j] <= 'z') || line.p[j] == '_')) j++;
        if (j == 0 || j >= line.len || line.p[j] != ':') {
            size_t st = arena->len;
            b_cstr(arena, "line ");
            b_i64(arena, (int64_t)(i + 1));
            b_cstr(arena, " is not \"key: value\"");
            r.err = b_span(arena, st);
            return r;
        }
        Str key = S(line.p, j);
        Str raw_full;
        if (j + 1 >= line.len) raw_full = S(line.p + line.len, 0);
        else if (line.p[j + 1] == ' ') raw_full = S(line.p + j + 2, line.len - j - 2);
        else {
            size_t st = arena->len;
            b_cstr(arena, "line ");
            b_i64(arena, (int64_t)(i + 1));
            b_cstr(arena, " is not \"key: value\"");
            r.err = b_span(arena, st);
            return r;
        }
        size_t rl = raw_full.len;
        while (rl > 0 && (raw_full.p[rl - 1] == ' ' || raw_full.p[rl - 1] == '\t')) rl--;
        Str raw = S(raw_full.p, rl);
        int ki = -1;
        for (int k = 0; k < 7; k++)
            if (seq(key, SC(FM_KEYS[k]))) { ki = k; break; }
        if (ki < 0) {
            size_t st = arena->len;
            b_cstr(arena, "unknown key \"");
            b_str(arena, key);
            b_byte(arena, '"');
            r.err = b_span(arena, st);
            return r;
        }
        if (seen & (1u << ki)) {
            size_t st = arena->len;
            b_cstr(arena, "key \"");
            b_str(arena, key);
            b_cstr(arena, "\" is given twice");
            r.err = b_span(arena, st);
            return r;
        }
        seen |= (uint8_t)(1u << ki);
        if (raw.len == 0) {
            size_t st = arena->len;
            b_cstr(arena, "key \"");
            b_str(arena, key);
            b_cstr(arena, "\" has no value");
            r.err = b_span(arena, st);
            return r;
        }
        Str verr;
        if (!fm_value_of(key, raw, d, arena, &verr)) {
            r.err = verr;
            return r;
        }
    }
    if (!d->has_title) {
        r.err = SC("no title");
        return r;
    }
    size_t body_start = 0;
    for (size_t l = 0; l <= close; l++) body_start += ls[l].len + 1;
    r.body = body_start > text.len ? S(text.p + text.len, 0)
                                   : S(text.p + body_start, text.len - body_start);
    r.ok = 1;
    return r;
}

/* ------------------------------------------------------------------ */
/* Markdown (fused lex+render: blocks stream straight to the HTML buf) */
/* ------------------------------------------------------------------ */
typedef struct { uint8_t level; Str id; Str html; } TocEntry;
typedef struct { Str path; Str url; } BySource;
typedef struct { Str spath; Str target; } BrokenPair;
typedef struct {
    BySource *bs; size_t nbs;
    Str *static_paths; size_t nstatic;
    Str base;
    Str *dir_parts; size_t ndir;
    Str spath;
    Vec *broken;
    Buf *arena;
} LinkCtx;

/* Globals: the three output buffers (set up by run_all). */
static Buf g_arena, g_perm, g_html, g_out;

static int is_blank(Str line) {
    for (size_t k = 0; k < line.len; k++)
        if (line.p[k] != ' ' && line.p[k] != '\t') return 0;
    return 1;
}
static int fence_open(Str line, Str *lang) {
    if (!sstarts(line, "```")) return 0;
    Str rest = S(line.p + 3, line.len - 3);
    size_t j = 0;
    while (j < rest.len) {
        uint8_t c = rest.p[j];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '+' || c == '-') j++;
        else break;
    }
    for (size_t k = j; k < rest.len; k++)
        if (rest.p[k] != ' ' && rest.p[k] != '\t') return 0;
    *lang = S(rest.p, j);
    return 1;
}
static int is_fence_close(Str line) {
    if (!sstarts(line, "```")) return 0;
    for (size_t k = 3; k < line.len; k++)
        if (line.p[k] != ' ' && line.p[k] != '\t') return 0;
    return 1;
}
static int heading_of(Str line, uint8_t *depth, Str *text) {
    size_t n = 0;
    while (n < line.len && line.p[n] == '#' && n < 6) n++;
    if (n == 0 || n >= line.len || (line.p[n] != ' ' && line.p[n] != '\t')) return 0;
    size_t j = n;
    while (j < line.len && (line.p[j] == ' ' || line.p[j] == '\t')) j++;
    size_t e = line.len;
    while (e > j && (line.p[e - 1] == ' ' || line.p[e - 1] == '\t')) e--;
    if (e <= j) return 0;
    *depth = (uint8_t)n;
    *text = S(line.p + j, e - j);
    return 1;
}
static int is_hr(Str line) {
    Str rest;
    if (sstarts(line, "---")) rest = S(line.p + 3, line.len - 3);
    else if (sstarts(line, "***")) rest = S(line.p + 3, line.len - 3);
    else return 0;
    for (size_t k = 0; k < rest.len; k++)
        if (rest.p[k] != ' ' && rest.p[k] != '\t') return 0;
    return 1;
}
static Str trim_line(Str line) {
    size_t a = 0, b = line.len;
    while (a < b && (line.p[a] == ' ' || line.p[a] == '\t')) a++;
    while (b > a && (line.p[b - 1] == ' ' || line.p[b - 1] == '\t')) b--;
    return S(line.p + a, b - a);
}
static int ends_para(Str line) {
    uint8_t d; Str t; Str lang;
    return is_blank(line) || fence_open(line, &lang) || heading_of(line, &d, &t) || is_hr(line);
}
static int bullet_of(Str line, Str *rest) {
    if (line.len >= 2 && (line.p[0] == '-' || line.p[0] == '*') && line.p[1] == ' ') {
        *rest = S(line.p + 2, line.len - 2);
        return 1;
    }
    return 0;
}
static int ordered_of(Str line, uint32_t *num, Str *rest) {
    size_t j = 0;
    while (j < line.len && line.p[j] >= '0' && line.p[j] <= '9' && j < 9) j++;
    if (j == 0 || j + 1 >= line.len || line.p[j] != '.' || line.p[j + 1] != ' ') return 0;
    uint32_t v = 0;
    for (size_t k = 0; k < j; k++) v = v * 10 + (line.p[k] - '0');
    *num = v;
    *rest = S(line.p + j + 2, line.len - j - 2);
    return 1;
}
static int is_continued(Str line) {
    size_t n = 0;
    size_t i = 0;
    while (i < line.len) {
        size_t adv;
        uint32_t ch = u8dec(line.p, line.len, i, &adv);
        if (ch == ' ') { n++; i += adv; }
        else return n >= 2 && !is_js_space(ch);
    }
    return 0;
}
static int has_scheme(Str target) {
    if (target.len == 0) return 0;
    uint8_t c0 = target.p[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return 0;
    size_t i = 1;
    while (i < target.len) {
        uint8_t c = target.p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '-' || c == '.') i++;
        else break;
    }
    return i < target.len && target.p[i] == ':';
}
static int by_source_get(LinkCtx *r, Str path, Str *out_url) {
    size_t lo = 0, hi = r->nbs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = cmp_u16(r->bs[mid].path, path);
        if (c == 0) { *out_url = r->bs[mid].url; return 1; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static int static_contains(LinkCtx *r, Str path) {
    size_t lo = 0, hi = r->nstatic;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = cmp_u16(r->static_paths[mid], path);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static Str resolve_path(LinkCtx *r, Str rel) {
    Str parts[64];
    size_t nparts = 0;
    for (size_t k = 0; k < r->ndir && nparts < 64; k++) parts[nparts++] = r->dir_parts[k];
    size_t start = 0;
    for (size_t i = 0; i <= rel.len; i++) {
        if (i == rel.len || rel.p[i] == '/') {
            Str seg = S(rel.p + start, i - start);
            if (seg.len == 0 || seq(seg, SC("."))) {
            } else if (seq(seg, SC(".."))) {
                if (nparts == 0) return S(NULL, 0);
                nparts--;
            } else {
                if (nparts >= 64) __builtin_trap();
                parts[nparts++] = seg;
            }
            start = i + 1;
        }
    }
    size_t st = r->arena->len;
    for (size_t k = 0; k < nparts; k++) {
        if (k > 0) b_byte(r->arena, '/');
        b_str(r->arena, parts[k]);
    }
    return b_span(r->arena, st);
}
static void broken_insert(LinkCtx *r, Str spath, Str target) {
    BrokenPair *items = (BrokenPair *)r->broken->ptr;
    for (size_t k = 0; k < r->broken->len; k++)
        if (seq(items[k].spath, spath) && seq(items[k].target, target)) return;
    BrokenPair *slot = (BrokenPair *)v_slot(r->arena, r->broken, sizeof(BrokenPair));
    slot->spath = spath; slot->target = target;
}
static void lr_href(LinkCtx *r, Str target) {
    if (has_scheme(target) || sstarts(target, "/") || sstarts(target, "#")) {
        esc_html(&g_html, target);
        return;
    }
    size_t hash = sfindbyte(target, 0, '#');
    Str rel = hash == (size_t)-1 ? target : S(target.p, hash);
    Str frag = hash == (size_t)-1 ? S(target.p + target.len, 0)
                                  : S(target.p + hash, target.len - hash);
    if (rel.len > 0) {
        Str path = resolve_path(r, rel);
        if (path.p) {
            Str url;
            if (by_source_get(r, path, &url)) {
                esc_html(&g_html, url);
                esc_html(&g_html, frag);
                return;
            }
            if (static_contains(r, path)) {
                esc_html(&g_html, r->base);
                b_byte(&g_html, '/');
                esc_html(&g_html, path);
                esc_html(&g_html, frag);
                return;
            }
        }
    }
    broken_insert(r, r->spath, target);
    esc_html(&g_html, target);
}

static int is_escapable(uint8_t c) {
    switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']':
    case '(': case ')': case '#': case '+': case '-': case '.':
    case '!': case '>':
        return 1;
    default:
        return 0;
    }
}
static size_t find_unescaped(Str s, const char *what, size_t from) {
    size_t wl = 0; while (what[wl]) wl++;
    size_t j = from;
    while (j + wl <= s.len) {
        if (s.p[j] == '\\' && j + 1 < s.len && is_escapable(s.p[j + 1])) { j += 2; continue; }
        size_t k = 0;
        while (k < wl && s.p[j + k] == (uint8_t)what[k]) k++;
        if (k == wl) return j;
        j++;
    }
    return (size_t)-1;
}
static size_t find_single_star(Str s, size_t from) {
    size_t j = from;
    while (j < s.len) {
        uint8_t c = s.p[j];
        if (c == '\\' && j + 1 < s.len && is_escapable(s.p[j + 1])) { j += 2; continue; }
        if (c != '*') { j++; continue; }
        if (j + 1 < s.len && s.p[j + 1] == '*') { j += 2; continue; }
        return j;
    }
    return (size_t)-1;
}
static void md_inline(Str s, LinkCtx *link) {
    static const uint8_t SPECIAL[4] = {'\\', '`', '*', '['};
    size_t i = 0;
    while (i < s.len) {
        size_t next = find_in_set(s.p, s.len, i, SPECIAL, 4);
        if (next == (size_t)-1) next = s.len;
        if (next > i) {
            esc_html(&g_html, S(s.p + i, next - i));
            i = next;
        }
        if (i >= s.len) break;
        uint8_t c = s.p[i];
        if (c == '\\' && i + 1 < s.len && is_escapable(s.p[i + 1])) {
            esc_html(&g_html, S(s.p + i + 1, 1));
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t j = (size_t)-1;
            for (size_t k = i + 1; k < s.len; k++)
                if (s.p[k] == '`') { j = k; break; }
            if (j != (size_t)-1 && j > i + 1) {
                b_cstr(&g_html, "<code>");
                esc_html(&g_html, S(s.p + i + 1, j - i - 1));
                b_cstr(&g_html, "</code>");
                i = j + 1;
            } else {
                b_byte(&g_html, '`');
                i++;
            }
            continue;
        }
        if (c == '*') {
            if (i + 1 < s.len && s.p[i + 1] == '*') {
                size_t j = find_unescaped(s, "**", i + 2);
                if (j != (size_t)-1 && j > i + 2) {
                    b_cstr(&g_html, "<strong>");
                    md_inline(S(s.p + i + 2, j - i - 2), link);
                    b_cstr(&g_html, "</strong>");
                    i = j + 2;
                } else {
                    b_cstr(&g_html, "**");
                    i += 2;
                }
                continue;
            }
            size_t j = find_single_star(s, i + 1);
            if (j != (size_t)-1 && j > i + 1) {
                b_cstr(&g_html, "<em>");
                md_inline(S(s.p + i + 1, j - i - 1), link);
                b_cstr(&g_html, "</em>");
                i = j + 1;
            } else {
                b_byte(&g_html, '*');
                i++;
            }
            continue;
        }
        if (c == '[') {
            size_t k = find_unescaped(s, "]", i + 1);
            size_t m = (size_t)-1;
            if (k != (size_t)-1 && k + 1 < s.len && s.p[k + 1] == '(') {
                for (size_t p = k + 2; p < s.len; p++)
                    if (s.p[p] == ')') { m = p; break; }
            }
            Str target = S(s.p, 0);
            if (m != (size_t)-1) target = S(s.p + k + 2, m - k - 2);
            int has_space = 0;
            if (!target.len) has_space = 1;
            else {
                size_t ti = 0;
                while (ti < target.len) {
                    size_t adv;
                    uint32_t cp = u8dec(target.p, target.len, ti, &adv);
                    if (is_js_space(cp)) { has_space = 1; break; }
                    ti += adv;
                }
            }
            if (m != (size_t)-1 && target.len > 0 && !has_space) {
                b_cstr(&g_html, "<a href=\"");
                lr_href(link, target);
                b_cstr(&g_html, "\">");
                md_inline(S(s.p + i + 1, k - i - 1), link);
                b_cstr(&g_html, "</a>");
                i = m + 1;
                continue;
            }
            b_byte(&g_html, '[');
            i++;
            continue;
        }
        if (c < 0x80) {
            esc_html(&g_html, S(s.p + i, 1));
            i++;
        } else {
            size_t adv;
            uint32_t ch = u8dec(s.p, s.len, i, &adv);
            (void)ch;
            b_put(&g_html, s.p + i, adv);
            i += adv;
        }
    }
}

typedef struct { Str html; TocEntry *toc; size_t ntoc; Str excerpt; } MdOut;
/* Render markdown body to g_html; TOC/excerpt are spans into g_html. */
static MdOut render_md(Str src, LinkCtx *link, Buf *arena) {
    Vec lines = {NULL, 0, 0};
    size_t start = 0;
    for (size_t i = 0; i <= src.len; i++) {
        if (i == src.len || src.p[i] == '\n') {
            Str *slot = (Str *)v_slot(arena, &lines, sizeof(Str));
            *slot = S(src.p + start, i - start);
            start = i + 1;
        }
    }
    Str *ln = (Str *)lines.ptr;
    size_t nl = lines.len;
    Vec toc = {NULL, 0, 0};
    Vec taken = {NULL, 0, 0};
    Str excerpt = S(NULL, 0);
    int excerpt_set = 0;
    size_t html_start = g_html.len;
    size_t i = 0;
    while (i < nl) {
        Str line = ln[i];
        if (is_blank(line)) { i++; continue; }
        Str lang;
        if (fence_open(line, &lang)) {
            b_cstr(&g_html, "<pre><code");
            if (lang.len > 0) {
                b_cstr(&g_html, " class=\"language-");
                esc_html(&g_html, lang);
                b_byte(&g_html, '"');
            }
            b_byte(&g_html, '>');
            i++;
            while (i < nl && !is_fence_close(ln[i])) {
                esc_html(&g_html, ln[i]);
                b_byte(&g_html, '\n');
                i++;
            }
            i++;
            b_cstr(&g_html, "</code></pre>\n");
            continue;
        }
        uint8_t depth; Str htext;
        if (heading_of(line, &depth, &htext)) {
            Str id = unique_slug(arena, slugify(arena, htext), &taken);
            b_cstr(&g_html, "<h");
            b_byte(&g_html, (uint8_t)('0' + depth));
            b_cstr(&g_html, " id=\"");
            b_str(&g_html, id);
            b_cstr(&g_html, "\">");
            size_t hs = g_html.len;
            md_inline(htext, link);
            size_t he = g_html.len;
            b_cstr(&g_html, "</h");
            b_byte(&g_html, (uint8_t)('0' + depth));
            b_cstr(&g_html, ">\n");
            if (depth == 2 || depth == 3) {
                TocEntry *e = (TocEntry *)v_slot(arena, &toc, sizeof(TocEntry));
                e->level = depth;
                e->id = id;
                e->html = S(g_html.buf + hs, he - hs);
            }
            i++;
            continue;
        }
        if (is_hr(line)) {
            b_cstr(&g_html, "<hr>\n");
            i++;
            continue;
        }
        if (line.len > 0 && line.p[0] == '>') {
            size_t st = arena->len;
            int first = 1;
            while (i < nl && ln[i].len > 0 && ln[i].p[0] == '>') {
                Str l = ln[i];
                Str stripped = (l.len >= 2 && l.p[1] == ' ')
                    ? S(l.p + 2, l.len - 2) : S(l.p + 1, l.len - 1);
                Str tl = trim_line(stripped);
                if (tl.len > 0) {
                    if (!first) b_byte(arena, '\n');
                    b_str(arena, tl);
                    first = 0;
                }
                i++;
            }
            Str joined = b_span(arena, st);
            if (joined.len == 0) {
                b_cstr(&g_html, "<blockquote>\n</blockquote>\n");
            } else {
                b_cstr(&g_html, "<blockquote>\n<p>");
                md_inline(joined, link);
                b_cstr(&g_html, "</p>\n</blockquote>\n");
            }
            continue;
        }
        Str brest;
        int is_bullet = bullet_of(line, &brest);
        uint32_t onum = 0; Str orest;
        int is_ordered = 0;
        if (!is_bullet) is_ordered = ordered_of(line, &onum, &orest);
        if (is_bullet || is_ordered) {
            int ordered = is_ordered;
            uint32_t stnum = ordered ? onum : 1;
            Vec items = {NULL, 0, 0};
            size_t cur_start = 0;
            int have_cur = 0;
            while (i < nl) {
                Str l = ln[i];
                Str r2; uint32_t n2 = 0;
                int is_item = ordered ? ordered_of(l, &n2, &r2) : bullet_of(l, &r2);
                if (is_item) {
                    if (have_cur) {
                        Str it = b_span(arena, cur_start);
                        Str *slot = (Str *)v_slot(arena, &items, sizeof(Str));
                        *slot = it;
                    }
                    cur_start = arena->len;
                    b_str(arena, trim_line(r2));
                    have_cur = 1;
                    i++;
                    continue;
                }
                if (is_continued(l)) {
                    b_byte(arena, '\n');
                    b_str(arena, trim_line(l));
                    i++;
                } else break;
            }
            if (have_cur) {
                Str it = b_span(arena, cur_start);
                Str *slot = (Str *)v_slot(arena, &items, sizeof(Str));
                *slot = it;
            }
            const char *tag = ordered ? "ol" : "ul";
            if (ordered && stnum != 1) {
                b_cstr(&g_html, "<ol start=\"");
                b_i64(&g_html, (int64_t)stnum);
                b_cstr(&g_html, "\">");
            } else {
                b_byte(&g_html, '<');
                b_cstr(&g_html, tag);
                b_byte(&g_html, '>');
            }
            b_byte(&g_html, '\n');
            for (size_t k = 0; k < items.len; k++) {
                b_cstr(&g_html, "<li>");
                md_inline(((Str *)items.ptr)[k], link);
                b_cstr(&g_html, "</li>\n");
            }
            b_cstr(&g_html, "</");
            b_cstr(&g_html, tag);
            b_cstr(&g_html, ">\n");
            continue;
        }
        /* Paragraph */
        {
            size_t st = arena->len;
            b_str(arena, trim_line(line));
            i++;
            while (i < nl && !ends_para(ln[i])) {
                b_byte(arena, '\n');
                b_str(arena, trim_line(ln[i]));
                i++;
            }
            Str joined = b_span(arena, st);
            b_cstr(&g_html, "<p>");
            size_t ps = g_html.len;
            md_inline(joined, link);
            size_t pe = g_html.len;
            b_cstr(&g_html, "</p>\n");
            if (!excerpt_set) {
                excerpt = S(g_html.buf + ps, pe - ps);
                excerpt_set = 1;
            }
        }
    }
    MdOut out;
    out.html = S(g_html.buf + html_start, g_html.len - html_start);
    out.toc = (TocEntry *)toc.ptr;
    out.ntoc = toc.len;
    out.excerpt = excerpt;
    return out;
}

/* ------------------------------------------------------------------ */
/* Templates: compile to flat instructions, fields interned to ids    */
/* ------------------------------------------------------------------ */
#define OP_TEXT 0
#define OP_VAR 1
#define OP_RAW 2
#define OP_IF 3
#define OP_ELSE 4
#define OP_ENDIF 5
#define OP_FOR 6
#define OP_ENDFOR 7
#define OP_INC 8
#define OP_END 9
typedef struct { uint8_t op; uint8_t x; uint16_t y; uint32_t a; uint32_t b; } Ins;
typedef struct { uint16_t nseg; uint16_t off; } Path;
typedef struct {
    Str name, src;
    Ins *ins; uint32_t nins;
    Path *paths; uint32_t npaths;
    uint16_t *segs;
    Str *incs; uint32_t ninc; /* include names, by OP_INC's a */
} CTpl;

/* Field ids (segments 1+ of a path). 0 = unknown (resolves to UNDEF). */
#define F_UNKNOWN 0
#define F_TITLE 1
#define F_URL 2
#define F_DATE 3
#define F_DATE_LONG 4
#define F_SUMMARY 5
#define F_EXCERPT 6
#define F_READING_MINUTES 7
#define F_TAGS 8
#define F_SLUG 9
#define F_CONTENT 10
#define F_TOC 11
#define F_IS_POST 12
#define F_NEWER 13
#define F_OLDER 14
#define F_NAME 15
#define F_COUNT 16
#define F_POSTS 17
#define F_LEVEL 18
#define F_ID 19
#define F_HTML 20
#define F_NUMBER 21
#define F_TOTAL 22
#define F_NEWER_URL 23
#define F_OLDER_URL 24
#define F_INDEX 25
#define F_FIRST 26
#define F_LAST 27
#define F_LENGTH 28
#define F_BASE_URL 29
static const struct { const char *n; uint16_t id; } FIELDS[] = {
    {"title", F_TITLE}, {"url", F_URL}, {"date", F_DATE}, {"date_long", F_DATE_LONG},
    {"summary", F_SUMMARY}, {"excerpt", F_EXCERPT}, {"reading_minutes", F_READING_MINUTES},
    {"tags", F_TAGS}, {"slug", F_SLUG}, {"content", F_CONTENT}, {"toc", F_TOC},
    {"is_post", F_IS_POST}, {"newer", F_NEWER}, {"older", F_OLDER}, {"name", F_NAME},
    {"count", F_COUNT}, {"posts", F_POSTS}, {"level", F_LEVEL}, {"id", F_ID},
    {"html", F_HTML}, {"number", F_NUMBER}, {"total", F_TOTAL},
    {"newer_url", F_NEWER_URL}, {"older_url", F_OLDER_URL}, {"index", F_INDEX},
    {"first", F_FIRST}, {"last", F_LAST}, {"length", F_LENGTH}, {"base_url", F_BASE_URL},
};
static uint16_t field_id(Str name) {
    for (size_t k = 0; k < sizeof(FIELDS) / sizeof(FIELDS[0]); k++)
        if (seq(name, SC(FIELDS[k].n))) return FIELDS[k].id;
    return F_UNKNOWN;
}
/* Name interning (segment 0 + loop var names). Table lives in the perm. */
static Vec g_names;
static uint16_t ID_SITE, ID_PAGE, ID_TAG, ID_PAGER, ID_TAGS, ID_RECENT, ID_POSTS, ID_LOOP;
static uint16_t intern_name(Str name) {
    for (size_t k = 0; k < g_names.len; k++)
        if (seq(((Str *)g_names.ptr)[k], name)) return (uint16_t)k;
    Str *slot = (Str *)v_slot(&g_perm, &g_names, sizeof(Str));
    *slot = name;
    return (uint16_t)(g_names.len - 1);
}
static int seg_valid(Str seg) {
    if (seg.len == 0) return 0;
    uint8_t c0 = seg.p[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) return 0;
    for (size_t k = 1; k < seg.len; k++) {
        uint8_t c = seg.p[k];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}
static Str tpl_err(Buf *arena, Str name, Str msg) {
    size_t st = arena->len;
    b_cstr(arena, "template \"");
    b_str(arena, name);
    b_cstr(arena, "\": ");
    b_str(arena, msg);
    return b_span(arena, st);
}
/* Convert a dotted path text to interned segment ids; append the path.
 * Returns 0 on error (out_err set), 1 on ok. */
static int path_add(Vec *paths, Vec *segs, Str text, Buf *arena, Str tpl_name, uint32_t *out_idx, Str *out_err) {
    if (text.len == 0) goto bad;
    /* count segments + validate */
    size_t nseg = 1;
    for (size_t k = 0; k < text.len; k++) if (text.p[k] == '.') nseg++;
    uint16_t off = (uint16_t)segs->len;
    size_t start = 0;
    for (size_t si = 0; si < nseg; si++) {
        size_t end = start;
        while (end < text.len && text.p[end] != '.') end++;
        Str seg = S(text.p + start, end - start);
        if (!seg_valid(seg)) goto bad;
        uint16_t id;
        if (si == 0) id = intern_name(seg);
        else id = field_id(seg);
        uint16_t *slot = (uint16_t *)v_slot(arena, segs, sizeof(uint16_t));
        *slot = id;
        start = end + 1;
    }
    {
        Path *p = (Path *)v_slot(arena, paths, sizeof(Path));
        p->nseg = (uint16_t)nseg;
        p->off = off;
        *out_idx = (uint32_t)(paths->len - 1);
        return 1;
    }
bad:;
    size_t st = arena->len;
    b_byte(arena, '"');
    esc_json(arena, text.p, text.len);
    b_byte(arena, '"');
    Str quoted = b_span(arena, st);
    size_t st2 = arena->len;
    b_str(arena, quoted);
    b_cstr(arena, " is not a path");
    *out_err = tpl_err(arena, tpl_name, b_span(arena, st2));
    return 0;
}

/* Compile-time frame stack (IF / FOR nesting). */
#define FR_IF 1
#define FR_FOR 2
typedef struct { uint8_t kind; uint32_t pc; uint32_t else_pc; int has_else; } CFrame;

/* Compile one template. Returns 0 ok, -1 error (out_err set). */
static int compile_tpl(Str name, Str src, CTpl *out, Buf *arena, Str *tpl_names, size_t ntpl, Str *out_err) {
    Vec ins = {NULL, 0, 0};
    Vec paths = {NULL, 0, 0};
    Vec segs = {NULL, 0, 0};
    Vec incs = {NULL, 0, 0};
    (void)tpl_names; (void)ntpl;
    CFrame frames[32];
    int nf = 0;
    size_t text_start = 0;
    size_t i = 0;
    while (i < src.len) {
        size_t open = sfindbyte(src, i, '{');
        if (open == (size_t)-1 || open == src.len - 1) break;
        uint8_t next = src.p[open + 1];
        if (next != '{' && next != '%') { i = open + 1; continue; }
        if (next == '{') {
            int raw = open + 2 < src.len && src.p[open + 2] == '{';
            const uint8_t *pat = (const uint8_t *)(raw ? "}}}" : "}}");
            size_t patlen = raw ? 3 : 2;
            size_t from = open + (raw ? 3 : 2);
            size_t close = sfind(src, from, pat, patlen);
            if (close == (size_t)-1) {
                *out_err = tpl_err(arena, name, SC(raw ? "a {{{ is not closed" : "a {{ is not closed"));
                return -1;
            }
            if (open > text_start) {
                Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                in->op = OP_TEXT; in->x = 0; in->y = 0;
                in->a = (uint32_t)text_start; in->b = (uint32_t)(open - text_start);
            }
            Str inner = trim_js(S(src.p + from, close - from));
            uint32_t pidx;
            if (!path_add(&paths, &segs, inner, arena, name, &pidx, out_err)) return -1;
            Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
            in->op = raw ? OP_RAW : OP_VAR; in->x = 0; in->y = 0;
            in->a = pidx; in->b = 0;
            i = close + patlen;
            text_start = i;
            continue;
        }
        /* {% %} */
        size_t from = open + 2;
        size_t close = sfind(src, from, (const uint8_t *)"%}", 2);
        if (close == (size_t)-1) {
            *out_err = tpl_err(arena, name, SC("a {% is not closed"));
            return -1;
        }
        size_t end = close + 2;
        size_t line_start = 0;
        for (size_t k = open; k-- > 0;) {
            if (src.p[k] == '\n') { line_start = k + 1; break; }
        }
        size_t nl = sfindbyte(src, end, '\n');
        size_t line_end = nl == (size_t)-1 ? src.len : nl;
        int standalone = 1;
        for (size_t k = line_start; k < open; k++)
            if (src.p[k] != ' ' && src.p[k] != '\t') { standalone = 0; break; }
        if (standalone)
            for (size_t k = end; k < line_end; k++)
                if (src.p[k] != ' ' && src.p[k] != '\t') { standalone = 0; break; }
        if (standalone) {
            if (line_start > text_start) {
                Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                in->op = OP_TEXT; in->x = 0; in->y = 0;
                in->a = (uint32_t)text_start; in->b = (uint32_t)(line_start - text_start);
            }
        } else if (open > text_start) {
            Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
            in->op = OP_TEXT; in->x = 0; in->y = 0;
            in->a = (uint32_t)text_start; in->b = (uint32_t)(open - text_start);
        }
        Str tag = trim_js(S(src.p + open + 2, close - open - 2));
        i = standalone ? (nl == (size_t)-1 ? src.len : nl + 1) : end;
        text_start = i;
        /* --- handle the tag --- */
        /* for */
        if (sstarts(tag, "for ")) {
            Str rest = S(tag.p + 4, tag.len - 4);
            size_t sep = sfind(rest, 0, (const uint8_t *)" in ", 4);
            if (sep != (size_t)-1) {
                Str var = S(rest.p, sep);
                Str path = S(rest.p + sep + 4, rest.len - sep - 4);
                int varok = var.len > 0;
                if (varok) {
                    uint8_t c0 = var.p[0];
                    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) varok = 0;
                    for (size_t k = 1; varok && k < var.len; k++) {
                        uint8_t c = var.p[k];
                        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) varok = 0;
                    }
                }
                int pathok = path.len > 0;
                if (pathok) {
                    size_t ti = 0;
                    while (ti < path.len) {
                        size_t adv; uint32_t cp = u8dec(path.p, path.len, ti, &adv);
                        if (is_js_space(cp)) { pathok = 0; break; }
                        ti += adv;
                    }
                }
                if (varok && pathok) {
                    uint32_t pidx;
                    if (!path_add(&paths, &segs, path, arena, name, &pidx, out_err)) return -1;
                    if (nf >= 32) { *out_err = tpl_err(arena, name, SC("nesting too deep")); return -1; }
                    Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                    in->op = OP_FOR; in->x = 0; in->y = intern_name(var);
                    in->a = pidx; in->b = 0;
                    frames[nf].kind = FR_FOR;
                    frames[nf].pc = (uint32_t)(ins.len - 1);
                    frames[nf].else_pc = 0;
                    frames[nf].has_else = 0;
                    nf++;
                    continue;
                }
            }
            /* fall through to the generic error below */
        }
        /* if */
        if (sstarts(tag, "if ")) {
            Str rest = S(tag.p + 3, tag.len - 3);
            uint8_t not = 0;
            if (sstarts(rest, "not ")) { not = 1; rest = S(rest.p + 4, rest.len - 4); }
            int ok = rest.len > 0;
            if (ok) {
                size_t ti = 0;
                while (ti < rest.len) {
                    size_t adv; uint32_t cp = u8dec(rest.p, rest.len, ti, &adv);
                    if (is_js_space(cp)) { ok = 0; break; }
                    ti += adv;
                }
            }
            if (ok) {
                /* validate_path: segments must be valid identifiers */
                size_t nseg = 1;
                for (size_t k = 0; k < rest.len; k++) if (rest.p[k] == '.') nseg++;
                int vok = 1;
                size_t st2 = 0;
                for (size_t si = 0; si < nseg; si++) {
                    size_t en = st2;
                    while (en < rest.len && rest.p[en] != '.') en++;
                    if (!seg_valid(S(rest.p + st2, en - st2))) { vok = 0; break; }
                    st2 = en + 1;
                }
                if (vok) {
                    uint32_t pidx;
                    if (!path_add(&paths, &segs, rest, arena, name, &pidx, out_err)) return -1;
                    if (nf >= 32) { *out_err = tpl_err(arena, name, SC("nesting too deep")); return -1; }
                    Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                    in->op = OP_IF; in->x = not; in->y = 0;
                    in->a = pidx; in->b = 0;
                    frames[nf].kind = FR_IF;
                    frames[nf].pc = (uint32_t)(ins.len - 1);
                    frames[nf].else_pc = 0;
                    frames[nf].has_else = 0;
                    nf++;
                    continue;
                }
                /* invalid path: error quoting the joined path */
                size_t st3 = arena->len;
                b_byte(arena, '"');
                esc_json(arena, rest.p, rest.len);
                b_byte(arena, '"');
                Str quoted = b_span(arena, st3);
                size_t st4 = arena->len;
                b_str(arena, quoted);
                b_cstr(arena, " is not a path");
                *out_err = tpl_err(arena, name, b_span(arena, st4));
                return -1;
            }
            /* fall through to generic error */
        }
        if (seq(tag, SC("else"))) {
            if (nf > 0 && frames[nf - 1].kind == FR_IF && !frames[nf - 1].has_else) {
                Ins *ifins = &((Ins *)ins.ptr)[frames[nf - 1].pc];
                /* IF-false jumps PAST the ELSE, into the else branch.
                 * (Jumping to the ELSE itself would skip the branch,
                 * because ELSE unconditionally jumps past ENDIF.) */
                ifins->b = (uint32_t)(ins.len + 1);
                Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                in->op = OP_ELSE; in->x = 0; in->y = 0; in->a = 0; in->b = 0;
                frames[nf - 1].has_else = 1;
                frames[nf - 1].else_pc = (uint32_t)(ins.len - 1);
                continue;
            }
            *out_err = tpl_err(arena, name, SC("an else outside an if"));
            return -1;
        }
        if (seq(tag, SC("endfor")) || seq(tag, SC("endif"))) {
            int is_for = seq(tag, SC("endfor"));
            if (nf == 0 || (is_for && frames[nf - 1].kind != FR_FOR) || (!is_for && frames[nf - 1].kind != FR_IF)) {
                size_t st = arena->len;
                b_cstr(arena, "an ");
                b_str(arena, tag);
                b_cstr(arena, " with no open ");
                b_cstr(arena, is_for ? "for" : "if");
                *out_err = tpl_err(arena, name, b_span(arena, st));
                return -1;
            }
            CFrame fr = frames[--nf];
            if (is_for) {
                Ins *forins = &((Ins *)ins.ptr)[fr.pc];
                forins->b = (uint32_t)(ins.len + 1); /* after ENDFOR */
                Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                in->op = OP_ENDFOR; in->x = 0; in->y = 0; in->a = 0; in->b = fr.pc;
            } else {
                if (fr.has_else) {
                    Ins *el = &((Ins *)ins.ptr)[fr.else_pc];
                    el->b = (uint32_t)(ins.len + 1); /* after ENDIF */
                } else {
                    Ins *ifins = &((Ins *)ins.ptr)[fr.pc];
                    ifins->b = (uint32_t)(ins.len + 1); /* after ENDIF */
                }
                Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                in->op = OP_ENDIF; in->x = 0; in->y = 0; in->a = 0; in->b = 0;
            }
            continue;
        }
        if (sstarts(tag, "include \"")) {
            Str rest = S(tag.p + 9, tag.len - 9);
            if (rest.len > 0 && rest.p[rest.len - 1] == '"') {
                Str inner = S(rest.p, rest.len - 1);
                int ok = inner.len > 0;
                for (size_t k = 0; ok && k < inner.len; k++) {
                    uint8_t c = inner.p[k];
                    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
                }
                if (ok) {
                    uint32_t iidx = (uint32_t)-1;
                    for (size_t k = 0; k < incs.len; k++)
                        if (seq(((Str *)incs.ptr)[k], inner)) { iidx = (uint32_t)k; break; }
                    if (iidx == (uint32_t)-1) {
                        Str *slot = (Str *)v_slot(arena, &incs, sizeof(Str));
                        *slot = inner;
                        iidx = (uint32_t)(incs.len - 1);
                    }
                    Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
                    in->op = OP_INC; in->x = 0; in->y = 0;
                    in->a = iidx;
                    in->b = 0;
                    continue;
                }
            }
            /* fall through to generic error */
        }
        {
            size_t st = arena->len;
            b_cstr(arena, "{% ");
            b_str(arena, tag);
            b_cstr(arena, " %} is not a tag");
            *out_err = tpl_err(arena, name, b_span(arena, st));
            return -1;
        }
    }
    if (text_start < src.len) {
        Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
        in->op = OP_TEXT; in->x = 0; in->y = 0;
        in->a = (uint32_t)text_start; in->b = (uint32_t)(src.len - text_start);
    }
    if (nf > 0) {
        const char *kind = frames[nf - 1].kind == FR_FOR ? "for" : "if";
        size_t st = arena->len;
        b_cstr(arena, "a ");
        b_cstr(arena, kind);
        b_cstr(arena, " is not closed");
        *out_err = tpl_err(arena, name, b_span(arena, st));
        return -1;
    }
    {
        Ins *in = (Ins *)v_slot(arena, &ins, sizeof(Ins));
        in->op = OP_END; in->x = 0; in->y = 0; in->a = 0; in->b = 0;
    }
    out->name = name;
    out->src = src;
    out->ins = (Ins *)ins.ptr;
    out->nins = (uint32_t)ins.len;
    out->paths = (Path *)paths.ptr;
    out->npaths = (uint32_t)paths.len;
    out->segs = (uint16_t *)segs.ptr;
    out->incs = (Str *)incs.ptr;
    out->ninc = (uint32_t)incs.len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Build data structures                                              */
/* ------------------------------------------------------------------ */
typedef struct { Str title; Str base_url; int64_t per_page; int64_t recent; } Site;
typedef struct { Str path; Str content; } FileInput;
typedef struct { Str name; Str src; } TplPair;
typedef struct { Site site; TplPair *templates; size_t ntpl; FileInput *files; size_t nfiles; } CaseInput;
typedef struct { Str name; Str slug; Str url; } TagRef;
typedef struct {
    Str path; FmData data; Str body; int post;
    Str layout, slug, out_dir, url, html;
    TocEntry *toc; size_t ntoc;
    Str excerpt;
    int newer, older;
    TagRef *tag_refs; size_t ntag_refs;
    int64_t reading_minutes;
    Str date_long;
} Source;
typedef struct { Str name, slug, url; Vec posts; } Tag; /* posts: Vec<size_t> kept indices */

/* ------------------------------------------------------------------ */
/* Renderer: values, field resolution, flat-instruction interpreter    */
/* ------------------------------------------------------------------ */
#define K_UNDEF 0
#define K_NULL 1
#define K_BOOL 2
#define K_NUM 3
#define K_STR 4
#define K_ARR 5
#define K_SITE 7
#define K_PAGE 8
#define K_CARD 9
#define K_LINK 10
#define K_TAG 11
#define K_TAGREF 12
#define K_TOC 13
#define K_PAGER 14
#define K_LOOP 15
#define IT_CARD 0
#define IT_TAG 1
#define IT_TAGREF 2
#define IT_TOC 3
typedef struct LoopFrame LoopFrame;
struct LoopFrame {
    uint16_t var_id;
    uint8_t kind;
    int64_t index, length;
    const void *item;
    uint32_t for_pc;
    const void *a; /* array items pointer */
    const void *b; /* array base (indices case) or NULL */
};
typedef struct {
    uint8_t kind;
    uint8_t akind;
    uint16_t _pad;
    uint32_t an;
    int64_t num;
    Str str;
    const void *a;
    const void *b;
} Val;
static Val v_undef(void) { Val v; v.kind = K_UNDEF; v.akind = 0; v._pad = 0; v.an = 0; v.num = 0; v.str = S(NULL, 0); v.a = NULL; v.b = NULL; return v; }
static Val v_null(void) { Val v = v_undef(); v.kind = K_NULL; return v; }
static Val v_bool(int x) { Val v = v_undef(); v.kind = K_BOOL; v.num = x ? 1 : 0; return v; }
static Val v_num(int64_t n) { Val v = v_undef(); v.kind = K_NUM; v.num = n; return v; }
static Val v_str(Str s) { Val v = v_undef(); v.kind = K_STR; v.str = s; return v; }
static Val v_arr(uint8_t ak, uint32_t n, const void *items, const void *base) {
    Val v = v_undef(); v.kind = K_ARR; v.akind = ak; v.an = n; v.a = items; v.b = base; return v;
}
static Val v_obj(uint8_t kind, const void *ptr) {
    Val v = v_undef(); v.kind = kind; v.a = ptr; return v;
}
static const void *arr_item2(uint8_t akind, const void *items, const void *base, size_t i) {
    if (base) return (const void *)&((Source *)base)[((size_t *)items)[i]];
    switch (akind) {
    case IT_CARD: return ((Source **)items)[i];
    case IT_TAG: return &((Tag *)items)[i];
    case IT_TAGREF: return &((TagRef *)items)[i];
    default: return &((TocEntry *)items)[i];
    }
}
static const void *arr_item(Val *v, size_t i) {
    return arr_item2(v->akind, v->a, v->b, i);
}
static Val val_from_item(uint8_t akind, const void *it) {
    switch (akind) {
    case IT_CARD: return v_obj(K_CARD, it);
    case IT_TAG: return v_obj(K_TAG, it);
    case IT_TAGREF: return v_obj(K_TAGREF, it);
    default: return v_obj(K_TOC, it);
    }
}

typedef struct {
    Str site_title, site_base_url;
    Tag *tags; size_t ntags;
    Source *kept; size_t nkept;
    Source *page; int has_page;
    Source **posts; size_t nposts; int has_posts;
    Source **recent; size_t nrecent;
    Tag *tag; int has_tag;
    int64_t pager_number, pager_total;
    Str pager_url, pager_newer_url, pager_older_url;
    int has_pager;
    LoopFrame loops[16];
    int nloops;
    CTpl *tpls; size_t ntpl;
} Env;

static Val card_field(Source *s, uint16_t fid) {
    switch (fid) {
    case F_TITLE: return v_str(s->data.has_title ? s->data.title : S(NULL, 0));
    case F_URL: return v_str(s->url);
    case F_DATE: return v_str(s->data.has_date ? s->data.date : S(NULL, 0));
    case F_DATE_LONG: return v_str(s->date_long);
    case F_SUMMARY: return v_str(s->data.has_summary ? s->data.summary : S(NULL, 0));
    case F_EXCERPT: return v_str(s->excerpt);
    case F_READING_MINUTES: return v_num(s->reading_minutes);
    case F_TAGS: return v_arr(IT_TAGREF, (uint32_t)s->ntag_refs, s->tag_refs, NULL);
    default: return v_undef();
    }
}
static Val val_field(Env *env, Val c, uint16_t fid) {
    switch (c.kind) {
    case K_SITE:
        if (fid == F_TITLE) return v_str(env->site_title);
        if (fid == F_BASE_URL) return v_str(env->site_base_url);
        return v_undef();
    case K_PAGE: {
        Source *s = (Source *)c.a;
        switch (fid) {
        case F_SLUG: return v_str(s->slug);
        case F_CONTENT: return v_str(s->html);
        case F_TOC: return v_arr(IT_TOC, (uint32_t)s->ntoc, s->toc, NULL);
        case F_IS_POST: return v_bool(s->post);
        case F_NEWER:
            if (s->newer < 0) return v_null();
            return v_obj(K_LINK, &env->kept[s->newer]);
        case F_OLDER:
            if (s->older < 0) return v_null();
            return v_obj(K_LINK, &env->kept[s->older]);
        default: return card_field(s, fid);
        }
    }
    case K_CARD:
        return card_field((Source *)c.a, fid);
    case K_LINK: {
        Source *s = (Source *)c.a;
        switch (fid) {
        case F_TITLE: return v_str(s->data.has_title ? s->data.title : S(NULL, 0));
        case F_URL: return v_str(s->url);
        case F_DATE: return v_str(s->data.has_date ? s->data.date : S(NULL, 0));
        case F_DATE_LONG: return v_str(s->date_long);
        default: return v_undef();
        }
    }
    case K_TAG: {
        Tag *t = (Tag *)c.a;
        switch (fid) {
        case F_NAME: return v_str(t->name);
        case F_SLUG: return v_str(t->slug);
        case F_URL: return v_str(t->url);
        case F_COUNT: return v_num((int64_t)t->posts.len);
        case F_POSTS: return v_arr(IT_CARD, (uint32_t)t->posts.len, t->posts.ptr, env->kept);
        default: return v_undef();
        }
    }
    case K_TAGREF: {
        TagRef *r = (TagRef *)c.a;
        switch (fid) {
        case F_NAME: return v_str(r->name);
        case F_SLUG: return v_str(r->slug);
        case F_URL: return v_str(r->url);
        default: return v_undef();
        }
    }
    case K_TOC: {
        TocEntry *e = (TocEntry *)c.a;
        switch (fid) {
        case F_LEVEL: return v_num(e->level);
        case F_ID: return v_str(e->id);
        case F_HTML: return v_str(e->html);
        default: return v_undef();
        }
    }
    case K_PAGER:
        switch (fid) {
        case F_NUMBER: return v_num(env->pager_number);
        case F_TOTAL: return v_num(env->pager_total);
        case F_URL: return v_str(env->pager_url);
        case F_NEWER_URL: return v_str(env->pager_newer_url);
        case F_OLDER_URL: return v_str(env->pager_older_url);
        default: return v_undef();
        }
    case K_LOOP: {
        LoopFrame *f = (LoopFrame *)c.a;
        switch (fid) {
        case F_INDEX: return v_num(f->index + 1);
        case F_FIRST: return v_bool(f->index == 0);
        case F_LAST: return v_bool(f->index + 1 == f->length);
        case F_LENGTH: return v_num(f->length);
        default: return v_undef();
        }
    }
    case K_ARR:
        if (fid == F_LENGTH) return v_num((int64_t)c.an);
        return v_undef();
    case K_STR:
        if (fid == F_LENGTH) return v_num((int64_t)u16len(c.str));
        return v_undef();
    default:
        return v_undef();
    }
}
static Val val_root(Env *env, uint16_t id) {
    for (int i = env->nloops - 1; i >= 0; i--) {
        if (env->loops[i].var_id == id) {
            switch (env->loops[i].kind) {
            case IT_CARD: return v_obj(K_CARD, env->loops[i].item);
            case IT_TAG: return v_obj(K_TAG, env->loops[i].item);
            case IT_TAGREF: return v_obj(K_TAGREF, env->loops[i].item);
            default: return v_obj(K_TOC, env->loops[i].item);
            }
        }
    }
    if (id == ID_LOOP) {
        if (env->nloops == 0) return v_undef();
        Val v = v_undef(); v.kind = K_LOOP; v.a = &env->loops[env->nloops - 1]; return v;
    }
    if (id == ID_SITE) return v_obj(K_SITE, NULL);
    if (id == ID_PAGE) {
        if (!env->has_page) return v_undef();
        return v_obj(K_PAGE, env->page);
    }
    if (id == ID_TAG) {
        if (!env->has_tag) return v_undef();
        return v_obj(K_TAG, env->tag);
    }
    if (id == ID_PAGER) {
        if (!env->has_pager) return v_undef();
        return v_obj(K_PAGER, NULL);
    }
    if (id == ID_TAGS) return v_arr(IT_TAG, (uint32_t)env->ntags, env->tags, NULL);
    if (id == ID_RECENT) return v_arr(IT_CARD, (uint32_t)env->nrecent, env->recent, NULL);
    if (id == ID_POSTS) {
        if (!env->has_posts) return v_undef();
        return v_arr(IT_CARD, (uint32_t)env->nposts, env->posts, NULL);
    }
    return v_undef();
}
static Val resolve_val(Env *env, CTpl *tpl, uint32_t pidx) {
    Path *p = &tpl->paths[pidx];
    uint16_t *segs = tpl->segs + p->off;
    Val c = val_root(env, segs[0]);
    for (uint16_t k = 1; k < p->nseg; k++) {
        c = val_field(env, c, segs[k]);
        if (c.kind == K_UNDEF) return c;
    }
    return c;
}
static int val_truthy(Val *v) {
    switch (v->kind) {
    case K_ARR: return v->an != 0;
    case K_UNDEF: case K_NULL: return 0;
    case K_BOOL: return v->num != 0;
    case K_STR: return v->str.len != 0;
    case K_NUM: return v->num != 0;
    default: return 1;
    }
}
static void val_write(Val *v, Env *env, int escaped) {
    switch (v->kind) {
    case K_STR:
        if (escaped) esc_html_json(&g_out, v->str);
        else esc_json(&g_out, v->str.p, v->str.len);
        break;
    case K_NUM:
        b_i64(&g_out, v->num);
        break;
    case K_BOOL:
        b_cstr(&g_out, v->num ? "true" : "false");
        break;
    case K_ARR:
        for (uint32_t k = 0; k < v->an; k++) {
            if (k > 0) b_cstr(&g_out, ", ");
            const void *it = arr_item(v, k);
            Val item = val_from_item(v->akind, it);
            val_write(&item, env, escaped);
        }
        break;
    default:
        break;
    }
}
/* Run a compiled template's instructions into g_out (JSON-escaped). */
static void run_tpl(Env *env, CTpl *tpl, int depth) {
    Ins *ins = tpl->ins;
    uint32_t pc = 0;
    while (pc < tpl->nins) {
        Ins *in = &ins[pc];
        switch (in->op) {
        case OP_TEXT:
            esc_json(&g_out, (const uint8_t *)tpl->src.p + in->a, in->b);
            pc++;
            break;
        case OP_VAR: {
            Val v = resolve_val(env, tpl, in->a);
            val_write(&v, env, 1);
            pc++;
            break;
        }
        case OP_RAW: {
            Val v = resolve_val(env, tpl, in->a);
            val_write(&v, env, 0);
            pc++;
            break;
        }
        case OP_IF: {
            Val v = resolve_val(env, tpl, in->a);
            int t = val_truthy(&v);
            if (t != in->x) pc++;
            else pc = in->b;
            break;
        }
        case OP_ELSE:
            pc = in->b;
            break;
        case OP_ENDIF:
            pc++;
            break;
        case OP_FOR: {
            /* Re-entry from ENDFOR? top frame already targets this pc. */
            if (env->nloops > 0 && env->loops[env->nloops - 1].for_pc == pc) {
                LoopFrame *f = &env->loops[env->nloops - 1];
                f->item = arr_item2(f->kind, f->a, f->b, (size_t)f->index);
                pc++;
                break;
            }
            Val v = resolve_val(env, tpl, in->a);
            if (v.kind != K_ARR || v.an == 0 || env->nloops >= 16) { pc = in->b; break; }
            LoopFrame *f = &env->loops[env->nloops++];
            f->var_id = in->y;
            f->kind = v.akind;
            f->index = 0;
            f->length = (int64_t)v.an;
            f->item = arr_item(&v, 0);
            f->for_pc = pc;
            f->a = v.a;
            f->b = v.b;
            pc++;
            break;
        }
        case OP_ENDFOR: {
            LoopFrame *f = &env->loops[env->nloops - 1];
            f->index++;
            if (f->index < f->length) pc = in->b;
            else { env->nloops--; pc++; }
            break;
        }
        case OP_INC: {
            Str nm = tpl->incs[in->a];
            int tidx = -1;
            size_t lo = 0, hi = env->ntpl;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                int c = cmp_u16(env->tpls[mid].name, nm);
                if (c == 0) { tidx = (int)mid; break; }
                if (c < 0) lo = mid + 1; else hi = mid;
            }
            if (tidx >= 0 && depth < 8)
                run_tpl(env, &env->tpls[tidx], depth + 1);
            pc++;
            break;
        }
        default: /* OP_END */
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sort (freestanding quicksort; elements must be <= 512 bytes)        */
/* ------------------------------------------------------------------ */
typedef int (*sg_cmp_fn)(const void *, const void *);
static void sg_swap(uint8_t *a, uint8_t *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}
static void sg_qsort(void *base, size_t n, size_t size, sg_cmp_fn cmp) {
    if (size > 512) __builtin_trap();
    if (n < 2) return;
    uint8_t *b = (uint8_t *)base;
    struct { uint8_t *lo; size_t n; } stack[64];
    int sp = 0;
    stack[sp].lo = b; stack[sp].n = n; sp++;
    while (sp > 0) {
        sp--;
        uint8_t *lo = stack[sp].lo;
        size_t nn = stack[sp].n;
        while (nn > 16) {
            uint8_t *mid = lo + (nn / 2) * size;
            uint8_t *hi = lo + (nn - 1) * size;
            if (cmp(lo, mid) > 0) sg_swap(lo, mid, size);
            if (cmp(lo, hi) > 0) sg_swap(lo, hi, size);
            if (cmp(mid, hi) > 0) sg_swap(mid, hi, size);
            sg_swap(mid, hi - size, size);
            uint8_t *pivot = hi - size;
            uint8_t *i = lo;
            for (uint8_t *j = lo; j < hi - size; j += size) {
                if (cmp(j, pivot) < 0) { sg_swap(i, j, size); i += size; }
            }
            sg_swap(i, pivot, size);
            size_t left_n = (size_t)(i - lo) / size;
            size_t right_n = nn - left_n - 1;
            uint8_t *right_lo = i + size;
            if (left_n > right_n) {
                if (left_n > 0) { stack[sp].lo = lo; stack[sp].n = left_n; sp++; }
                lo = right_lo; nn = right_n;
            } else {
                if (right_n > 0) { stack[sp].lo = right_lo; stack[sp].n = right_n; sp++; }
                nn = left_n;
            }
        }
        for (size_t k = 1; k < nn; k++) {
            uint8_t tmp[512];
            uint8_t *cur = lo + k * size;
            for (size_t t = 0; t < size; t++) tmp[t] = cur[t];
            size_t j = k;
            while (j > 0 && cmp(lo + (j - 1) * size, tmp) > 0) {
                uint8_t *dst = lo + j * size, *src = lo + (j - 1) * size;
                for (size_t t = 0; t < size; t++) dst[t] = src[t];
                j--;
            }
            uint8_t *dst = lo + j * size;
            for (size_t t = 0; t < size; t++) dst[t] = tmp[t];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input parsing (cases.json -> CaseInput). Arrays live in the perm.    */
/* ------------------------------------------------------------------ */
static Site parse_site(P *p) {
    Site s;
    s.title = S(NULL, 0); s.base_url = S(NULL, 0);
    s.per_page = 0; s.recent = 0;
    p_ws(p); p->i++;
    for (;;) {
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == '}') { p->i++; break; }
        Str key = p_string(p);
        p_ws(p); p->i++;
        if (seq(key, SC("title"))) s.title = p_string(p);
        else if (seq(key, SC("base_url"))) s.base_url = p_string(p);
        else if (seq(key, SC("per_page"))) s.per_page = p_i64(p);
        else if (seq(key, SC("recent"))) s.recent = p_i64(p);
        else p_skip(p);
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == ',') p->i++;
        else { p->i++; break; }
    }
    return s;
}
static void parse_templates(P *p, Buf *perm, TplPair **out, size_t *out_n) {
    Vec v = {NULL, 0, 0};
    p_ws(p); p->i++;
    for (;;) {
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == '}') { p->i++; break; }
        Str key = p_string(p);
        p_ws(p); p->i++;
        Str val = p_string(p);
        TplPair *slot = (TplPair *)v_slot(perm, &v, sizeof(TplPair));
        slot->name = key; slot->src = val;
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == ',') p->i++;
        else { p->i++; break; }
    }
    *out = (TplPair *)v.ptr; *out_n = v.len;
}
static void parse_files(P *p, Buf *perm, FileInput **out, size_t *out_n) {
    Vec v = {NULL, 0, 0};
    p_ws(p); p->i++;
    for (;;) {
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == ']') { p->i++; break; }
        Str path = S(NULL, 0);
        Str content = S(NULL, 0);
        p_ws(p); p->i++;
        for (;;) {
            p_ws(p);
            if (p->i < p->len && p->buf[p->i] == '}') { p->i++; break; }
            Str key = p_string(p);
            p_ws(p); p->i++;
            if (seq(key, SC("path"))) path = p_string(p);
            else if (seq(key, SC("content"))) content = p_string(p);
            else p_skip(p);
            p_ws(p);
            if (p->i < p->len && p->buf[p->i] == ',') p->i++;
            else { p->i++; break; }
        }
        FileInput *slot = (FileInput *)v_slot(perm, &v, sizeof(FileInput));
        slot->path = path; slot->content = content;
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == ',') p->i++;
        else { p->i++; break; }
    }
    *out = (FileInput *)v.ptr; *out_n = v.len;
}
static CaseInput parse_case_input(P *p, Buf *perm) {
    CaseInput ci;
    ci.site.title = S(NULL, 0); ci.site.base_url = S(NULL, 0);
    ci.site.per_page = 0; ci.site.recent = 0;
    ci.templates = NULL; ci.ntpl = 0;
    ci.files = NULL; ci.nfiles = 0;
    p_ws(p); p->i++;
    for (;;) {
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == '}') { p->i++; break; }
        Str key = p_string(p);
        p_ws(p); p->i++;
        if (seq(key, SC("site"))) ci.site = parse_site(p);
        else if (seq(key, SC("templates"))) parse_templates(p, perm, &ci.templates, &ci.ntpl);
        else if (seq(key, SC("files"))) parse_files(p, perm, &ci.files, &ci.nfiles);
        else p_skip(p);
        p_ws(p);
        if (p->i < p->len && p->buf[p->i] == ',') p->i++;
        else { p->i++; break; }
    }
    return ci;
}
typedef struct { CaseInput *cases; size_t n; } Cases;
static Cases parse_cases(uint8_t *buf, size_t len, Buf *perm) {
    P p;
    p.buf = buf; p.len = len; p.i = 0;
    Vec v = {NULL, 0, 0};
    p_ws(&p); p.i++;
    p_ws(&p);
    if (p.i < p.len && p.buf[p.i] == ']') {
        Cases c; c.cases = NULL; c.n = 0; return c;
    }
    for (;;) {
        p_ws(&p); p.i++;
        CaseInput input;
        input.site.title = S(NULL, 0); input.site.base_url = S(NULL, 0);
        input.site.per_page = 0; input.site.recent = 0;
        input.templates = NULL; input.ntpl = 0;
        input.files = NULL; input.nfiles = 0;
        for (;;) {
            p_ws(&p);
            if (p.i < p.len && p.buf[p.i] == '}') { p.i++; break; }
            Str key = p_string(&p);
            p_ws(&p); p.i++;
            if (seq(key, SC("input"))) { input = parse_case_input(&p, perm); }
            else p_skip(&p);
            p_ws(&p);
            if (p.i < p.len && p.buf[p.i] == ',') p.i++;
            else { p.i++; break; }
        }
        CaseInput *slot = (CaseInput *)v_slot(perm, &v, sizeof(CaseInput));
        *slot = input;
        p_ws(&p);
        if (p.i < p.len && p.buf[p.i] == ',') p.i++;
        else { p.i++; break; }
    }
    Cases c;
    c.cases = (CaseInput *)v.ptr; c.n = v.len;
    return c;
}

/* ------------------------------------------------------------------ */
/* Pipeline                                                           */
/* ------------------------------------------------------------------ */
static const char *const RESERVED[3] = {"posts", "tags", "page"};
static Str dir_of(Str path) {
    size_t last = (size_t)-1;
    for (size_t i = 0; i < path.len; i++)
        if (path.p[i] == '/') last = i;
    if (last == (size_t)-1) return S(path.p, 0);
    return S(path.p, last);
}
static Str normalize_crlf(Str content, Buf *arena) {
    if (sfindbyte(content, 0, '\r') == (size_t)-1) return content;
    size_t st = arena->len;
    size_t i = 0;
    while (i < content.len) {
        size_t j = i;
        while (j < content.len && content.p[j] != '\r') j++;
        b_put(arena, content.p + i, j - i);
        if (j >= content.len) break;
        b_byte(arena, '\n');
        i = (j + 1 < content.len && content.p[j + 1] == '\n') ? j + 2 : j + 1;
    }
    return b_span(arena, st);
}
static int cmp_tplpair(const void *a, const void *b) {
    return cmp_u16(((const TplPair *)a)->name, ((const TplPair *)b)->name);
}
static int cmp_fileinput(const void *a, const void *b) {
    return cmp_u16(((const FileInput *)a)->path, ((const FileInput *)b)->path);
}
static int cmp_str_q(const void *a, const void *b) {
    return cmp_u16(*(const Str *)a, *(const Str *)b);
}
static int cmp_tag_q(const void *a, const void *b) {
    const Tag *ta = (const Tag *)a, *tb = (const Tag *)b;
    if (ta->posts.len != tb->posts.len) return ta->posts.len < tb->posts.len ? 1 : -1;
    return cmp_u16(ta->slug, tb->slug);
}
static Source *g_sort_kept;
static int cmp_post_idx(const void *a, const void *b) {
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    Source *sa = &g_sort_kept[ia], *sb = &g_sort_kept[ib];
    Str da = sa->data.has_date ? sa->data.date : S(NULL, 0);
    Str db = sb->data.has_date ? sb->data.date : S(NULL, 0);
    int c = cmp_u16(db, da);
    if (c) return c;
    return cmp_u16(sa->slug, sb->slug);
}
static void own_tags(Source *s, Buf *arena, Vec *out) {
    out->ptr = NULL; out->len = 0; out->cap = 0;
    Vec seen = {NULL, 0, 0};
    if (!s->data.has_tags) return;
    for (size_t k = 0; k < s->data.ntags; k++) {
        Str name = s->data.tags[k];
        Str sl = slugify(arena, name);
        int found = 0;
        for (size_t j = 0; j < seen.len; j++)
            if (seq(((Str *)seen.ptr)[j], sl)) { found = 1; break; }
        if (found) continue;
        Str *slot = (Str *)v_slot(arena, &seen, sizeof(Str));
        *slot = sl;
        Str *slot2 = (Str *)v_slot(arena, out, sizeof(Str));
        *slot2 = name;
    }
}
/* Binary search the compiled templates by name. Returns index or -1. */
static int ctpl_find(CTpl *tpls, size_t n, Str name) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = cmp_u16(tpls[mid].name, name);
        if (c == 0) return (int)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}
typedef struct { Str dir; Vec set; } TakenDir;
static Vec *taken_set_for(Vec *taken, Buf *arena, Str dir) {
    for (size_t k = 0; k < taken->len; k++) {
        TakenDir *td = &((TakenDir *)taken->ptr)[k];
        if (seq(td->dir, dir)) return &td->set;
    }
    TakenDir *td = (TakenDir *)v_slot(arena, taken, sizeof(TakenDir));
    td->dir = dir;
    td->set.ptr = NULL; td->set.len = 0; td->set.cap = 0;
    if (dir.len == 0) {
        for (int k = 0; k < 3; k++) {
            Str *slot = (Str *)v_slot(arena, &td->set, sizeof(Str));
            *slot = SC(RESERVED[k]);
        }
    }
    return &td->set;
}
static int strset_contains(Vec *set, Str s) {
    for (size_t k = 0; k < set->len; k++)
        if (seq(((Str *)set->ptr)[k], s)) return 1;
    return 0;
}
static void strset_insert(Buf *arena, Vec *set, Str s) {
    if (strset_contains(set, s)) return;
    Str *slot = (Str *)v_slot(arena, set, sizeof(Str));
    *slot = s;
}
static void pair_begin(int *first, Str path) {
    if (!*first) b_byte(&g_out, ',');
    *first = 0;
    o_json_str(&g_out, path);
    b_byte(&g_out, ':');
    b_byte(&g_out, '"');
}
static void pair_end(void) { b_byte(&g_out, '"'); }

static void build_case(CaseInput *input) {
    Buf *arena = &g_arena;
    /* Templates, parsed in code-unit order of name. */
    Vec names_v = {NULL, 0, 0};
    for (size_t k = 0; k < input->ntpl; k++) {
        TplPair *slot = (TplPair *)v_slot(arena, &names_v, sizeof(TplPair));
        *slot = input->templates[k];
    }
    if (names_v.len > 1)
        sg_qsort(names_v.ptr, names_v.len, sizeof(TplPair), cmp_tplpair);
    Vec report = {NULL, 0, 0};
    Vec parsed_v = {NULL, 0, 0};
    for (size_t k = 0; k < names_v.len; k++) {
        TplPair *tp = &((TplPair *)names_v.ptr)[k];
        CTpl ct;
        Str err;
        if (compile_tpl(tp->name, tp->src, &ct, arena, NULL, 0, &err) == 0) {
            CTpl *slot = (CTpl *)v_slot(arena, &parsed_v, sizeof(CTpl));
            *slot = ct;
        } else {
            Str *slot = (Str *)v_slot(arena, &report, sizeof(Str));
            *slot = err;
        }
    }
    CTpl *ctx = (CTpl *)parsed_v.ptr;
    size_t nctx = parsed_v.len;
    Str base = input->site.base_url;
    while (base.len > 0 && base.p[base.len - 1] == '/') base.len--;
    /* 1. Sources, in code-unit order of path; \r\n? -> \n. */
    Vec files_v = {NULL, 0, 0};
    for (size_t k = 0; k < input->nfiles; k++) {
        FileInput *slot = (FileInput *)v_slot(arena, &files_v, sizeof(FileInput));
        *slot = input->files[k];
    }
    if (files_v.len > 1)
        sg_qsort(files_v.ptr, files_v.len, sizeof(FileInput), cmp_fileinput);
    Vec sources_v = {NULL, 0, 0};
    Vec statics_v = {NULL, 0, 0};
    for (size_t k = 0; k < files_v.len; k++) {
        FileInput *f = &((FileInput *)files_v.ptr)[k];
        if (sends(f->path, ".md")) {
            Str text = normalize_crlf(f->content, arena);
            Source *s = (Source *)v_slot(arena, &sources_v, sizeof(Source));
            s->path = f->path;
            s->data.title = S(NULL, 0); s->data.has_title = 0;
            s->data.date = S(NULL, 0); s->data.has_date = 0;
            s->data.tags = NULL; s->data.ntags = 0; s->data.has_tags = 0;
            s->data.draft = 0; s->data.has_draft = 0;
            s->data.layout = S(NULL, 0); s->data.has_layout = 0;
            s->data.slug = S(NULL, 0); s->data.has_slug = 0;
            s->data.summary = S(NULL, 0); s->data.has_summary = 0;
            s->body = text;
            s->post = 0;
            s->layout = S(NULL, 0);
            s->slug = S(NULL, 0);
            s->out_dir = S(NULL, 0);
            s->url = S(NULL, 0);
            s->html = S(NULL, 0);
            s->toc = NULL; s->ntoc = 0;
            s->excerpt = S(NULL, 0);
            s->newer = -1; s->older = -1;
            s->tag_refs = NULL; s->ntag_refs = 0;
            s->reading_minutes = 0;
            s->date_long = S(NULL, 0);
        } else {
            FileInput *slot = (FileInput *)v_slot(arena, &statics_v, sizeof(FileInput));
            *slot = *f;
        }
    }
    /* 2. Front matter. */
    Vec kept_v = {NULL, 0, 0};
    for (size_t si = 0; si < sources_v.len; si++) {
        Source s = ((Source *)sources_v.ptr)[si];
        FmResult fm = parse_fm(s.body, arena);
        if (!fm.ok) {
            size_t st = arena->len;
            b_cstr(arena, "refused ");
            b_str(arena, s.path);
            b_cstr(arena, ": ");
            b_str(arena, fm.err);
            Str line = b_span(arena, st);
            Str *slot = (Str *)v_slot(arena, &report, sizeof(Str));
            *slot = line;
            continue;
        }
        s.data = fm.data;
        s.body = fm.body;
        s.post = sstarts(s.path, "posts/");
        Str own = S(NULL, 0);
        int have_own = 0;
        if (sstarts(s.path, "tags/")) { own = SC("tags"); have_own = 1; }
        else if (sstarts(s.path, "page/")) { own = SC("page"); have_own = 1; }
        if (have_own) {
            size_t st = arena->len;
            b_cstr(arena, "refused ");
            b_str(arena, s.path);
            b_cstr(arena, ": ");
            b_str(arena, own);
            b_cstr(arena, "/ is the generator's own");
            Str line = b_span(arena, st);
            Str *slot = (Str *)v_slot(arena, &report, sizeof(Str));
            *slot = line;
            continue;
        }
        if (s.post && !s.data.has_date) {
            size_t st = arena->len;
            b_cstr(arena, "refused ");
            b_str(arena, s.path);
            b_cstr(arena, ": a post needs a date");
            Str line = b_span(arena, st);
            Str *slot = (Str *)v_slot(arena, &report, sizeof(Str));
            *slot = line;
            continue;
        }
        Str layout = s.data.has_layout ? s.data.layout : SC(s.post ? "post" : "page");
        if (ctpl_find(ctx, nctx, layout) < 0) {
            size_t st = arena->len;
            b_cstr(arena, "refused ");
            b_str(arena, s.path);
            b_cstr(arena, ": layout \"");
            b_str(arena, layout);
            b_cstr(arena, "\" is no template that parses");
            Str line = b_span(arena, st);
            Str *slot = (Str *)v_slot(arena, &report, sizeof(Str));
            *slot = line;
            continue;
        }
        s.layout = layout;
        if (s.data.has_draft && s.data.draft) {
            size_t st = arena->len;
            b_cstr(arena, "draft ");
            b_str(arena, s.path);
            Str line = b_span(arena, st);
            Str *slot = (Str *)v_slot(arena, &report, sizeof(Str));
            *slot = line;
            continue;
        }
        Source *slot = (Source *)v_slot(arena, &kept_v, sizeof(Source));
        *slot = s;
    }
    Source *kept = (Source *)kept_v.ptr;
    size_t nkept = kept_v.len;
    /* 3. Permalinks. */
    Vec taken = {NULL, 0, 0};
    Vec by_source_v = {NULL, 0, 0};
    for (size_t si = 0; si < nkept; si++) {
        Source *s = &kept[si];
        Str dir = s->post ? SC("posts") : dir_of(s->path);
        Vec *set = taken_set_for(&taken, arena, dir);
        Str slug = unique_slug(arena,
            s->data.has_slug ? s->data.slug : slugify(arena, s->data.has_title ? s->data.title : S(NULL, 0)),
            set);
        s->slug = slug;
        if (dir.len == 0) {
            s->out_dir = slug;
        } else {
            size_t st = arena->len;
            b_str(arena, dir);
            b_byte(arena, '/');
            b_str(arena, slug);
            s->out_dir = b_span(arena, st);
        }
        {
            size_t st = arena->len;
            b_str(arena, base);
            b_byte(arena, '/');
            b_str(arena, s->out_dir);
            b_byte(arena, '/');
            s->url = b_span(arena, st);
        }
        BySource *slot = (BySource *)v_slot(arena, &by_source_v, sizeof(BySource));
        slot->path = s->path; slot->url = s->url;
    }
    /* 4. Markdown. */
    Vec static_paths_v = {NULL, 0, 0};
    for (size_t k = 0; k < statics_v.len; k++) {
        Str *slot = (Str *)v_slot(arena, &static_paths_v, sizeof(Str));
        *slot = ((FileInput *)statics_v.ptr)[k].path;
    }
    Vec broken = {NULL, 0, 0};
    for (size_t idx = 0; idx < nkept; idx++) {
        Source *s = &kept[idx];
        Str dir = dir_of(s->path);
        Vec dir_parts_v = {NULL, 0, 0};
        if (dir.len > 0) {
            size_t start = 0;
            for (size_t i = 0; i <= dir.len; i++) {
                if (i == dir.len || dir.p[i] == '/') {
                    Str *slot = (Str *)v_slot(arena, &dir_parts_v, sizeof(Str));
                    *slot = S(dir.p + start, i - start);
                    start = i + 1;
                }
            }
        }
        LinkCtx resolver;
        resolver.bs = (BySource *)by_source_v.ptr;
        resolver.nbs = by_source_v.len;
        resolver.static_paths = (Str *)static_paths_v.ptr;
        resolver.nstatic = static_paths_v.len;
        resolver.base = base;
        resolver.dir_parts = (Str *)dir_parts_v.ptr;
        resolver.ndir = dir_parts_v.len;
        resolver.spath = s->path;
        resolver.broken = &broken;
        resolver.arena = arena;
        MdOut md = render_md(s->body, &resolver, arena);
        s->html = md.html;
        s->toc = md.toc;
        s->ntoc = md.ntoc;
        s->excerpt = md.excerpt;
    }
    /* 5. Collections. */
    Vec posts_v = {NULL, 0, 0};
    for (size_t i = 0; i < nkept; i++)
        if (kept[i].post) {
            size_t *slot = (size_t *)v_slot(arena, &posts_v, sizeof(size_t));
            *slot = i;
        }
    size_t *posts = (size_t *)posts_v.ptr;
    size_t nposts = posts_v.len;
    g_sort_kept = kept;
    if (nposts > 1)
        sg_qsort(posts, nposts, sizeof(size_t), cmp_post_idx);
    for (size_t k = 0; k < nposts; k++) {
        size_t p = posts[k];
        kept[p].newer = k == 0 ? -1 : (int)posts[k - 1];
        kept[p].older = k + 1 == nposts ? -1 : (int)posts[k + 1];
    }
    /* 6. Tags. */
    Vec tags_v = {NULL, 0, 0};
    for (size_t pi = 0; pi < nposts; pi++) {
        size_t p = posts[pi];
        Vec names;
        own_tags(&kept[p], arena, &names);
        Vec refs = {NULL, 0, 0};
        for (size_t ni = 0; ni < names.len; ni++) {
            Str name = ((Str *)names.ptr)[ni];
            Str slug = slugify(arena, name);
            size_t ti = (size_t)-1;
            for (size_t t = 0; t < tags_v.len; t++)
                if (seq(((Tag *)tags_v.ptr)[t].slug, slug)) { ti = t; break; }
            if (ti == (size_t)-1) {
                ti = tags_v.len;
                Tag *tg = (Tag *)v_slot(arena, &tags_v, sizeof(Tag));
                tg->name = name; tg->slug = slug;
                {
                    size_t st = arena->len;
                    b_str(arena, base);
                    b_cstr(arena, "/tags/");
                    b_str(arena, slug);
                    b_byte(arena, '/');
                    tg->url = b_span(arena, st);
                }
                tg->posts.ptr = NULL; tg->posts.len = 0; tg->posts.cap = 0;
            }
            Tag *tg = &((Tag *)tags_v.ptr)[ti];
            size_t *slot = (size_t *)v_slot(arena, &tg->posts, sizeof(size_t));
            *slot = p;
            TagRef *rslot = (TagRef *)v_slot(arena, &refs, sizeof(TagRef));
            tg = &((Tag *)tags_v.ptr)[ti];
            rslot->name = tg->name; rslot->slug = tg->slug; rslot->url = tg->url;
        }
        kept[p].tag_refs = (TagRef *)refs.ptr;
        kept[p].ntag_refs = refs.len;
    }
    for (size_t idx = 0; idx < nkept; idx++) {
        if (kept[idx].post) continue;
        Vec names;
        own_tags(&kept[idx], arena, &names);
        Vec refs = {NULL, 0, 0};
        for (size_t ni = 0; ni < names.len; ni++) {
            Str name = ((Str *)names.ptr)[ni];
            Str slug = slugify(arena, name);
            size_t ti = (size_t)-1;
            for (size_t t = 0; t < tags_v.len; t++)
                if (seq(((Tag *)tags_v.ptr)[t].slug, slug)) { ti = t; break; }
            TagRef *rslot = (TagRef *)v_slot(arena, &refs, sizeof(TagRef));
            if (ti != (size_t)-1) {
                Tag *tg = &((Tag *)tags_v.ptr)[ti];
                rslot->name = tg->name; rslot->slug = tg->slug; rslot->url = tg->url;
            } else {
                rslot->name = name; rslot->slug = slug; rslot->url = S(NULL, 0);
            }
        }
        kept[idx].tag_refs = (TagRef *)refs.ptr;
        kept[idx].ntag_refs = refs.len;
    }
    Tag *tags = (Tag *)tags_v.ptr;
    size_t ntags = tags_v.len;
    if (ntags > 1)
        sg_qsort(tags, ntags, sizeof(Tag), cmp_tag_q);
    /* 7. Layouts, streamed straight into the answer JSON. */
    Vec output_paths = {NULL, 0, 0};
    for (size_t k = 0; k < nkept; k++) {
        int64_t reading = (int64_t)((words_of(kept[k].body) + 199) / 200);
        if (reading < 1) reading = 1;
        kept[k].reading_minutes = reading;
        kept[k].date_long = kept[k].data.has_date ? long_date(arena, kept[k].data.date) : S(NULL, 0);
    }
    Source **post_cards = (Source **)b_alloc(arena, (nposts ? nposts : 1) * sizeof(Source *));
    for (size_t k = 0; k < nposts; k++) post_cards[k] = &kept[posts[k]];
    int64_t recent_n = input->site.recent;
    size_t nrecent = recent_n < 0 ? nposts : (size_t)recent_n;
    if (nrecent > nposts) nrecent = nposts;
    Env env;
    env.site_title = input->site.title;
    env.site_base_url = base;
    env.tags = tags; env.ntags = ntags;
    env.kept = kept; env.nkept = nkept;
    env.page = NULL; env.has_page = 0;
    env.posts = NULL; env.nposts = 0; env.has_posts = 0;
    env.recent = post_cards; env.nrecent = nrecent;
    env.tag = NULL; env.has_tag = 0;
    env.pager_number = 0; env.pager_total = 0;
    env.pager_url = S(NULL, 0);
    env.pager_newer_url = S(NULL, 0);
    env.pager_older_url = S(NULL, 0);
    env.has_pager = 0;
    env.nloops = 0;
    env.tpls = ctx; env.ntpl = nctx;
    b_byte(&g_out, '{');
    int first = 1;
    for (size_t si = 0; si < nkept; si++) {
        Source *s = &kept[si];
        size_t st = arena->len;
        b_str(arena, s->out_dir);
        b_cstr(arena, "/index.html");
        Str path = b_span(arena, st);
        strset_insert(arena, &output_paths, path);
        int tidx = ctpl_find(ctx, nctx, s->layout);
        env.page = s; env.has_page = 1;
        env.has_posts = 0; env.has_pager = 0; env.has_tag = 0;
        env.nloops = 0;
        pair_begin(&first, path);
        run_tpl(&env, &ctx[tidx], 0);
        pair_end();
    }
    int64_t per = input->site.per_page;
    int64_t pages = (int64_t)(((int64_t)nposts + per - 1) / per);
    if (pages < 1) pages = 1;
    int index_idx = ctpl_find(ctx, nctx, SC("index"));
    int tag_idx = ctpl_find(ctx, nctx, SC("tag"));
    int tags_idx = ctpl_find(ctx, nctx, SC("tags"));
    for (int64_t k = 1; k <= pages; k++) {
        size_t lo = (size_t)((k - 1) * per);
        int64_t hi64 = k * per;
        size_t hi = hi64 < (int64_t)nposts ? (size_t)hi64 : nposts;
        Str url_k, url_newer, url_older;
        {
            size_t st = arena->len;
            if (k == 1) { b_str(arena, base); b_byte(arena, '/'); }
            else { b_str(arena, base); b_cstr(arena, "/page/"); b_i64(arena, k); b_byte(arena, '/'); }
            url_k = b_span(arena, st);
        }
        if (k == 1) url_newer = S(NULL, 0);
        else {
            size_t st = arena->len;
            if (k - 1 == 1) { b_str(arena, base); b_byte(arena, '/'); }
            else { b_str(arena, base); b_cstr(arena, "/page/"); b_i64(arena, k - 1); b_byte(arena, '/'); }
            url_newer = b_span(arena, st);
        }
        if (k == pages) url_older = S(NULL, 0);
        else {
            size_t st = arena->len;
            b_str(arena, base); b_cstr(arena, "/page/"); b_i64(arena, k + 1); b_byte(arena, '/');
            url_older = b_span(arena, st);
        }
        Str path;
        if (k == 1) path = SC("index.html");
        else {
            size_t st = arena->len;
            b_cstr(arena, "page/");
            b_i64(arena, k);
            b_cstr(arena, "/index.html");
            path = b_span(arena, st);
        }
        if (index_idx != -1) {
            strset_insert(arena, &output_paths, path);
            env.has_page = 0; env.has_tag = 0;
            env.posts = &post_cards[lo]; env.nposts = hi - lo; env.has_posts = 1;
            env.pager_number = k; env.pager_total = pages;
            env.pager_url = url_k; env.pager_newer_url = url_newer; env.pager_older_url = url_older;
            env.has_pager = 1;
            env.nloops = 0;
            pair_begin(&first, path);
            run_tpl(&env, &ctx[index_idx], 0);
            pair_end();
        }
    }
    for (size_t ti = 0; ti < ntags; ti++) {
        Tag *t = &tags[ti];
        size_t st = arena->len;
        b_cstr(arena, "tags/");
        b_str(arena, t->slug);
        b_cstr(arena, "/index.html");
        Str path = b_span(arena, st);
        if (tag_idx != -1) {
            strset_insert(arena, &output_paths, path);
            env.has_page = 0; env.has_posts = 0; env.has_pager = 0;
            env.tag = t; env.has_tag = 1;
            env.nloops = 0;
            pair_begin(&first, path);
            run_tpl(&env, &ctx[tag_idx], 0);
            pair_end();
        }
    }
    if (tags_idx != -1) {
        strset_insert(arena, &output_paths, SC("tags/index.html"));
        env.has_page = 0; env.has_posts = 0; env.has_pager = 0; env.has_tag = 0;
        env.nloops = 0;
        pair_begin(&first, SC("tags/index.html"));
        run_tpl(&env, &ctx[tags_idx], 0);
        pair_end();
    }
    size_t n_statics = statics_v.len;
    for (size_t k = 0; k < statics_v.len; k++) {
        FileInput *f = &((FileInput *)statics_v.ptr)[k];
        if (strset_contains(&output_paths, f->path)) {
            size_t st = arena->len;
            b_cstr(arena, "shadowed ");
            b_str(arena, f->path);
            b_cstr(arena, ": a built page has its path");
            Str line = b_span(arena, st);
            Str *slot = (Str *)v_slot(arena, &report, sizeof(Str));
            *slot = line;
        } else {
            strset_insert(arena, &output_paths, f->path);
            pair_begin(&first, f->path);
            esc_json(&g_out, f->content.p, f->content.len);
            pair_end();
        }
    }
    /* 8. Report. */
    Vec lines = report;
    for (size_t k = 0; k < broken.len; k++) {
        BrokenPair *bp = &((BrokenPair *)broken.ptr)[k];
        size_t st = arena->len;
        b_cstr(arena, "broken-link ");
        b_str(arena, bp->spath);
        b_cstr(arena, " -> ");
        b_str(arena, bp->target);
        Str line = b_span(arena, st);
        Str *slot = (Str *)v_slot(arena, &lines, sizeof(Str));
        *slot = line;
    }
    if (lines.len > 1)
        sg_qsort(lines.ptr, lines.len, sizeof(Str), cmp_str_q);
    size_t n_pages = 0;
    for (size_t k = 0; k < nkept; k++) if (!kept[k].post) n_pages++;
    {
        size_t st = arena->len;
        b_cstr(arena, "built ");
        b_i64(arena, (int64_t)n_pages);
        b_cstr(arena, " pages, ");
        b_i64(arena, (int64_t)nposts);
        b_cstr(arena, " posts, ");
        b_i64(arena, (int64_t)ntags);
        b_cstr(arena, " tags and ");
        b_i64(arena, (int64_t)n_statics);
        b_cstr(arena, " static files");
        Str line = b_span(arena, st);
        Str *slot = (Str *)v_slot(arena, &lines, sizeof(Str));
        *slot = line;
    }
    pair_begin(&first, SC("build-report.txt"));
    for (size_t k = 0; k < lines.len; k++) {
        if (k > 0) b_cstr(&g_out, "\\n");
        esc_json(&g_out, ((Str *)lines.ptr)[k].p, ((Str *)lines.ptr)[k].len);
    }
    b_cstr(&g_out, "\\n");
    pair_end();
    b_byte(&g_out, '}');
}

/* ------------------------------------------------------------------ */
/* Exports + static buffers                                           */
/* ------------------------------------------------------------------ */
#define INPUT_CAP (8u * 1024u * 1024u)
#define ARENA_CAP (16u * 1024u * 1024u)
#define PERM_CAP (1u * 1024u * 1024u)
#define OUT_CAP (16u * 1024u * 1024u)
#define HTML_CAP (4u * 1024u * 1024u)
static uint8_t g_input[INPUT_CAP];
static uint8_t g_arena_mem[ARENA_CAP];
static uint8_t g_perm_mem[PERM_CAP];
static uint8_t g_out_mem[OUT_CAP];
static uint8_t g_html_mem[HTML_CAP];
static size_t g_input_len;

uint8_t *alloc(size_t size) {
    if (size > INPUT_CAP) __builtin_trap();
    g_input_len = size;
    return g_input;
}
const uint8_t *out_ptr(void) { return g_out.buf; }
size_t out_len(void) { return g_out.len; }
void run_all(void) {
    g_arena.buf = g_arena_mem; g_arena.len = 0; g_arena.cap = ARENA_CAP;
    g_perm.buf = g_perm_mem; g_perm.len = 0; g_perm.cap = PERM_CAP;
    g_html.buf = g_html_mem; g_html.len = 0; g_html.cap = HTML_CAP;
    g_out.buf = g_out_mem; g_out.len = 0; g_out.cap = OUT_CAP;
    g_names.ptr = NULL; g_names.len = 0; g_names.cap = 0;
    ID_SITE = intern_name(SC("site"));
    ID_PAGE = intern_name(SC("page"));
    ID_TAG = intern_name(SC("tag"));
    ID_PAGER = intern_name(SC("pager"));
    ID_TAGS = intern_name(SC("tags"));
    ID_RECENT = intern_name(SC("recent"));
    ID_POSTS = intern_name(SC("posts"));
    ID_LOOP = intern_name(SC("loop"));
    Cases cases = parse_cases(g_input, g_input_len, &g_perm);
    b_byte(&g_out, '[');
    int first = 1;
    for (size_t ci = 0; ci < cases.n; ci++) {
        if (!first) b_byte(&g_out, ',');
        first = 0;
        build_case(&cases.cases[ci]);
        g_arena.len = 0;
        g_html.len = 0;
    }
    b_byte(&g_out, ']');
}

#ifdef SG_NATIVE_MAIN
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cases.json> <answers.json>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open input"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n > INPUT_CAP) { fprintf(stderr, "input too large\n"); return 2; }
    uint8_t *in = alloc((size_t)n);
    if (fread(in, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);
    run_all();
    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror("open output"); return 2; }
    if (fwrite(out_ptr(), 1, out_len(), o) != out_len()) { fprintf(stderr, "short write\n"); return 2; }
    fclose(o);
    fprintf(stderr, "wrote %s (%zu bytes)\n", argv[2], out_len());
    return 0;
}
#endif
