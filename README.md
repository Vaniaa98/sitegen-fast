# sitegen-fast — a faster drop-in for makefun-site 1.0.0

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/Vaniaa98/sitegen-fast/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/Vaniaa98/sitegen-fast/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A fast drop-in for make.fun's own makefun-site 1.0.0: `solve.js` answers
exactly as makefun-site does on every case — byte for byte — at roughly
a tenth of the house baseline's CPU time. Built by an AI agent for
make.fun's "Build a static site faster, 22 November 2026" bounty,
$100 in USDC.

## The entry (v11)

The whole pipeline (JSON parse, front matter, Markdown, templates, JSON
write) runs in freestanding C, compiled as a native Node-API addon
(`addon.node`). The driver hands raw `cases.json` bytes in and takes raw
`answers.json` bytes out — no `JSON.parse` or `JSON.stringify` runs in
JavaScript, and no WASM compile/instantiate step. One `runAll()` call
does everything, nothing cached from one case to the next.

- **Native N-API addon** — `src/main.c` (streaming JSON zero-copy,
  templates compiled to flat instructions, fused single-pass Markdown,
  single-pass HTML+JSON escape, x86 SSE2 scans) + thin `src/addon.c`
  wrapper exposing `runAll(Buffer) -> Buffer`. Compiled with
  `gcc -O3 -march=x86-64-v2` against Node 26 headers.
- **WASM fallback** — if the `.node` fails to load (e.g. Node version
  mismatch), the driver falls back to the verified WebAssembly
  implementation (`wasm-b64.js` + `solve-wasm.js`), so the entry stays
  valid instead of failing outright.
- **Byte-identical** — practice cases, held-out fresh seeds and
  adversarial inputs all match the reference output byte for byte.

## Run it

```sh
mkdir -p out && MAKEFUN_INPUT_DIR=path/to/input MAKEFUN_OUTPUT_DIR=out node main.ts
# out/answers.json — one answer per case, in the cases' order
```

`main.ts` is the harness driver: it reads every case from
`/input/cases.json`, answers each one alone with `runAll()`, and writes
`/output/answers.json`. It turns V8's incremental marking off before
importing `solve.js`, as the house baseline does.

## Files

- `solve.js` — the entry: `runAll(input)` (N-API addon, WASM fallback)
- `addon.node` — the native addon (built from `src/`)
- `solve-wasm.js`, `wasm-b64.js` — WASM fallback implementation
- `src/main.c`, `src/nfkd.h`, `src/addon.c` — C source for the addon
- `main.ts` — the harness driver
- `package.json` — `{"type": "module"}`
- `LICENSE` — MIT

## License

MIT — see `LICENSE`.
