# bridges

Frontends that drive the engine. The engine has no opinion about who calls into
`er_scene.h` — each bridge is one way for a particular runtime, language, or build
system to map developer-facing concepts (React components, Lua tables, JSON descriptors,
GUI editor output) into the engine's pure C API.

| Bridge | Description | Status |
|---|---|---|
| `quickjs/` | Reference QuickJS adapter that hosts a React reconciler. The primary supported developer path. | Stub (Flow A milestone) |

## Planned

The engine is intentionally bridge-agnostic to leave room for these later:

- **Lua UI** — drive the scene graph from Lua tables / a Lua DSL.
- **JSON UI** — a serialized React-tree format loaded at runtime; pairs well with the
  visual editor below.
- **Visual editor runtime** — a desktop GUI designer that emits the JSON format.
- **Compiled mode** — Flow B in `PLAN.md`. JSX → C source AOT compiler emitting calls
  to `er_scene.h` at build time. No JS runtime on device.
- **Scripting APIs** — language-agnostic C bindings that other runtimes can wrap.

None of these are roadmap items today. They're listed so contributors know the engine
keeps the door open and doesn't bake any single frontend's assumptions into the
scene-graph layer.
