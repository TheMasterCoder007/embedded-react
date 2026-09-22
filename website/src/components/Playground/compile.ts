import {transform} from 'sucrase';

export type Files = Record<string, string>;

export class CompileError extends Error {
  constructor(
    message: string,
    public file: string,
    public line?: number,
    public column?: number,
  ) {
    super(message);
  }
}

const ASSET = /\.(png|jpe?g|webp|gif|bmp|svg|ttf|otf)$/i;
const EXTENSIONS = ['', '.jsx', '.js', '.tsx', '.ts', '/index.jsx', '/index.js'];

/** Normalizes `./a/../b` against the importing file's directory. */
function resolvePath(from: string, spec: string): string {
  const parts = from.split('/').slice(0, -1);
  for (const seg of spec.split('/')) {
    if (seg === '.' || seg === '') continue;
    if (seg === '..') parts.pop();
    else parts.push(seg);
  }
  return parts.join('/');
}

/**
 * Turns the editor's files into one bundle the simulator can run: each file is transformed (JSX,
 * ESM to CommonJS) and wrapped in a module function, then a small loader wires the `require`s
 * together. Imports of `react` and `embedded-react` resolve to the vendor bundle the build step
 * staged, and an asset import evaluates to the asset's name, as the simulator's own bundler does.
 */
export function compile(entry: string, files: Files, vendor: string): string {
  const modules: string[] = [];
  for (const [path, code] of Object.entries(files)) {
    let js: string;
    try {
      js = transform(code, {
        transforms: ['jsx', 'imports', ...(/\.tsx?$/.test(path) ? (['typescript'] as const) : [])],
        jsxRuntime: 'automatic',
        production: true,
        filePath: path,
      }).code;
    } catch (e) {
      const err = e as Error & {loc?: {line: number; column: number}};
      const m = /\((\d+):(\d+)\)\s*$/.exec(err.message);
      throw new CompileError(
        err.message.replace(/^Error transforming [^:]*: /, '').replace(/\s*\(\d+:\d+\)\s*$/, ''),
        path,
        err.loc?.line ?? (m ? Number(m[1]) : undefined),
        err.loc?.column ?? (m ? Number(m[2]) : undefined),
      );
    }
    modules.push(`${JSON.stringify(path)}: function (module, exports, require) {\n${js}\n}`);
  }

  // Everything below runs inside QuickJS. Kept dependency-free and ES2020.
  const loader = `
;(function () {
  var files = {\n${modules.join(',\n')}\n};
  var cache = {};
  var resolvePath = ${resolvePath.toString()};
  var EXTENSIONS = ${JSON.stringify(EXTENSIONS)};
  var ASSET = ${ASSET.toString()};
  function load(path) {
    if (cache[path]) return cache[path].exports;
    var module = {exports: {}};
    cache[path] = module;
    files[path](module, module.exports, makeRequire(path));
    return module.exports;
  }
  function makeRequire(from) {
    return function require(spec) {
      var vendor = globalThis.__erVendor[spec];
      if (vendor) return vendor;
      if (spec[0] !== '.') throw new Error('Cannot find module "' + spec + '" (the playground only has react and embedded-react)');
      var base = resolvePath(from, spec);
      if (ASSET.test(base)) return base.split('/').pop().replace(/\\.[^.]+$/, '');
      for (var i = 0; i < EXTENSIONS.length; i++) {
        var p = base + EXTENSIONS[i];
        if (files[p]) return load(p);
      }
      throw new Error('Cannot find module "' + spec + '" from ' + from);
    };
  }
  load(${JSON.stringify(entry)});
})();
`;
  return vendor + '\n' + loader;
}
