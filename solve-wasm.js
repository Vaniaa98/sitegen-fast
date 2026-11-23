// solve.js — WebAssembly entry for makefun-site 1.0.0 (v9).
// The whole pipeline (JSON parse, front matter, Markdown, templates, JSON
// write) runs in freestanding C compiled to WebAssembly (Zig), on a
// bump-arena / zero-copy pipeline. The driver hands raw cases.json bytes in
// and takes raw answers.json bytes out; no JSON.parse or JSON.stringify runs
// in JavaScript. The .wasm bytes are embedded as base64 in wasm-b64.js (no
// separate binary file in the repo).
//
// Single entry point: one run_all() call does everything (parse + build
// every case + stream the answers.json out). No per-case JS->WASM calls.
import { wasmB64 } from './wasm-b64.js';

const debug = process.env.V9_DEBUG === '1';
const t0 = debug ? process.hrtime.bigint() : 0n;
const bytes = Buffer.from(wasmB64, 'base64');
const t1 = debug ? process.hrtime.bigint() : 0n;
const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
const t2 = debug ? process.hrtime.bigint() : 0n;
const { memory, alloc, run_all, out_ptr, out_len } = instance.exports;

if (debug) {
  const us = (a, b) => Number(b - a) / 1000;
  console.error(`[v9] base64 decode: ${us(t0, t1).toFixed(0)} us, wasm instantiate: ${us(t1, t2).toFixed(0)} us, wasm bytes: ${bytes.length}, memory bytes: ${memory.buffer.byteLength}`);
}

/** cases.json bytes -> answers.json bytes. */
export function runAll(input) {
  const t3 = debug ? process.hrtime.bigint() : 0n;
  const ptr = alloc(input.length);
  new Uint8Array(memory.buffer).set(input, ptr);
  run_all();
  const t4 = debug ? process.hrtime.bigint() : 0n;
  const len = out_len();
  const out = Buffer.from(new Uint8Array(memory.buffer, out_ptr(), len));
  if (debug) {
    const us = (a, b) => Number(b - a) / 1000;
    console.error(`[v9] run_all: ${us(t3, t4).toFixed(0)} us, out bytes: ${len}`);
  }
  return out;
}
