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

import { AppRegistry } from 'embedded-react';
import { App } from './App.jsx';

// Flow A entry point (QuickJS): mounts App into a screen-sized root. Flow B (AOT) compiles App.jsx
// directly and provides its own entry, so it doesn't use this file.
AppRegistry.registerComponent('music-player', () => App);
