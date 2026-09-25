---
title: 'Internals'
description: 'How the engine, bridges and tooling fit together, for contributors.'
---

These pages are for people changing Embedded React rather than using it: how the repository is
laid out, what a backend has to get right, how the tests are tiered, how a release is cut, and the
rules that keep the engine portable.

- [Architecture](./architecture.md): the monorepo's layers and the one boundary that matters, the
  C ABI between everything and the engine.
- [Writing a backend](./writing-a-backend.md): the five callbacks, the optional extensions, and the
  conventions learned from the four backends that run on hardware.
- [Testing](./testing.md): engine CTest suites, the bridge's unit, runtime and bytecode tiers, the
  AOT smoke tests, the parity harness, and what CI runs.
- [Releasing](./releasing.md): one lockstep version, `release.mjs`, the tag-gated publish workflow,
  and how the docs site follows a release.
- [Contributing](./contributing.md): the engine invariants, documentation conventions and code
  style.

Each top-level folder in the repository also has a README with layer-specific detail
(`engine/`, `backends/`, `bridges/`, `bridges/quickjs/js/`); these pages are the map, the READMEs
are the territory.
