# bridges

Frontends that drive the engine. The engine has no opinion about who calls `er_scene.h`; each bridge
is one way for a runtime, language or build system to map developer-facing concepts onto the
engine's C API.

| Bridge | What it is | Status |
|---|---|---|
| `quickjs/` | The reference frontend: a React reconciler hosted in QuickJS (Flow A), the `er_runtime` host core, and, in `quickjs/js/aot/`, the Flow B JSX-to-C compiler. | Working, on desktop and on hardware |

The layering deliberately leaves room for other frontends (a Lua UI, a JSON tree loader, a visual
editor's output); none is on the roadmap. See
[Architecture](https://embedded-react.dev/internals/architecture).
