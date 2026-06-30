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

// Dev-only Fast Refresh runtime wiring, bundled FIRST into the vendor chunk when the app is built with the
// react-refresh transform. It must run before the embedded-react reconciler loads: it installs
// react-refresh/runtime's global hook (so renderer.js's injectIntoDevTools, gated on $RefreshReg$,
// registers our roots with it) and the $RefreshReg$/$RefreshSig$ globals the transformed app code calls.
//
// __er_performRefresh is invoked at the end of every app-chunk evaluation (a footer the build appends): on
// the first boot it's a no-op; on a hot-reload re-evaluation it swaps the changed components in place with
// their state preserved. Never included in a production build — this module is added only in refresh mode.
import * as RefreshRuntime from 'react-refresh/runtime';

RefreshRuntime.injectIntoGlobalHook(globalThis);
globalThis.$RefreshReg$ = (type, id) => RefreshRuntime.register(type, id);
globalThis.$RefreshSig$ = () =>
  RefreshRuntime.createSignatureFunctionForTransform();
globalThis.__er_performRefresh = () => RefreshRuntime.performReactRefresh();
