/* Added to the pico-sdk's linker script: main.c makes the top __stack_guard_bytes of main RAM, just
   below the stack in SCRATCH_X, a no-access MPU guard, so no static data may be placed there. */
ASSERT(__end__ <= __StackOneBottom - __stack_guard_bytes,
       "static RAM (.data + .bss) reaches into the stack guard: shrink the ERUI_* pools in CMakeLists.txt")
