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

// The "edited" app for the split round-trip test — loaded as an app-only frame (no vendor) with NO reset.
// It must mount against the resident vendor (proving require() still resolves) and read the persisted 'n'
// it never set itself (proving the store survived the soft reload). Distinct build tag so the harness can
// confirm the NEW code actually ran.
import {AppRegistry, View, usePersistentState} from 'embedded-react';

function App() {
  const [n] = usePersistentState('n', 0);
  globalThis.__probe = {build: 'reloaded', n};
  return <View />;
}

AppRegistry.registerComponent('split-fixture', () => App);
