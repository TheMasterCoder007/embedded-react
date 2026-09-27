# Contributing

Contributions are welcome on any layer: the C engine, a backend, the Flow A QuickJS bridge, the
Flow B compiler, the tooling, the docs. The full rules live on the site:
**[embedded-react.dev/internals/contributing](https://embedded-react.dev/internals/contributing)**,
alongside [Testing](https://embedded-react.dev/internals/testing) and
[Releasing](https://embedded-react.dev/internals/releasing). What is planned and known-broken is in
[`ROADMAP.md`](ROADMAP.md).

The short version:

- **Engine invariants.** No platform headers (pure C99; hardware specifics live in `backends/`), no
  React or any frontend in the engine (bindings live in `bridges/`), and no heap during rendering
  (every scratch buffer is static, sized by the `ERUI_*` flags).
- **Code style.** C is formatted with clang-format (`cmake -S . -B build && cmake --build build
  --target format`); JavaScript and the site's Markdown with Prettier, run from the npm package
  (`cd bridges/quickjs/js && npm run format`). Every function carries a description, its parameters
  and its return value; comments say what the code does, not its history.
- **Changes that need more than code.** A user-facing change gets a bullet under `## [Unreleased]`
  in `CHANGELOG.md`; a version-bearing file goes through `tools/sync-version.mjs`; a new prop needs
  the bridge tables, the type declarations and the AOT compiler to agree; a new engine feature needs
  a CTest case; a change that affects a board is checked on that board and says so in the pull
  request.
- **The pull request.** `master` is protected: one concern per PR, a review, and the CI checks green.
