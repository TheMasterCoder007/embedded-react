import {transform} from 'sucrase';

import {persistUseState} from './persist';

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
const EXTENSIONS = [
  '',
  '.jsx',
  '.js',
  '.tsx',
  '.ts',
  '/index.jsx',
  '/index.js',
];

/** Normalizes `./a/../b` against the importing file's directory. */
function resolvePath(from: string, specifier: string): string {
  const parts = from.split('/').slice(0, -1);
  for (const segment of specifier.split('/')) {
    if (segment === '.' || segment === '') continue;
    if (segment === '..') parts.pop();
    else parts.push(segment);
  }
  return parts.join('/');
}

/**
 * Turns the editor's files into one bundle the simulator can run: each file is transformed (JSX,
 * ESM to CommonJS) and wrapped in a module function, then a small loader wires the `require`s
 * together. Imports of `react` and `embedded-react` resolve to the vendor bundle the build step
 * staged, and an asset import evaluates to the asset's name, as the simulator's own bundler does.
 *
 * @param entry  The file to run.
 * @param files  The editor's files.
 * @param vendor  The path to the vendor bundle.
 * @param generation  Folded into the persisted-state keys; bump it to start from fresh state.
 */
export function compile(
  entry: string,
  files: Files,
  vendor: string,
  generation = 0,
): string {
  const modules: string[] = [];
  for (const [path, code] of Object.entries(files)) {
    let transformed: string;
    try {
      // State survives edits, as under the dev server. The rewrite only understands JavaScript;
      // TypeScript files run without it.
      const source = /\.tsx?$/.test(path)
        ? code
        : persistUseState(code, `${generation}:${path}`);
      transformed = transform(source, {
        transforms: [
          'jsx',
          'imports',
          ...(/\.tsx?$/.test(path) ? (['typescript'] as const) : []),
        ],
        jsxRuntime: 'automatic',
        production: true,
        filePath: path,
      }).code;
    } catch (caught) {
      const error = caught as Error & {loc?: {line: number; column: number}};
      const position = /\((\d+):(\d+)\)\s*$/.exec(error.message);
      throw new CompileError(
        error.message
          .replace(/^Error transforming [^:]*: /, '')
          .replace(/\s*\(\d+:\d+\)\s*$/, ''),
        path,
        error.loc?.line ?? (position ? Number(position[1]) : undefined),
        error.loc?.column ?? (position ? Number(position[2]) : undefined),
      );
    }
    modules.push(
      `${JSON.stringify(path)}: function (module, exports, require) {\n${transformed}\n}`,
    );
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
    return function require(specifier) {
      var vendor = globalThis.__erVendor[specifier];
      if (vendor) return vendor;
      if (specifier[0] !== '.') throw new Error('Cannot find module "' + specifier + '" (the playground only has react and embedded-react)');
      var base = resolvePath(from, specifier);
      if (ASSET.test(base)) return base.split('/').pop().replace(/\\.[^.]+$/, '');
      for (var index = 0; index < EXTENSIONS.length; index++) {
        var candidate = base + EXTENSIONS[index];
        if (files[candidate]) return load(candidate);
      }
      throw new Error('Cannot find module "' + specifier + '" from ' + from);
    };
  }
  load(${JSON.stringify(entry)});
})();
`;
  return vendor + '\n' + loader;
}
