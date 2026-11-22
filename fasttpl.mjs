// fasttpl.mjs — fast template engine: same parse (verbatim semantics) +
// codegen to JS functions (new Function) with a mutable scope object.
// Byte-identical to the reference template.js.
import { escapeHtml } from './fastlib.mjs';

export const INCLUDE_DEPTH = 8;

const PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF = /^if (not )?(\S+)$/;
const INCLUDE = /^include "([a-z0-9-]+)"$/;

export class TemplateError extends Error {}

function pathOf(name, text) {
  if (!PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = /^[ \t]*$/.test(src.slice(lineStart, open)) && /^[ \t]*$/.test(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

export function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = FOR.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = IF.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = INCLUDE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

// ---------- runtime helpers (passed to generated code) ----------
// A "pieces" value is a plain array of strings carrying the marker __pieces
// (the markdown HTML of a page, not yet joined). It must behave exactly like
// the joined string in every template operation; the helpers below enforce
// that. __empty = joined string is '', __len = joined string length.
export function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) {
    if (v.__pieces) return v.join('');
    return v.map(textOf).join(', ');
  }
  return '';
}

export function truthy(v) {
  if (Array.isArray(v)) {
    if (v.__pieces) return !v.__empty;
    return v.length > 0;
  }
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

// getProp(v, k): v[k] if v is a non-null object with own property k, else undefined.
// For a pieces array this is always undefined: the reference never traverses
// into a string (typeof string !== 'object' -> undefined), so a pieces value
// behaves exactly like the joined string here.
export function getProp(v, k) {
  if (v !== null && typeof v === 'object') {
    if (Array.isArray(v) && v.__pieces) return undefined;
    return Object.hasOwn(v, k) ? v[k] : undefined;
  }
  return undefined;
}

// pushRaw(O, v): raw {{{ }}} output. A pieces value is spliced piece by piece
// into O (no join); anything else is pushed as textOf(v).
export function pushRaw(O, v) {
  if (Array.isArray(v) && v.__pieces) {
    for (let i = 0; i < v.length; i++) O.push(v[i]);
  } else O.push(textOf(v));
}

// pushEsc(O, v, E): escaped {{ }} output. A pieces value is joined then
// escaped (the reference escapes the joined string); anything else is
// E(textOf(v)) as before.
export function pushEsc(O, v, E) {
  if (Array.isArray(v) && v.__pieces) O.push(E(v.join('')));
  else O.push(E(textOf(v)));
}

// ---------- codegen ----------
// Generated function signature:
//   fn(O, S, X, D, E, txt, tru, get)
//   O: output array, S: scope object (null-prototype), X: {T: Map name->fn},
//   D: include depth, E: escapeHtml, txt: textOf, tru: truthy, get: getProp
//
// Loop bindings are compile-time: a stack of {name, elemVar, loopVar}.
// For a path's first segment:
//   - if it matches a loop binding (innermost first): use the JS variable.
//   - else: S["name"].

function jsStr(s) {
  return JSON.stringify(s);
}

function genPathExpr(path, loopStack) {
  // returns a JS expression resolving the path
  const first = path[0];
  let expr;
  // 'loop' always resolves to the innermost loop object (the reference builds
  // vars as {[name]: x, loop: {...}} so a loop var named 'loop' is shadowed).
  if (loopStack.length > 0 && first === 'loop') {
    expr = loopStack[loopStack.length - 1].loopVar;
  } else {
    for (let i = loopStack.length - 1; i >= 0; i--) {
      if (loopStack[i].name === first) { expr = loopStack[i].elemVar; break; }
    }
  }
  if (expr === undefined) expr = `S[${jsStr(first)}]`;
  for (let k = 1; k < path.length; k++) {
    expr = `get(${expr},${jsStr(path[k])})`;
  }
  return expr;
}

function genNodes(nodes, loopStack, counter, staticSet, memoRead) {
  let code = '';
  for (const n of nodes) {
    if (n.kind === 'text') {
      code += `O.push(${jsStr(n.text)});`;
    } else if (n.kind === 'var') {
      code += `pE(O,${genPathExpr(n.path, loopStack)},E);`;
    } else if (n.kind === 'raw') {
      code += `pR(O,${genPathExpr(n.path, loopStack)});`;
    } else if (n.kind === 'if') {
      const cond = `tru(${genPathExpr(n.path, loopStack)})`;
      const test = n.not ? `!(${cond})` : cond;
      code += `if(${test}){${genNodes(n.then, loopStack, counter, staticSet, memoRead)}}`;
      if (n.else !== null) code += `else{${genNodes(n.else, loopStack, counter, staticSet, memoRead)}}`;
    } else if (n.kind === 'for') {
      const id = counter.next++;
      const xsVar = `_xs${id}`, kVar = `_k${id}`, elVar = `_e${id}`, lpVar = `_l${id}`;
      const oxVar = `_ox${id}`, olVar = `_ol${id}`;
      const pathExpr = genPathExpr(n.path, loopStack);
      code += `{const ${xsVar}=${pathExpr};if(Array.isArray(${xsVar})&&!${xsVar}.__pieces){`;
      code += `const ${oxVar}=S[${jsStr(n.name)}];const ${olVar}=S["loop"];`;
      code += `const ${lpVar}={index:0,first:false,last:false,length:${xsVar}.length};`;
      code += `for(let ${kVar}=0;${kVar}<${xsVar}.length;${kVar}++){`;
      code += `const ${elVar}=${xsVar}[${kVar}];`;
      code += `${lpVar}.index=${kVar}+1;${lpVar}.first=${kVar}===0;${lpVar}.last=${kVar}===${xsVar}.length-1;`;
      code += `S[${jsStr(n.name)}]=${elVar};S["loop"]=${lpVar};`;
      loopStack.push({ name: n.name, elemVar: elVar, loopVar: lpVar });
      code += genNodes(n.body, loopStack, counter, staticSet, memoRead);
      loopStack.pop();
      code += `}S[${jsStr(n.name)}]=${oxVar};S["loop"]=${olVar};}}`;
    } else if (n.kind === 'include') {
      const id = counter.next++;
      if (staticSet !== null && staticSet.has(n.name)) {
        code += `inc(O,S,X,D,${jsStr(n.name)},E,txt,tru,get,pR,pE);`;
      } else if (memoRead !== null && memoRead.has(n.name)) {
        code += `incM(O,S,X,D,${jsStr(n.name)},${jsStr(memoRead.get(n.name))},E,txt,tru,get,pR,pE,inc);`;
      } else {
        code += `if(D<8){const _f${id}=X.T.get(${jsStr(n.name)});if(_f${id})_f${id}(O,S,X,D+1,E,txt,tru,get,pR,pE,inc,incM);}`;
      }
    }
  }
  return code;
}

/** Compile parsed nodes into a function. `staticSet` (or null) names the
 * templates whose output is identical for every render in the case;
 * `memoRead` (or null) maps a template name to the single scope variable its
 * output depends on (for memoization by that variable's identity). */
export function compileTemplate(nodes, staticSet, memoRead) {
  const counter = { next: 0 };
  const body = genNodes(nodes, [], counter, staticSet ?? null, memoRead ?? null);
  // eslint-disable-next-line no-new-func
  return new Function('O', 'S', 'X', 'D', 'E', 'txt', 'tru', 'get', 'pR', 'pE', 'inc', 'incM', body);
}

/**
 * Render a case-static template into O (or push the cached string). A
 * case-static template reads only case-level scope (site/tags/recent), so its
 * output is identical for every render in the case; render it once and reuse.
 * `inc` is passed to generated functions as their 11th parameter.
 */
export function incStatic(O, S, X, D, name, E, txt, tru, get, pR, pE) {
  if (D >= 8) return;
  const C = X.C;
  let c = C.get(name);
  if (c === undefined) {
    const f = X.T.get(name);
    if (f === undefined) { C.set(name, ''); return; }
    const tmp = [];
    f(tmp, S, X, D + 1, E, txt, tru, get, pR, pE, incStatic, incMemo);
    c = tmp.join('');
    C.set(name, c);
  }
  O.push(c);
}

/**
 * Render a single-read template into O (or push the cached string for the
 * same read value). A single-read template's output depends only on the one
 * scope variable it reads (plus case-level data, constant per case), so
 * renders with the same read value (by identity for objects, by value for
 * primitives) are identical. `incM` is passed to generated functions as
 * their 12th parameter. The read objects are never mutated after they are
 * built (cards/views are created once per case), so the cache stays valid.
 */
export function incMemo(O, S, X, D, name, readName, E, txt, tru, get, pR, pE, inc) {
  if (D >= 8) return;
  const f = X.T.get(name);
  if (f === undefined) return;
  let m = X.M.get(name);
  if (m === undefined) { m = new Map(); X.M.set(name, m); }
  const key = S[readName];
  let c = m.get(key);
  if (c === undefined) {
    const tmp = [];
    f(tmp, S, X, D + 1, E, txt, tru, get, pR, pE, inc, incMemo);
    c = tmp.join('');
    m.set(key, c);
  }
  O.push(c);
}

// ---------- static analysis: which templates are case-static? ----------
// A template's external reads are the first segments of the paths it reads
// (var/raw/if/for), excluding names bound by an enclosing loop (the loop
// variable and 'loop'). Its transitive reads add the reads of every template
// it includes (fixed-point, so include cycles are handled). A template is
// case-static when its transitive reads are a subset of {site, tags, recent}.
const CASE_VARS = new Set(['site', 'tags', 'recent']);

function directReads(nodes, bound) {
  const reads = new Set();
  const walk = (ns, b) => {
    for (const n of ns) {
      if (n.kind === 'var' || n.kind === 'raw') {
        if (!b.has(n.path[0])) reads.add(n.path[0]);
      } else if (n.kind === 'if') {
        if (!b.has(n.path[0])) reads.add(n.path[0]);
        walk(n.then, b);
        if (n.else !== null) walk(n.else, b);
      } else if (n.kind === 'for') {
        if (!b.has(n.path[0])) reads.add(n.path[0]);
        const nb = new Set(b);
        nb.add(n.name);
        nb.add('loop');
        walk(n.body, nb);
      }
    }
  };
  walk(nodes, bound ?? new Set());
  return reads;
}

function collectIncludes(nodes, out) {
  for (const n of nodes) {
    if (n.kind === 'include') out.add(n.name);
    else if (n.kind === 'if') {
      collectIncludes(n.then, out);
      if (n.else !== null) collectIncludes(n.else, out);
    } else if (n.kind === 'for') collectIncludes(n.body, out);
  }
  return out;
}

/** Compile all templates (name -> source) into a Map name -> fn. Parse errors -> name -> error message. */
export function compileTemplates(templates) {
  const fns = new Map();
  const errors = [];
  const names = Object.keys(templates).sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  // Pass 1: parse all templates.
  const trees = new Map();
  for (const name of names) {
    try {
      trees.set(name, parseTemplate(name, templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      errors.push(err.message);
    }
  }
  // Pass 2: transitive reads (fixed-point over includes) -> static set.
  const reads = new Map();
  const incs = new Map();
  for (const [name, nodes] of trees) {
    reads.set(name, directReads(nodes));
    incs.set(name, collectIncludes(nodes, new Set()));
  }
  let changed = true;
  while (changed) {
    changed = false;
    for (const [name, incSet] of incs) {
      const r = reads.get(name);
      for (const inc of incSet) {
        const ir = reads.get(inc);
        if (ir === undefined) continue;
        for (const x of ir) {
          if (!r.has(x)) { r.add(x); changed = true; }
        }
      }
    }
  }
  const staticSet = new Set();
  for (const [name, r] of reads) {
    let ok = true;
    for (const x of r) {
      if (!CASE_VARS.has(x)) { ok = false; break; }
    }
    if (ok) staticSet.add(name);
  }
  // Single-read memoization: a non-static template with exactly one external
  // read is memoized by that read's value (e.g. `card` by the post object).
  const memoRead = new Map();
  for (const [name, r] of reads) {
    if (staticSet.has(name)) continue;
    if (r.size === 1) memoRead.set(name, [...r][0]);
  }
  // Pass 3: compile with the static set + memo map (include codegen uses them).
  for (const [name, nodes] of trees) {
    fns.set(name, compileTemplate(nodes, staticSet, memoRead));
  }
  return { fns, errors, staticSet, memoRead };
}

/**
 * Render a compiled template. Returns string or null (missing template).
 * `scope` is used directly as S when it is already a null-prototype object
 * (the caller builds a fresh one per render); a plain object is copied into
 * one. `ctx` is the shared {T: fns} for the case (reused across renders).
 */
export function renderCompiled(fns, name, scope, ctx) {
  const fn = fns.get(name);
  if (fn === undefined) return null;
  const S = Object.getPrototypeOf(scope) === null ? scope : Object.assign(Object.create(null), scope);
  const O = [];
  const X = ctx ?? { T: fns, C: new Map(), M: new Map() };
  if (X.C === undefined) X.C = new Map();
  if (X.M === undefined) X.M = new Map();
  // A case-static template rendered at top level is cached the same way.
  if (X.S !== undefined && X.S.has(name)) {
    let c = X.C.get(name);
    if (c === undefined) {
      fn(O, S, X, 0, escapeHtml, textOf, truthy, getProp, pushRaw, pushEsc, incStatic, incMemo);
      c = O.join('');
      X.C.set(name, c);
      return c;
    }
    return c;
  }
  fn(O, S, X, 0, escapeHtml, textOf, truthy, getProp, pushRaw, pushEsc, incStatic, incMemo);
  return O.join('');
}
