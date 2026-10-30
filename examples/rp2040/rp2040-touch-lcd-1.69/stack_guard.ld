/* Added to the pico-sdk's linker script: main.c makes the top __stack_guard_bytes of main RAM, just
   below the stack in SCRATCH_X, a no-access MPU guard, so no static data may be placed there. */
ASSERT(__end__ <= __StackOneBottom - __stack_guard_bytes,
       "static RAM (.data + .bss) reaches into the stack guard: shrink the ERUI_* pools in CMakeLists.txt")

/* The heap runs from the end of static data up to the guard, and the framebuffer is malloc'd from it at boot. */
ASSERT(__end__ + __framebuffer_bytes <= __StackOneBottom - __stack_guard_bytes,
       "the framebuffer doesn't fit beside static RAM: shrink the ERUI_* pools or ERB_ARENA_BYTES")

/* The package slots start above the firmware, so the firmware must end below them. */
ASSERT(__flash_binary_end <= ORIGIN(FLASH) + __package_slots_offset,
       "the firmware reaches into the package slots: move them in CMakeLists.txt and flash.mjs")
