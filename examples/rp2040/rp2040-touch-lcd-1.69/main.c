/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * embedded-react — Waveshare RP2040-Touch-LCD-1.69 host (Flow B).
 *
 * The smallest board in the examples family: an RP2040 (264 KB SRAM, no FPU, no PSRAM) driving a
 * 1.69" 240x280 ST7789V2 panel with CST816S capacitive touch. Like the CYD example, this has NO QuickJS
 * and no JS at runtime. It brings up the panel + touch (board.c), registers the pico-spi-lcd render
 * backend, loads the active package from its flash slot, and runs the frame loop: poll touch, commit,
 * present, tick. The package is written separately from the firmware (`embedded-react flash`), so changing
 * the UI needs no firmware rebuild.
 *
 * See ./README.md.
 */

#include "board.h"
#include "er_error_screen.h"
#include "er_scene.h"
#include "erb_package_slots.h"
#include "erb_runtime.h"
#include "native_modules.h"
#include "native_renderer.h"
#include "pedometer.h"
#include "pico_spi_lcd_backend.h"

#if ER_HOTRELOAD
#include "erb_package_reload.h"
#endif

#include "hardware/flash.h"
#include "hardware/structs/mpu.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** @brief The app's mutable state. A package states what it needs, and the watch face takes about 0.5 KB. */
#ifndef ERB_ARENA_BYTES
#define ERB_ARENA_BYTES (4 * 1024)
#endif

static uint32_t s_arena[ERB_ARENA_BYTES / sizeof(uint32_t)];

/** @brief Room for one line of erb_error_message(). */
#define ERROR_MESSAGE_BYTES 160

/* USB serial connects after the boot is over, so what happened is kept and said once a host is listening. */
#define BOOT_REPORT_BYTES 320
static char s_boot_report[BOOT_REPORT_BYTES];

/** @brief Frames to wait once a host connects: a serial monitor flushes what arrives as it opens the port. */
#define BOOT_REPORT_CONNECTED_FRAMES 30U

/**
 * @brief Adds a line to the boot report.
 *
 * @param[in] format  As for printf; its arguments follow.
 */
static void __attribute__((format(printf, 1, 2))) boot_report(const char* format, ...)
{
    const size_t used = strlen(s_boot_report);
    va_list args;
    va_start(args, format);
    vsnprintf(s_boot_report + used, sizeof(s_boot_report) - used, format, args);
    va_end(args);
}

/** @brief Target frame period for the adaptive pacer (~60 fps); heavy frames just run as fast as work allows.
 *  Also sets the touch sample rate (one poll per frame), so a smaller value = finer drag tracking. */
#define ER_TARGET_FRAME_MS 16U

/** @brief Returns milliseconds since boot (engine-clock delta source). */
static uint32_t now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

/** @brief Bring-up debug aids: 1 = wait briefly for a USB host + heartbeat log; 2 = also hold a
 *  color-bar test pattern for 3 s at boot (panel-driver proof). Set to 0 for release builds. */
#ifndef ER_BOARD_DEBUG
#define ER_BOARD_DEBUG 0
#endif

/** @brief Watchdog timeout. A frame takes tens of ms, so only a hang (or a stack-guard lockup) reaches it. */
#define ER_WATCHDOG_MS 1000U

/** @brief Watchdog timeout while booting: checking a full slot's CRC from flash, twice on a rollback, takes longer
 *  than a frame. */
#define ER_WATCHDOG_BOOT_MS 8000U

/** @brief Backlight once the first frame is up, in percent. */
#define BACKLIGHT_PERCENT 90

#define MS_PER_SECOND 1000U

/** @brief Debug builds wait this long for a USB host, in steps, and hold the test pattern this long. */
#define USB_WAIT_ATTEMPTS 30
#define USB_WAIT_STEP_MS 100
#define TEST_PATTERN_HOLD_MS 3000

/** @brief Bubble level: dot travel in px, the tilt that reaches it (~0.5 g in accel LSB), the low-pass weight, and
 *  the update period. */
#define LEVEL_RADIUS 74
#define LEVEL_FULL_TILT 4096.0f
#define LEVEL_SMOOTHING 0.15f
#define LEVEL_PERIOD_MS 33u

/* A forced failure, to check the error screen on hardware: `-DER_BOARD_CRASH_TEST=1` hard faults 5 s after boot, and
 * =2 hangs. Each shows the error screen after the reset, naming it, then the app comes back and it happens again. */
