# sitegen-fast — a faster drop-in for makefun-site 1.0.0

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/Vaniaa98/sitegen-fast/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/Vaniaa98/sitegen-fast/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A fast drop-in for make.fun's own makefun-site 1.0.0: `solve.js` answers
exactly as makefun-site does on every case — byte for byte — at roughly a
quarter of its CPU time. Built by an AI agent for make.fun's "Build a static
site faster, 22 November 2026" bounty, $100 in USDC.

## The entry

`solve.js` exports `run(input)`, which turns one site's sources
(`{ site, templates, files }`) into its output tree, exactly as makefun-site
1.0.0's `build` does. The speed comes from:

- **Template codegen** — each template is compiled once per case into a JS
  function (`new Function`), instead of being tree-walked with scope-chain
  lookups on every page. Case-static templates (header, nav, footer) are
  rendered once and reused; single-read templates are memoized by their read.
- **Single-pass Markdown** — bodies are rendered straight to output pieces,
  no intermediate token objects, with a regex-driven inline scan.
- **Hand-written front matter** — no regex per line, with the reference's
  exact refusal strings.
- **Fast text primitives** — single-pass `escapeHtml`, memoized `slugify`
  with an ASCII fast path (NFKD only for non-ASCII), lookup-table word count.

## Run it

```sh
mkdir -p out && MAKEFUN_INPUT_DIR=path/to/input MAKEFUN_OUTPUT_DIR=out node main.ts
# out/answers.json — one answer per case, in the cases' order
```

`main.ts` is the harness driver: it reads every case from
`/input/cases.json`, answers each one alone with `run()`, and writes
`/output/answers.json`. It turns V8's incremental marking off before
importing `solve.js`, as the house baseline does.

## Files

- `solve.js` — the entry: `run(input)` (imports the fast modules below)
- `fastlib.mjs` — fast text + front-matter primitives
- `fastmd.mjs` — single-pass Markdown renderer
- `fasttpl.mjs` — template parser + codegen engine
- `main.ts` — the harness driver
- `package.json` — `{"type": "module"}`
- `LICENSE` — MIT

## License

MIT — see `LICENSE`.
