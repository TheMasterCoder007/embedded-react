# bridges

Frontends that drive the engine. The engine has no opinion about who calls `er_scene.h`; each bridge
is one way for a runtime, language or build system to map developer-facing concepts onto the
engine's C API.

| Bridge | What it is | Status |
|---|---|---|
| `quickjs/` | The reference frontend for Flow A: the NativeUI bridge and `er_runtime` host core over QuickJS. The React reconciler it hosts is in `packages/embedded-react/`. | Working, on desktop and on hardware |

The layering deliberately leaves room for other frontends (a Lua UI, a JSON tree loader, a visual
editor's output); none is on the roadmap. See
[Architecture](https://embedded-react.dev/internals/architecture).
