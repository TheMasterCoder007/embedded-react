---
title: 'Releasing'
description: 'One lockstep version, release.mjs, the tag-gated publish workflow, and how the docs site follows a release.'
---

Every artifact ships at one version: the npm package and the scaffolder, the engine source bundle
on GitHub, the ESP-IDF component, the PlatformIO library, and the version pins in the READMEs and
this site. The single source of truth is the repo-root `VERSION` file, and `tools/sync-version.mjs`
propagates it everywhere it needs to appear.

## Cutting a release

Day to day, add notes under `## [Unreleased]` in `CHANGELOG.md` as changes land. A release then is:

```bash
node tools/release.mjs 0.15.0            # on master, with a clean tree
git push --follow-tags
```

`release.mjs` does five things, in order: sets `VERSION`; runs `sync-version.mjs --set`, which
writes the version into every manifest and pin (twenty files at the time of writing); stamps the
changelog, promoting `## [Unreleased]` to `## [0.15.0] - <date>` with a compare link; commits
exactly those files as `release: v0.15.0`, never sweeping in unrelated changes; and tags `v0.15.0`.
`--dry-run` prints the plan and changes nothing. It refuses to run off `master` unless told
otherwise, because a tag cut on a feature branch would publish off-branch code.

What `sync-version` keeps in step:

| File                                                         | What carries the version                                                                                           |
| ------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------ |
| `bridges/quickjs/js/package.json`, `package-lock.json`       | The `embedded-react` package                                                                                       |
| `create-embedded-react/package.json`                         | The scaffolder                                                                                                     |
| `demos/*/package.json`                                       | Each demo's `embedded-react` dependency (the scaffolder's templates carry a placeholder it fills at scaffold time) |
| `library.json`                                               | PlatformIO                                                                                                         |
| `engine/idf_component.yml`                                   | The ESP-IDF component                                                                                              |
| `engine/include/er_version.h`                                | The engine's own version, which the AOT compiler `_Static_assert`s against                                         |
| `README.md`, `website/docs/getting-started/installation.mdx` | The CMake, ESP-IDF and PlatformIO install pins                                                                     |
| `website/package.json`                                       | The engine the playground runs                                                                                     |
| `examples/esp32/*/CMakeLists.txt`                            | The `FetchContent` tag a copied-out example fetches                                                                |

`node tools/sync-version.mjs --check` fails if any of them drifts, and CI runs it on every push,
so a hand-edited version cannot land. A missing file is skipped, so a renamed manifest silently
drops out of the sync: check the list when you move one.

## The publish workflow

Pushing the tag runs `.github/workflows/release.yml`, which gates twice before publishing anything:
the tag must equal the committed `VERSION`, and the tagged commit must be on `master`. Then, channel
by channel:

1. **npm.** `embedded-react` and `create-embedded-react`, through OIDC trusted publishing with
   provenance, no token. Each publish is idempotent: if that version is already on the registry the
   step skips, so a re-run after a transient failure never dies on "cannot publish over existing
   version". The simulator wasm is built with Emscripten in the same job and staged into the package
   before publishing.
2. **ESP-IDF Component Registry**, from `engine/`, when `IDF_COMPONENT_API_TOKEN` is set.
3. **PlatformIO**, from the root `library.json`, when `PLATFORMIO_AUTH_TOKEN` is set.
4. **GitHub Release**, with generated notes and an engine source tarball (`engine`, `backends`,
   `LICENSE`, `NOTICE`, `README.md`) for the CMake `FetchContent` channel.

Each channel is gated on its own variable or secret, so they can be enabled one at a time. First-time
setup for npm is the trusted publisher on npmjs.com, pointed at this repository and workflow, with
the owner name in the exact case GitHub reports; a case mismatch makes the token exchange fail
silently and npm fall back to asking for a token.

## The docs site

The site deploys from `master` on any push that touches `website/`, the changelog, the roadmap or
the starter template, so documentation goes live when it merges, not when a release is cut. The one
thing that ties it to releases is the playground, which runs the published `embedded-react` at the
version `website/package.json` pins. `release.mjs` bumps that pin in the release commit; the docs
workflow then waits for that version to appear on npm (the publish runs in parallel from the same
push) before installing and building. The lockfile cannot carry the hash of an unpublished tarball,
which is why the site's CI uses `npm install` rather than `npm ci`, and why the lockfile catches up
on the next local install.

## Version compatibility

Three things must agree, and each is checked rather than trusted:

- **The container and the runtime.** An `app.erpkg` is stamped with the QuickJS version it was
  compiled for; the loader rejects a mismatch with an on-screen panel rather than running garbage.
  The tag is pinned in the examples' `CMakeLists.txt` and in the bridge; bump them together.
- **Generated C and the engine.** The AOT compiler writes its version into `app.gen.c` and
  `_Static_assert`s it against `er_version.h`, so a stale `app.gen.c` fails to compile.
- **Bytecode and the QuickJS build.** Bytecode is specific to the QuickJS version, which is why the
  package ships its own compiler through the prebuilt wasm and the firmware fetches a pinned tag.

## Changelog conventions

One bullet per user-facing change, under `### Added`, `### Changed` or `### Fixed`. The
`[Unreleased]` section is what the release stamps, so a change without a bullet there is invisible
in the release notes.
