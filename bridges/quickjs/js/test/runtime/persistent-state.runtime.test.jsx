// Runtime e2e: usePersistentState restores its value across a remount when a __erPersist host is
// present (the simulator's state preservation), and falls back to plain useState (the default) when
// it isn't (a device). Mounts real React through the reconciler; "reload" is modeled as unmount +
// remount, which re-runs the useState initializer just like a fresh context after a hot reload.
import { createRoot } from '../../src/renderer.js';
import { usePersistentState, View } from 'embedded-react';
import { check, report } from './harness.js';

// In-memory __erPersist (what the C host provides in the simulator).
const store = new Map();
globalThis.__erPersist = { get: (k) => store.get(k), set: (k, v) => store.set(k, v) };

let lastValue;
let setter;
function Comp() {
  const [v, setV] = usePersistentState('counter', 10);
  lastValue = v;
  setter = setV;
  return <View style={{ width: 1, height: 1 }} />;
}

const root = createRoot({ width: screen.width, height: screen.height });

root.render(<Comp />);
NativeUI.tick(0);
check(lastValue === 10, `initial uses the default when nothing is persisted (got ${lastValue})`);

setter(42);
NativeUI.tick(0);
check(lastValue === 42, `setter updates the value (got ${lastValue})`);
check(store.get('counter') === '42', `value persisted as JSON (got ${store.get('counter')})`);

// "Reload": unmount + remount — usePersistentState should restore the persisted 42, not the default.
root.render(null);
NativeUI.tick(0);
root.render(<Comp />);
NativeUI.tick(0);
check(lastValue === 42, `restored persisted value after remount (got ${lastValue})`);

// Functional updates work and persist too.
setter((n) => n + 1);
NativeUI.tick(0);
check(lastValue === 43 && store.get('counter') === '43', `functional update persists (got ${lastValue})`);

// Without a __erPersist host (a device), it degrades to plain useState (the default).
delete globalThis.__erPersist;
root.render(null);
NativeUI.tick(0);
root.render(<Comp />);
NativeUI.tick(0);
check(lastValue === 10, `degrades to useState default when no persist host (got ${lastValue})`);

report('persistent-state');