#ifdef ER_BOARD_CRASH_TEST
#define CRASH_TEST_AFTER_MS 5000U
#define CRASH_TEST_HARD_FAULT 1
#define CRASH_TEST_HANG 2

/** @brief Fails the way ER_BOARD_CRASH_TEST asks, once the time has come. */
static void crash_test(uint32_t since_boot_ms)
{
    if (since_boot_ms < CRASH_TEST_AFTER_MS)
    {
        return;
    }
#if ER_BOARD_CRASH_TEST == CRASH_TEST_HARD_FAULT
    __asm volatile("udf #0");
#elif ER_BOARD_CRASH_TEST == CRASH_TEST_HANG
    for (;;)
    {
    }
#endif
}
#endif

/** @brief Longest a fault from before the reset stays on screen at boot; a tap ends it sooner. */
#define FAULT_HOLD_MS 10000U

/** @brief The stacked exception frame's pc, in words. */
#define EXCEPTION_FRAME_PC 6

/** @brief A native fault, kept in RAM the boot leaves alone so it survives the reset it causes. */
static ERFaultRecord __uninitialized_ram(s_fault);

/** @brief The hard fault handler's own stack, so recording a fault never needs the stack that may have overflowed. */
#define FAULT_STACK_BYTES 256U
static uint8_t __attribute__((aligned(8))) s_fault_stack[FAULT_STACK_BYTES];
static uint8_t* const __attribute__((used)) s_fault_stack_top = s_fault_stack + FAULT_STACK_BYTES;

/* Core 0's stack is 8 KB: SCRATCH_Y (PICO_STACK_SIZE, see CMakeLists.txt) plus this lower half in
 * SCRATCH_X. The watch face peaks at ~3.1 KB; the rest is headroom for deeper apps. This
 * claims SCRATCH_X the same way the SDK claims it for core 1's stack, so starting core 1 (or placing
 * anything else there) fails to link instead of sharing the memory. */
static uint32_t __attribute__((section(".stack1"))) s_stack_low[4096 / sizeof(uint32_t)];
extern uint32_t __StackTop[];

/* The top STACK_GUARD_BYTES of main RAM (2 KB, set in CMakeLists.txt), just below the stack, is a
 * no-access guard. It is bigger than any engine stack frame (the largest, the arc sector fill, is ~0.8 KB), so an
 * overflow cannot step over it into the heap. */
_Static_assert(STACK_GUARD_BYTES >= 256U && (STACK_GUARD_BYTES & (STACK_GUARD_BYTES - 1U)) == 0U,
               "the MPU guard must be a power of two of at least 256 bytes");
#define STACK_GUARD_MPU_SIZE ((uint32_t)__builtin_ctz(STACK_GUARD_BYTES) - 1U) /* region is 2^(SIZE+1) bytes */
#define MPU_RASR_XN_BITS (1U << 28)

/** @brief Turns on the MPU guard below the stack; a stack overflow then faults instead of writing the heap. */
static void stack_guard_install(void)
{
    mpu_hw->rbar = ((uint32_t)s_stack_low - STACK_GUARD_BYTES) | M0PLUS_MPU_RBAR_VALID_BITS;
    mpu_hw->rasr = M0PLUS_MPU_RASR_ENABLE_BITS | (STACK_GUARD_MPU_SIZE << M0PLUS_MPU_RASR_SIZE_LSB) | MPU_RASR_XN_BITS;
    mpu_hw->ctrl = M0PLUS_MPU_CTRL_PRIVDEFENA_BITS | M0PLUS_MPU_CTRL_ENABLE_BITS;
}

/** @brief malloc's heap: grows from the end of .bss up to the stack guard, and fails there instead of
 *  faulting in it. Replaces the SDK's weak _sbrk, which stops at the stack itself. */
void* _sbrk(int increment)
{
    extern char end;
    static char* heap_end = &end;
    char* const limit = (char*)s_stack_low - STACK_GUARD_BYTES;
    if (increment > limit - heap_end)
    {
        return (void*)-1;
    }
    char* const previous_end = heap_end;
    heap_end += increment;
    return previous_end;
}

/**
 * @brief Records a hard fault with the pc it happened at, then resets through the watchdog.
 *
 * @param[in] exception_frame  The registers the core stacked on the fault.
 */
