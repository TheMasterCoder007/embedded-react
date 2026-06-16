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

// usePersistentState — a useState that survives the simulator's hot reload (see
// tools/simulator/README.md). The simulator's build transform (persist-transform.mjs) rewrites the app's plain `useState`
// into this helper, so persistence is transparent — you normally never call this directly; it's also
// exported for explicit use. Outside the simulator (e.g., on a device, where the host doesn't install
// `__erPersist`) it is exactly useState, so the same app code runs everywhere.
//
// Values must be JSON-serializable (numbers, strings, booleans, plain objects/arrays). The simulator
// resets the persisted state when you press R (manual reload) or restart it.
import { useState, useCallback } from 'react';

/**
 * Drop-in useState that persists across simulator reloads, keyed by a stable string.
 *
 * @param {string} key       Stable identity for this piece of state (unique within the app).
 * @param {*|Function} initial  Initial value (or a lazy initializer), used when nothing is persisted.
 * @returns {[*, Function]} [value, setValue] — same shape as useState.
 */
export function usePersistentState(key, initial) {
  const [value, setValue] = useState(() => {
    const store = globalThis.__erPersist;
    if (store) {
      const stored = store.get(key);
      if (stored !== undefined) {
        try {
          return JSON.parse(stored);
        } catch {
          /* corrupt/incompatible — fall back to initial */
        }
      }
    }
    return typeof initial === 'function' ? initial() : initial;
  });

  const set = useCallback(
    (next) => {
      setValue((prev) => {
        const v = typeof next === 'function' ? next(prev) : next;
        const store = globalThis.__erPersist;
        if (store) {
          try {
            store.set(key, JSON.stringify(v));
          } catch {
            /* unserializable value — skip persistence, keep the in-memory state */
          }
        }
        return v;
      });
    },
    [key],
  );

  return [value, set];
}
