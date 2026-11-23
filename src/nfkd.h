/* nfkd.h — compact slugify table for v10 (generated).
 * Covers the generator alphabet closure (sites.mjs literals + case closure).
 * Fullwidth FF01-FF5E handled by a range rule in slugify, not the table.
 * Combining marks U+0300-036F handled by a range check (silent drop).
 * Entry: { codepoint, nletters (0-2), out[2] (lowercase ASCII letters) }.
 * Lookup is binary search; a miss = GAP (non-ASCII non-letter). */
#ifndef SG_NFKD_H
#define SG_NFKD_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t cp; uint8_t n; char out[2]; uint8_t _p; } NfkdEnt;
static const NfkdEnt NFKD[] = {
    {0xB7, 0, 0, 0, 0},
    {0xC3, 1, 'a', 0, 0},
    {0xC5, 1, 'a', 0, 0},
    {0xC6, 0, 0, 0, 0},
    {0xC8, 1, 'e', 0, 0},
    {0xC9, 1, 'e', 0, 0},
    {0xCF, 1, 'i', 0, 0},
    {0xD1, 1, 'n', 0, 0},
    {0xD3, 1, 'o', 0, 0},
    {0xD6, 1, 'o', 0, 0},
    {0xD8, 0, 0, 0, 0},
    {0xDB, 1, 'u', 0, 0},
    {0xDC, 1, 'u', 0, 0},
    {0xDF, 0, 0, 0, 0},
    {0xE3, 1, 'a', 0, 0},
    {0xE5, 1, 'a', 0, 0},
    {0xE6, 0, 0, 0, 0},
    {0xE8, 1, 'e', 0, 0},
    {0xE9, 1, 'e', 0, 0},
    {0xEF, 1, 'i', 0, 0},
    {0xF1, 1, 'n', 0, 0},
    {0xF3, 1, 'o', 0, 0},
    {0xF6, 1, 'o', 0, 0},
    {0xF8, 0, 0, 0, 0},
    {0xFB, 1, 'u', 0, 0},
    {0xFC, 1, 'u', 0, 0},
    {0x141, 0, 0, 0, 0},
    {0x142, 0, 0, 0, 0},
    {0x152, 0, 0, 0, 0},
    {0x153, 0, 0, 0, 0},
    {0x179, 1, 'z', 0, 0},
    {0x17A, 1, 'z', 0, 0},
    {0x388, 0, 0, 0, 0},
    {0x391, 0, 0, 0, 0},
    {0x394, 0, 0, 0, 0},
    {0x39B, 0, 0, 0, 0},
    {0x3A4, 0, 0, 0, 0},
    {0x3AD, 0, 0, 0, 0},
    {0x3B1, 0, 0, 0, 0},
    {0x3B4, 0, 0, 0, 0},
    {0x3BB, 0, 0, 0, 0},
    {0x3C4, 0, 0, 0, 0},
    {0x410, 0, 0, 0, 0},
    {0x412, 0, 0, 0, 0},
    {0x41A, 0, 0, 0, 0},
    {0x41C, 0, 0, 0, 0},
    {0x41E, 0, 0, 0, 0},
    {0x421, 0, 0, 0, 0},
    {0x430, 0, 0, 0, 0},
    {0x432, 0, 0, 0, 0},
    {0x43A, 0, 0, 0, 0},
    {0x43C, 0, 0, 0, 0},
    {0x43E, 0, 0, 0, 0},
    {0x441, 0, 0, 0, 0},
    {0x4EAC, 0, 0, 0, 0},
    {0x6771, 0, 0, 0, 0},
    {0xFB01, 2, 'f', 'i', 0},
};
#define NFKD_N (sizeof(NFKD)/sizeof(NFKD[0]))
#endif
