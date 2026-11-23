// solve.js — N-API native addon entry for makefun-site 1.0.0 (v11).
//
// The whole pipeline (JSON parse, front matter, Markdown, templates, JSON
// write) runs in freestanding C (v10) compiled as a native Node-API addon
// (addon.node). The driver hands raw cases.json bytes in and takes raw
// answers.json bytes out; no JSON.parse or JSON.stringify runs in
// JavaScript. Single entry point: one runAll() call does everything.
//
// Fallback (like schneizel's "WebAssembly fallback"): if the .node fails
// to load (e.g. Node version mismatch), the driver falls back to the
// verified WASM v9 implementation (wasm-b64.js + solve-wasm.js), so the
// entry stays valid (~600k) instead of failing outright.
import { createRequire } from 'node:module';

const debug = process.env.V11_DEBUG === '1';
const t0 = debug ? process.hrtime.bigint() : 0n;

let runAll;
let backend = 'napi';
try {
  const require = createRequire(import.meta.url);
  const addon = require('./addon.node');
  runAll = addon.runAll;
  if (typeof runAll !== 'function') throw new Error('addon has no runAll');
} catch (e) {
  backend = 'wasm';
  if (debug) console.error(`[v11] N-API load failed (${e.message}); falling back to WASM v9`);
  const wasm = await import('./solve-wasm.js');
  runAll = wasm.runAll;
}

const t1 = debug ? process.hrtime.bigint() : 0n;
if (debug) {
  console.error(`[v11] backend: ${backend}, load: ${Number(t1 - t0) / 1000} us`);
}

/** cases.json bytes -> answers.json bytes. */
export { runAll };
export const backendUsed = backend;
