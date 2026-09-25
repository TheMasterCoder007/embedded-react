---
title: 'Contributing'
description: 'The engine invariants, documentation conventions and code style that apply across the repository.'
---

Contributions are welcome on any layer: the engine, a backend, the Flow A bridge, the Flow B
compiler, the tooling, these docs. This page is the project-wide rules; each top-level folder's
README has the layer-specific ones, and the [roadmap](/roadmap) has what is planned and what is
known to be broken.

## Engine invariants

Three rules are not negotiable, because they are what keeps the engine portable to any MCU:

- **No platform headers.** Pure C99. No `stm32h7xx_hal.h`, no `esp_lcd.h`, no `<windows.h>`. The
  engine needs `<stdint.h>`, `<string.h>`, `<stdlib.h>` and `<math.h>` and nothing else. Hardware
  specifics live in `backends/`.
- **No React, or any frontend, in the engine.** It does not import React and makes no assumptions
  about who calls `er_scene.h`. Bindings to React, or to anything else, live in `bridges/`. That
  neutrality is what makes the two-flow design work.
- **No heap during rendering.** Every scratch buffer is static, sized at compile time through the
  `ERUI_*` flags. A render pass never allocates.

A change that needs to break one of these is a design conversation first, in an issue.

## Code style

**C** is formatted by clang-format with the repository's `.clang-format`. The root `CMakeLists.txt`
exposes two targets; run the first before committing:

```bash
cmake -S . -B build
cmake --build build --target format        # rewrite in place
cmake --build build --target format-check  # the CI dry-run: --Werror, no edits
```

**JavaScript** is formatted by Prettier with the root `.prettierrc.cjs`. Prettier is a dev tool of
the npm package, not a dependency of it or of the scaffolded template, so run it from there over
the whole repository:

```bash
cd bridges/quickjs/js
npm run format         # rewrite in place
npm run format:check   # dry-run
```

A bare `npx prettier` from the repository root resolves a different Prettier and flags unrelated
files; use the package's.

**Markdown** for this site is also formatted by that Prettier, with `--prose-wrap preserve`, which
is what keeps the tables aligned.

## Documentation conventions

C files are broken into sections with a banner:

```c
/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/
```

Every function, static or public, carries a description, its parameters and its return value:

```c
/**
 * @brief What the function does.
 *
 * @param[in] param1  What it is.
 * @param[in] param2  What it is.
 *
 * @return What comes back.
 */
```

Public APIs are documented in the header; static functions in the `.c` file that implements them.
The header comments are the reference the [C engine](../api/c-engine.md) page points at, so they
are worth keeping exact.

Comments say what the code does, not its history. A comment that cites an issue number or a review
round goes stale the moment the code moves; the git log has the history.

## Changes that need more than code

- **Anything user-facing gets a changelog bullet** under `## [Unreleased]`, short and high level, 
  before the change is finished. See [Releasing](./releasing.md).
- **Anything that changes a version-bearing file** (a manifest, an install pin) must go through
  `sync-version.mjs`; CI checks for drift.
- **A new prop or event** needs the bridge's tables, the package's type declarations and the AOT
  compiler to agree; `npm test` has a parity test over the tables, and the AOT either lowers the
  prop or rejects it by name.
- **A new engine feature** needs a CTest case, and if it adds a compile-time flag, a CI pass with
  the flag on. See [Testing](./testing.md).
- **A change that affects a board** should be checked on that board; the examples' READMEs say how
  to build and flash each. Hardware results belong in the pull request, with the board named.

## The pull request

`master` is protected: every change lands through a squash-merged pull request with a review and
the CI checks green (engine, bridge, JS tests and version drift, AOT compile smoke, consumer smoke).
Keep a pull request to one concern. Describe what changed and why in a few sentences; the diff
shows the how.
