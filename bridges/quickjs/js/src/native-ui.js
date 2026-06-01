// The QuickJS C bridge (native_ui_bridge.c) installs a global `NativeUI` object before the
// bundle runs. Re-export it so the rest of the JS imports it cleanly instead of touching the
// global directly.
export const NativeUI = globalThis.NativeUI;

if (!NativeUI) {
  throw new Error('NativeUI global is missing — the QuickJS bridge must be installed before the bundle runs.');
}