void __attribute__((used, noreturn)) hard_fault_record(const uint32_t* exception_frame)
{
    er_fault_record(&s_fault, "hard fault", exception_frame[EXCEPTION_FRAME_PC], 0u);
    watchdog_reboot(0u, 0u, 0u);
    for (;;)
    {
    }
}

/**
 * @brief The hard fault vector: finds the stacked frame on the stack in use, moves to s_fault_stack, and hands
 *        the frame on. A stack overflow that faults while the core stacks the frame locks up instead; the
 *        watchdog resets it and the boot records that.
 */
void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile("movs r0, #4\n"
                   "mov r1, lr\n"
                   "tst r0, r1\n"
                   "beq 1f\n"
                   "mrs r0, psp\n"
                   "b 2f\n"
                   "1: mrs r0, msp\n"
                   "2: ldr r1, =s_fault_stack_top\n"
                   "ldr r1, [r1]\n"
                   "msr msp, r1\n"
                   "ldr r1, =hard_fault_record\n"
                   "bx r1\n"
                   ".ltorg\n");
}

/**
 * @brief Keeps the error screen up until the panel is tapped or FAULT_HOLD_MS passes, then takes it down.
 *
 * @param[in] touch  Whether touch came up, so a tap can end it.
 */
static void hold_error_screen(bool touch)
{
    board_backlight(BACKLIGHT_PERCENT);
    for (uint32_t waited = 0; waited < FAULT_HOLD_MS; waited += ER_TARGET_FRAME_MS)
    {
        watchdog_update();
        er_commit();
        er_pico_spi_lcd_present();
        int touch_x = 0;
        int touch_y = 0;
        bool pressed = false;
        if (touch && board_touch_read(&touch_x, &touch_y, &pressed) && pressed)
        {
            break;
        }
        sleep_ms(ER_TARGET_FRAME_MS);
    }
    er_error_screen_hide();
}

/**
 * @brief Shows a fault from before the reset, if there was one, for as long as hold_error_screen() does.
 *
 * @param[in] touch  Whether touch came up, so a tap can end it.
 */
