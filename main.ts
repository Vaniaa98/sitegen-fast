// main.ts (v11) — the program the harness runs. It reads /input/cases.json
// as raw bytes, answers every case inside the native N-API addon (one
// runAll call, nothing cached from one case to the next), and writes
// /output/answers.json. No JSON.parse / JSON.stringify runs in JavaScript.
// Outside a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in
// for /input and /output.
//
// The addon is loaded with a DYNAMIC import, AFTER the flags are set
// (same pattern as v8/v9/v10: a static import would be hoisted and the
// flags would not apply to anything the driver loads).
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';

const FLAGS =
  process.env.V8_FLAGS ?? '--no-incremental-marking';
if (FLAGS) {
  setFlagsFromString(FLAGS);
}

const { runAll } = await import('./solve.js');

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const input = readFileSync(join(INPUT_DIR, 'cases.json'));
writeFileSync(join(OUTPUT_DIR, 'answers.json'), runAll(input));
