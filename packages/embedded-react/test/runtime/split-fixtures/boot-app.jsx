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

// Boot app for the split round-trip test. Seeds the persisted store, then reads it back through
// usePersistentState and publishes (build, n) on globalThis.__probe for the C harness to read. The seed
// lets the soft reload (reloaded-app) prove the store survived an app-only swap.
import {AppRegistry, View, usePersistentState} from 'embedded-react';

globalThis.__erPersist.set('n', '41');

function App() {
  const [n] = usePersistentState('n', 0);
  globalThis.__probe = {build: 'boot', n};
  return <View />;
}

AppRegistry.registerComponent('split-fixture', () => App);
