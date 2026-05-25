# engine/platform

Future home for engine-side platform-abstraction hooks — things like a configurable time
source (so the engine can be driven by something other than `embedded_renderer_tick`'s
delta_ms argument), memory allocators if we ever need them, or weak symbols for
RTOS-aware mutexes.

Empty today. The engine has no platform abstractions yet; everything platform-specific
is pushed out to `backends/`.
