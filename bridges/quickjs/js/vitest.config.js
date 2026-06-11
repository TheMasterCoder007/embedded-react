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
    ],
    environment: 'node',
  },
  // Let .jsx unit tests use the automatic JSX runtime (same as the bundle build).
  esbuild: { jsx: 'automatic' },
});
