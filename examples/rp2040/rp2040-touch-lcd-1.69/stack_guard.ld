/* Added to the pico-sdk's linker script: main.c makes the top __stack_guard_bytes of main RAM, just
   below the stack in SCRATCH_X, a no-access MPU guard, so no static data may be placed there. */
ASSERT(__end__ <= __StackOneBottom - __stack_guard_bytes,
       "static RAM (.data + .bss) reaches into the stack guard: shrink the ERUI_* pools in CMakeLists.txt")

/* The heap runs from the end of static data, rounded up to 8 bytes as main.c's _sbrk starts it, up to the guard,
   and the framebuffer is malloc'd from it at boot with malloc's own room on top. A lower bound: it cannot see what
   else is malloc'd before the framebuffer. */
ASSERT(ALIGN(__end__, 8) + __framebuffer_bytes + __heap_reserve_bytes <= __StackOneBottom - __stack_guard_bytes,
       "the framebuffer doesn't fit beside static RAM: shrink the ERUI_* pools or ERB_ARENA_BYTES")

/* The package slots start above the firmware, so the firmware must end below them. */
ASSERT(__flash_binary_end <= ORIGIN(FLASH) + __package_slots_offset,
       "the firmware reaches into the package slots: move them in CMakeLists.txt and flash.mts")
