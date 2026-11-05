---
title: 'Contributing'
description: 'The engine invariants, documentation conventions and code style that apply across the repository.'
---

Contributions are welcome on any layer: the engine, a backend, the Flow A bridge, the Flow B
compiler, the tooling, these docs. This page is the project-wide rules; the rest of the Internals
section has the layer-specific ones, and the [roadmap](/roadmap) has what is planned and what is
known to be broken.

## Engine invariants

Three rules are not negotiable, because they are what keeps the engine portable to any MCU:

- **No platform headers.** Pure C99. No `stm32h7xx_hal.h`, no `esp_lcd.h`, no `<windows.h>`. The
  engine uses only the C standard library headers. Hardware
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
cmake --build build --target format-check  # dry run: --Werror, no edits
```

**JavaScript, TypeScript and this site's Markdown** are formatted by Prettier with the root
`.prettierrc.cjs`. The repository root has its own `package.json` that pins the Prettier version,
so install it once there and run it from there:

```bash
npm install            # once, at the repository root
npm run format         # rewrite in place
npm run format:check   # dry-run
```

Markdown keeps its line breaks as written (Prettier's default `proseWrap: "preserve"`), and only
the tables are aligned. `changelog.md` and `roadmap.md` are copies synced from the root files, so
they are left out.

Neither formatter runs in CI, so run them before you push.

**TypeScript.** The Flow B compiler (`aot/compile.mts` and `aot/compiler/`) is TypeScript: `.mts`
files, checked strictly by `npm run typecheck`. Node runs them as they are by stripping their types,
so running the compiler or the JS tests from source needs Node 22.18 or later, and only erasable
syntax is allowed: no `enum`, `namespace` or parameter properties, a relative import names the real
file (`./switch.mts`), and a type-only import is written `import type`. The published package ships
JavaScript: `tools/stage-npm-package.mjs` compiles the `.mts` files when a release packs it.

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
- **A change that affects a board** should be checked on that board; the [board guides](../guides/index.md#boards)
  say how to build and flash each. Hardware results belong in the pull request, with the board named.

## The pull request

`master` is protected: every change lands through a squash-merged pull request with a review and
the CI checks green (engine, bridge, JS tests and version drift, Flow B golden images, fuzzing, consumer smoke).
Keep a pull request to one concern. Describe what changed and why in a few sentences; the diff
shows the how.
