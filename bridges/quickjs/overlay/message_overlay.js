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

// Message-overlay app: builds a full-screen red panel from the `__title` / `__error` / `__hint`
// globals (RN redbox). Used for both JS errors and config-load failures, so a device shows the same
// screen the desktop does. `__hint` is optional (empty → no hint line).
//
// Ships as PRECOMPILED BYTECODE (message_overlay.qbc.c) so it also works on parser-less
// (QJS_DISABLE_PARSER) device builds — er_runtime_show_message runs the bytecode, never this source.
// After editing this file (or bumping the pinned QuickJS, whose bytecode format it must match),
// regenerate with:
//
//   cmake --build bridges/quickjs/build --target er-bridge-quickjs-compile
//   ./bridges/quickjs/build/er-bridge-quickjs-compile bridges/quickjs/overlay/message_overlay.js \
//       bridges/quickjs/overlay/message_overlay.qbc.c er_overlay_qbc
//
// The IIFE keeps bindings out of the global scope so the overlay can run more than once per context.
(() => {
  const W = screen.width, H = screen.height;
  const title = (typeof __title === 'string' && __title) ? __title : 'Error';
  const msg = (typeof __error === 'string' && __error) ? __error : 'Unknown error';
  const hint = (typeof __hint === 'string') ? __hint : '';
  const root = NativeUI.createNode('View');
  NativeUI.setProps(root, { width: W, height: H, backgroundColor: '#7a0b0b',
                            flexDirection: 'column', padding: 24, gap: 12 });
  const t = NativeUI.createNode('Text');
  NativeUI.setProps(t, { text: title, color: '#ffffff', fontSize: 24, fontWeight: 'bold' });
  NativeUI.appendChild(root, t);
  const body = NativeUI.createNode('Text');
  NativeUI.setProps(body, { text: msg, color: '#ffd9d9', fontSize: 14, width: W - 48, numberOfLines: 20 });
  NativeUI.appendChild(root, body);
  if (hint) {
    const h = NativeUI.createNode('Text');
    NativeUI.setProps(h, { text: hint, color: '#ff9b9b', fontSize: 12 });
    NativeUI.appendChild(root, h);
  }
  NativeUI.setRoot(root);
  NativeUI.commit();
})();