static void show_recorded_fault(bool touch)
{
    if (!er_fault_recorded(&s_fault) && watchdog_enable_caused_reboot())
    {
        er_fault_record(&s_fault, "watchdog reset: a hang, or a stack overflow", 0u, 0u);
    }
    if (!er_fault_recorded(&s_fault))
    {
        return;
    }
    boot_report("restarted after a fault: %s (pc 0x%08lx)\n", s_fault.reason, (unsigned long)s_fault.program_counter);
    er_fault_show_recorded(&s_fault, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    hold_error_screen(touch);
}

/**
 * @brief Programs a slot's header page to zeros, which takes no erase, so it no longer reads as valid.
 *
 * @param[in] slot     The slot.
 * @param[in] context  Unused.
 */
static void invalidate_slot(unsigned slot, void* context)
{
    (void)context;
    uint8_t zeros[FLASH_PAGE_SIZE]; /* in RAM: flash can't be read while it is programmed */
    memset(zeros, 0, sizeof(zeros));
    const uint32_t offset = slot == 0u ? ER_PACKAGE_SLOT_A_OFFSET : ER_PACKAGE_SLOT_B_OFFSET;
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_program(offset, zeros, sizeof(zeros));
    restore_interrupts(interrupts);
}

#if ER_HOTRELOAD
/** @brief Chunk size: the USB serial port is quick, so a small chunk is fine and costs little RAM beside the
 *  framebuffer. */
#define RELOAD_CHUNK_BYTES 1024U

/** @brief Bytes read from the USB port per poll, before the watchdog is seen to again. */
#define RELOAD_READ_BYTES 64U

/** @brief How long the frame pacer waits between polls while it has time to spare. */
#define RELOAD_POLL_US 200U

static ErbPackageReload s_reload;
static uint32_t s_reload_buffer[ERB_PACKAGE_RELOAD_BUFFER_BYTES(RELOAD_CHUNK_BYTES) / sizeof(uint32_t)];

static uint32_t slot_offset(unsigned slot)
{
    return slot == 0u ? ER_PACKAGE_SLOT_A_OFFSET : ER_PACKAGE_SLOT_B_OFFSET;
}

static bool erase_slot(unsigned slot, uint32_t offset, uint32_t bytes, void* context)
{
    (void)context;
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(slot_offset(slot) + offset, bytes);
    restore_interrupts(interrupts);
    return true;
}

static bool program_slot(unsigned slot, uint32_t offset, const void* data, uint32_t bytes, void* context)
{
    (void)context;
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_program(slot_offset(slot) + offset, data, bytes);
    restore_interrupts(interrupts);
    return true;
}

/** @brief Prints one line to the host. */
static void reload_log(const char* line, void* context)
{
    (void)context;
    printf("%s\n", line);
}

/**
 * @brief Hands the receiver what the USB port has. Checking a whole package from flash takes longer than a frame, so
 *        the boot's longer watchdog timeout covers it.
 *
 * @return true when a new app started.
 */
static bool reload_poll(void)
{
    uint8_t bytes[RELOAD_READ_BYTES];
    size_t count = 0u;
    while (count < sizeof(bytes))
    {
        const int received = getchar_timeout_us(0);
        if (received == PICO_ERROR_TIMEOUT)
        {
            break;
        }
        bytes[count++] = (uint8_t)received;
    }
    if (count == 0u)
    {
        return false;
    }
    watchdog_enable(ER_WATCHDOG_BOOT_MS, true);
    const bool started = erb_package_reload_feed(&s_reload, bytes, count);
    watchdog_enable(ER_WATCHDOG_MS, true);
    return started;
}
#endif

/**
 * @brief Loads the active package at the panel's size. With none that loads, the error screen stays up and the
 *        frame loop keeps running under it.
 *
 * @param[in] touch  Whether touch came up.
 */
static void load_package(bool touch)
{
    const ErbPackageSlots slots = {
        .slots =
            {
                {(const void*)(XIP_BASE + ER_PACKAGE_SLOT_A_OFFSET), ER_PACKAGE_SLOT_BYTES},
                {(const void*)(XIP_BASE + ER_PACKAGE_SLOT_B_OFFSET), ER_PACKAGE_SLOT_BYTES},
            },
        .invalidate = invalidate_slot,
#if ER_HOTRELOAD
        .erase = erase_slot,
        .program = program_slot,
#endif
    };
    const ErbConfig config = {
        .arena = s_arena,
        .arena_bytes = sizeof(s_arena),
        .screen_width = BOARD_LCD_WIDTH,
        .screen_height = BOARD_LCD_HEIGHT,
        .natives = native_modules,
        .native_count = native_module_count,
    };
    ErbPackageSlotBoot boot;
    const bool running = erb_package_slots_boot(&slots, &config, &boot);
    if (boot.error.status != ERB_OK)
    {
        char message[ERROR_MESSAGE_BYTES];
        erb_error_message(&boot.error, message, sizeof(message));
        boot_report("package not loaded: %s\n", message);
        erb_package_slots_show_error(&boot);
    }
    if (running)
    {
        boot_report("package running from slot %c; slot %c is staging\n", 'a' + boot.active, 'a' + boot.staging);
        if (boot.clear_failed)
        {
            boot_report("couldn't clear the rejected package's header: the next flash may replace the last good one\n");
        }
        if (boot.error.status != ERB_OK)
        {
            hold_error_screen(touch);
        }
    }
#if ER_HOTRELOAD
    erb_package_reload_init(&s_reload, &slots, &config, &boot, s_reload_buffer, RELOAD_CHUNK_BYTES, reload_log, NULL);
#endif
}

#if ER_BOARD_DEBUG
#define STACK_PAINT 0xA5A5A5A5u

/** @brief Left unpainted just below the stack pointer, for stack_paint()'s own frame. */
#define STACK_PAINT_MARGIN_BYTES 64u

/** @brief Fills the unused stack below the caller with a known pattern, for stack_high_water(). */
static void __attribute__((noinline)) stack_paint(void)
{
    uint32_t stack_pointer;
    __asm volatile("mov %0, sp" : "=r"(stack_pointer));
    for (volatile uint32_t* word = s_stack_low; (uint32_t)word < stack_pointer - STACK_PAINT_MARGIN_BYTES; word++)
    {
        *word = STACK_PAINT;
    }
}

/** @brief Returns the deepest the stack has reached since stack_paint(), in bytes from its top. */
static uint32_t stack_high_water(void)
{
    const uint32_t* word = s_stack_low;
    while (word < __StackTop && *word == STACK_PAINT)
    {
        word++;
    }
    return (uint32_t)(__StackTop - word) * sizeof(uint32_t);
}
#endif

int main(void)
{
#if ER_BOARD_DEBUG
    stack_paint();
#endif
    stack_guard_install();

    /* Bring the panel up FIRST — board_display_init()'s very first action drives the backlight low, so
     * the panel's random power-on GRAM ("rainbow") is never lit. Doing it before stdio_init_all() makes
     * that backlight-off happen as early as the firmware possibly can. The backlight stays off until the
     * first real frame is presented (below). */
    const bool display_ok = board_display_init();

    stdio_init_all();
#if ER_BOARD_DEBUG
    /* Give the USB host a moment to attach so the boot log is actually seen. */
    for (int attempt = 0; attempt < USB_WAIT_ATTEMPTS && !stdio_usb_connected(); attempt++)
    {
        sleep_ms(USB_WAIT_STEP_MS);
    }
#endif
    printf("embedded-react RP2040-Touch-LCD-1.69 host — Flow B (no QuickJS)\n");

    if (!display_ok)
    {
        printf("display init failed — halting\n");
        return 1;
    }
    if (!er_pico_spi_lcd_backend_init(board_lcd_ops(), BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT))
    {
        printf("render backend init failed (framebuffer alloc?) — halting\n");
        return 1;
    }
#if ER_BOARD_DEBUG >= 2
    board_backlight(BACKLIGHT_PERCENT); /* light the panel so the bring-up test pattern is actually visible */
    board_lcd_test_pattern();
    printf("test pattern up (R/G/B/W bands) — holding 3 s\n");
    sleep_ms(TEST_PATTERN_HOLD_MS);
#endif

    const bool touch = board_touch_init();
    if (!touch)
    {
        printf("touch init failed — UI renders but input is disabled\n");
    }

    /* IMU for real step counting (shares I2C1 with touch). If absent, the app's steps and level stay unset. */
    const bool imu = board_imu_init();
    native_modules_init();
    Pedometer pedometer;
    pedometer_reset(&pedometer);

    show_recorded_fault(touch);

    /* Arm the watchdog before the app runs: the first full repaint below is where the stack is deepest. */
    watchdog_enable(ER_WATCHDOG_BOOT_MS, true);

    load_package(touch);

    /* Present the first frame while the backlight is still off, THEN light it — so the panel goes
     * straight from black to the watch face, with no flash of noise or blank. (The backlight was held
     * off from power-on through init for exactly this.) */
    er_commit();
    er_pico_spi_lcd_present();
#if ER_BOARD_DEBUG < 2
    board_backlight(BACKLIGHT_PERCENT); /* debug builds already lit it for the test pattern above */
#endif
    watchdog_enable(ER_WATCHDOG_MS, true);

    /* Frame loop. The press state machine turns CST816S polls into down/move/up for the engine. */
    uint32_t previous_ms = now_ms();
#ifdef ER_BOARD_CRASH_TEST
    const uint32_t boot_ms = previous_ms;
#endif
    bool touch_down = false;
    int last_touch_x = 0, last_touch_y = 0;
    while (true)
    {
        const uint32_t frame_start = now_ms();
        watchdog_update();

        static uint32_t s_connected_frames = 0;
        s_connected_frames = stdio_usb_connected() ? s_connected_frames + 1U : 0U;
        if (s_boot_report[0] && s_connected_frames >= BOOT_REPORT_CONNECTED_FRAMES)
        {
            printf("%s", s_boot_report);
            s_boot_report[0] = '\0';
        }

        if (touch)
        {
            int touch_x = 0, touch_y = 0;
            bool pressed = false;
            if (board_touch_read(&touch_x, &touch_y, &pressed))
            {
                if (pressed)
                {
                    embedded_renderer_touch(0, touch_down ? ER_TOUCH_MOVE : ER_TOUCH_DOWN, touch_x, touch_y);
                    touch_down = true;
                    last_touch_x = touch_x;
                    last_touch_y = touch_y;
                }
                else if (touch_down)
                {
                    embedded_renderer_touch(0, ER_TOUCH_UP, last_touch_x, last_touch_y);
                    touch_down = false;
#if ER_HOTRELOAD
                    erb_package_reload_tap(&s_reload);
#endif
                }
            }
        }

        /* Real step counting: poll the accelerometer, run the pedometer, and hand the total to the motion module,
         * which posts it to the app only when it changes: a post runs the app's update. */
        if (imu)
        {
            int16_t accel_x = 0, accel_y = 0, accel_z = 0;
            if (board_imu_read_accel(&accel_x, &accel_y, &accel_z))
            {
                native_modules_set_steps(pedometer_update(&pedometer, accel_x, accel_y, accel_z, frame_start));
            }

            /* Bubble level → the showcase page. Reuse the accel sample just read: low-pass each axis to a
             * steady gravity vector, then map its X/Y to a dot offset in px (±LEVEL_RADIUS, full deflection at
             * ~0.5 g ≈ 30° tilt). Pushed ~30 Hz, and only when it actually moves, so a resting board is
             * quiet. Flip a sign below if the dot rolls the wrong way on your unit. */
            static float filtered_x = 0.0f, filtered_y = 0.0f;
            filtered_x += ((float)accel_x - filtered_x) * LEVEL_SMOOTHING;
            filtered_y += ((float)accel_y - filtered_y) * LEVEL_SMOOTHING;
            static uint32_t s_last_level_ms = 0;
            if (frame_start - s_last_level_ms >= LEVEL_PERIOD_MS)
            {
                s_last_level_ms = frame_start;
                const int level_radius = LEVEL_RADIUS;
                /* The IMU is mounted rotated 90° vs the screen: accel Y drives the horizontal dot, accel X
                 * the vertical, with these signs so the dot rolls toward the low (down-tilted) side. */
                int dot_x = (int)(-filtered_y * level_radius / LEVEL_FULL_TILT);
                int dot_y = (int)(filtered_x * level_radius / LEVEL_FULL_TILT);
                dot_x = dot_x < -level_radius ? -level_radius : (dot_x > level_radius ? level_radius : dot_x);
                dot_y = dot_y < -level_radius ? -level_radius : (dot_y > level_radius ? level_radius : dot_y);
                native_modules_set_tilt(dot_x, dot_y);
            }
        }

        er_commit();
        er_pico_spi_lcd_present();

#if ER_BOARD_DEBUG
        /* 1 Hz heartbeat: frame rate over the last second, plus the running step count. FPS is the whole
         * loop (idle it's paced to ~60; during a swipe the full-screen repaint drops it — that's the
         * metric to watch for choppiness). */
        static uint32_t s_last_beat_ms = 0;
        static uint32_t s_frames = 0;
        s_frames++;
        if (now_ms() - s_last_beat_ms >= MS_PER_SECOND)
        {
            printf("beat t=%lus fps=%lu steps=%lu stack=%lu/%lu\n",
                   (unsigned long)(now_ms() / MS_PER_SECOND),
                   (unsigned long)s_frames,
                   (unsigned long)pedometer.steps,
                   (unsigned long)stack_high_water(),
                   (unsigned long)((__StackTop - s_stack_low) * sizeof(uint32_t)));
            s_frames = 0;
            s_last_beat_ms = now_ms();
        }
#endif

        const uint32_t now = now_ms();
        const uint32_t delta_ms = now - previous_ms;
        embedded_renderer_tick(delta_ms);
        erb_tick(delta_ms); /* advance app timers */
        previous_ms = now;
        native_modules_frame(now); /* the chip's second drives the watch face's clock */

#ifdef ER_BOARD_CRASH_TEST
        crash_test(now - boot_ms);
#endif

        /* Adaptive pacing: sleep the remainder up to the target frame period. */
        const uint32_t used = now_ms() - frame_start;
#if ER_HOTRELOAD
        /* The USB port's buffer is small, so the spare time goes to emptying it. */
        const absolute_time_t frame_end =
            make_timeout_time_ms(used < ER_TARGET_FRAME_MS ? ER_TARGET_FRAME_MS - used : 0U);
        bool started = reload_poll();
        while (!time_reached(frame_end))
        {
            sleep_us(RELOAD_POLL_US);
            started = reload_poll() || started;
        }
        if (started)
        {
            native_modules_repost();
        }
#else
        if (used < ER_TARGET_FRAME_MS)
        {
            sleep_ms(ER_TARGET_FRAME_MS - used);
        }
#endif
    }
}
