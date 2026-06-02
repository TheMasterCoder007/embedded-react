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
  console.log(globalThis.__runtime_failed === 0 ? `RUNTIME PASS: ${name}` : `RUNTIME FAIL: ${name}`);
}
