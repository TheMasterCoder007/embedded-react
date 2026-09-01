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

// Entry point for the PanResponder variant (Flow A only) — see App.pan.jsx. The stock entry is
// index.jsx, which registers the AOT-compatible App.jsx that the RP2040 example compiles to C.
import {AppRegistry} from 'embedded-react';
import {App} from './App.pan.jsx';

AppRegistry.registerComponent('demo', () => App);
console.log('React mounted at', screen.width + 'x' + screen.height);
