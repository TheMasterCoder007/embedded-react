import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    // Unit tests are co-located next to the code they cover, in __tests__ folders.
    include: ['src/**/__tests__/**/*.unit.test.{js,jsx}'],
    environment: 'node',
  },
  // Let .jsx unit tests use the automatic JSX runtime (same as the bundle build).
  esbuild: { jsx: 'automatic' },
});
