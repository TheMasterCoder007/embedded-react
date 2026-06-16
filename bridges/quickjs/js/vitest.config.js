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

import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    // Unit tests are co-located next to the code they cover, in __tests__ folders: the library
    // surface under src/, the build-time asset bakers under assets/, and the Flow B AOT compiler
    // under aot/ (JSX → C; tests assert on the generated C string).
    include: [
      'src/**/__tests__/**/*.unit.test.{js,jsx}',
      'assets/**/__tests__/**/*.unit.test.{js,jsx}',
      'aot/**/__tests__/**/*.unit.test.{js,mjs}',
      '__tests__/**/*.unit.test.{js,mjs}', // package-root dev tooling (sim-server / persist transform)
    ],
    environment: 'node',
  },
  // Let .jsx unit tests use the automatic JSX runtime (same as the bundle build). vitest 3 transforms
  // with esbuild, which defaults to the classic runtime, so this must be set explicitly.
  esbuild: { jsx: 'automatic' },
});
