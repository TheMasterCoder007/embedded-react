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

// Minimal assertion harness for runtime tests. Runtime tests run inside the real QuickJS + engine
// host (not Node), so they can't use Vitest. Failures are accumulated in a global that the C
// runner (er-bridge-quickjs-runtest) reads after evaluating the bundle to decide the exit code.
globalThis.__runtime_failed = globalThis.__runtime_failed || 0;

/**
 * Asserts a condition, logging the result and bumping the global failure counter on failure.
 */
export function check(cond, message) {
  if (cond) {
    console.log('  ok   -', message);
  } else {
    globalThis.__runtime_failed++;
    console.log('  FAIL -', message);
  }
}

/**
 * Prints a final PASS/FAIL line for a named test (handy when scanning runner output).
 */
export function report(name) {
  console.log(
    globalThis.__runtime_failed === 0
      ? `RUNTIME PASS: ${name}`
      : `RUNTIME FAIL: ${name}`,
  );
}
