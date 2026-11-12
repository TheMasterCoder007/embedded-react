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

import {existsSync} from 'node:fs';
import {resolve} from 'node:path';

/**
 * A demo's file, in TypeScript or JavaScript: `demoFile(directory, 'App')` is App.tsx when there is one, else
 * App.jsx.
 *
 * @param directory  The demo's folder.
 * @param name  The file's name, without its extension.
 *
 * @returns The path, which is the .jsx one when neither is there.
 */
export function demoFile(directory: string, name: string): string {
  const typed = resolve(directory, `${name}.tsx`);
  return existsSync(typed) ? typed : resolve(directory, `${name}.jsx`);
}
