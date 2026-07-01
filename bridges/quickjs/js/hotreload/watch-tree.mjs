/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// watchTree — recursively watch a directory across platforms.
//
// fs.watch's `{recursive: true}` is only supported on macOS, Windows, and Node >=20 on Linux; on older
// Linux/Node it throws. A naive fallback that watches just the root misses edits under src/** — which makes
// the dev loop look broken. So where native recursive watching isn't available we fall back to a
// per-directory watcher that also picks up directories created after startup.

import {watch, readdirSync, statSync} from 'node:fs';
import {join} from 'node:path';

// Directories never worth watching (build output, deps, VCS/tool caches). Skipping them keeps the watcher
// count low on the per-directory fallback and avoids rebuild storms from generated files.
const IGNORE_DIR =
  /^(node_modules|dist|build|out|coverage|\.git|\.hg|\.svn|\.next|\.cache)$/;

/**
 * Recursively watch `root`, calling `onEvent(eventType, filename)` on any change beneath it. Uses the
 * native recursive watcher where the platform supports it, else a per-directory fallback.
 *
 * @param {string} root
 * @param {(eventType: string, filename: string|null) => void} onEvent
 * @returns {{close: () => void}}
 */
export function watchTree(root, onEvent) {
  try {
    const w = watch(root, {recursive: true}, onEvent);
    return {close: () => w.close()};
  } catch {
    // Native recursive watching unavailable (older Linux/Node, or an unsupported FS) — use the fallback.
    return watchTreePolyfill(root, onEvent);
  }
}

/**
 * Per-directory recursive watcher: one non-recursive `fs.watch` per directory under `root`, plus hooks for
 * directories created later. This is what makes `src/**` edits fire on platforms without native recursion.
 * Exported for direct testing (so the fallback can be exercised even where native recursion works).
 *
 * @param {string} root
 * @param {(eventType: string, filename: string|null) => void} onEvent
 * @returns {{close: () => void}}
 */
export function watchTreePolyfill(root, onEvent) {
  const watchers = new Map(); // absolute dir path -> FSWatcher

  const watchDir = dir => {
    if (watchers.has(dir)) return;
    let w;
    try {
      w = watch(dir, {}, (event, name) => {
        onEvent(event, name);
        // `mkdir src/components` needs its own watcher — hook any freshly-created subdirectory.
        if (!name || IGNORE_DIR.test(name)) return;
        const child = join(dir, name);
        try {
          if (statSync(child).isDirectory()) scan(child);
        } catch {
          /* a file (not a dir), or created-then-removed — ignore */
        }
      });
    } catch {
      return; // dir vanished between scan and watch
    }
    watchers.set(dir, w);
  };

  const scan = dir => {
    watchDir(dir);
    let entries;
    try {
      entries = readdirSync(dir, {withFileTypes: true});
    } catch {
      return;
    }
    for (const ent of entries)
      if (ent.isDirectory() && !IGNORE_DIR.test(ent.name))
        scan(join(dir, ent.name));
  };

  scan(root);
  return {
    close() {
      for (const w of watchers.values()) w.close();
      watchers.clear();
    },
  };
}
