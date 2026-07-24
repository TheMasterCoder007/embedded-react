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
 * embedded-react — Waveshare RP2040-Touch-LCD-1.69 host (Flow B / AOT).
 *
 * The smallest board in the examples family: an RP2040 (264 KB SRAM, no FPU, no PSRAM) driving a
 * 1.69" 240x280 ST7789V2 panel with CST816S capacitive touch. Like the CYD example, this runs
 * the AOT-compiled C app (dist/app.gen.c, from `npm run aot`) with NO QuickJS and no JS at runtime.
 * It brings up the panel + touch (board.c), registers the pico-spi-lcd render backend, calls
 * er_app_build() once, and runs the frame loop: poll touch, commit, present, tick.
 *
 * See ./README.md.
 */

#include "app.gen.h"
#include "assets.generated.h" /* er_register_assets() — baked <Image> assets (a no-op when none) */
#include "board.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "pedometer.h"
#include "pico_spi_lcd_backend.h"

#include "pico/stdlib.h"

#include <stdio.h>

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

int main(void)
{
    /* Bring the panel up FIRST — board_display_init()'s very first action drives the backlight low, so
     * the panel's random power-on GRAM ("rainbow") is never lit. Doing it before stdio_init_all() makes
     * that backlight-off happen as early as the firmware possibly can. The backlight stays off until the
     * first real frame is presented (below). */
    const bool display_ok = board_display_init();

    stdio_init_all();
#if ER_BOARD_DEBUG
    /* Give the USB host a moment to attach so the boot log is actually seen. */
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++)
    {
        sleep_ms(100);
    }
#endif
    printf("embedded-react RP2040-Touch-LCD-1.69 host — Flow B (AOT, no QuickJS)\n");

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
    board_backlight(90); /* light the panel so the bring-up test pattern is actually visible */
    board_lcd_test_pattern();
    printf("test pattern up (R/G/B/W bands) — holding 3 s\n");
    sleep_ms(3000);
#endif

    const bool touch = board_touch_init();
    if (!touch)
    {
        printf("touch init failed — UI renders but input is disabled\n");
    }

    /* IMU for real step counting (shares I2C1 with touch). If absent, steps just stay at 0. */
    const bool imu = board_imu_init();
    Pedometer pedo;
    pedometer_reset(&pedo);

    /* Register baked <Image> assets BEFORE building the tree so Image nodes resolve their source by
       name (a no-op for the watch face — it imports no images). */
    er_register_assets();

    /* Build the compiled app's scene graph (no JS — straight er_scene.h calls baked at build time). */
    er_app_build(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    printf("AOT app built at %dx%d (no QuickJS)\n", BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);

    /* Present the first frame while the backlight is still off, THEN light it — so the panel goes
     * straight from black to the watch face, with no flash of noise or blank. (The backlight was held
     * off from power-on through init for exactly this.) */
    er_commit();
    er_pico_spi_lcd_present();
#if ER_BOARD_DEBUG < 2
    board_backlight(90); /* debug builds already lit it for the test pattern above */
#endif

    /* Frame loop. The press state machine turns CST816S polls into down/move/up for the engine. */
    uint32_t prev = now_ms();
    bool touch_down = false;
    int last_x = 0, last_y = 0;
    while (true)
    {
        const uint32_t frame_start = now_ms();

        if (touch)
        {
            int tx = 0, ty = 0;
            bool pressed = false;
            if (board_touch_read(&tx, &ty, &pressed))
            {
                if (pressed)
                {
                    embedded_renderer_touch(0, touch_down ? ER_TOUCH_MOVE : ER_TOUCH_DOWN, tx, ty);
                    touch_down = true;
                    last_x = tx;
                    last_y = ty;
                }
                else if (touch_down)
                {
                    embedded_renderer_touch(0, ER_TOUCH_UP, last_x, last_y);
                    touch_down = false;
                }
            }
        }

        /* Real step counting: poll the accelerometer, run the pedometer, and push the total into the app
         * (er_app_set_steps → the useHostValue('steps') field) only when it changes — app_update() re-
         * applies dependent nodes, so there's no point re-running it every frame. */
        if (imu)
        {
            int16_t ax = 0, ay = 0, az = 0;
            if (board_imu_read_accel(&ax, &ay, &az))
            {
                const uint32_t steps = pedometer_update(&pedo, ax, ay, az, frame_start);
                static uint32_t s_last_steps = 0;
                if (steps != s_last_steps)
                {
                    s_last_steps = steps;
                    er_app_set_steps((int)steps);
                }
            }

            /* Bubble level → the showcase page. Reuse the accel sample just read: low-pass each axis to a
             * steady gravity vector, then map its X/Y to a dot offset in px (±LEVEL_R, full deflection at
             * ~0.5 g ≈ 30° tilt). Pushed ~30 Hz, and only when it actually moves, so a resting board is
             * quiet. Flip a sign below if the dot rolls the wrong way on your unit. */
            static float fax = 0.0f, fay = 0.0f;
            fax += ((float)ax - fax) * 0.15f;
            fay += ((float)ay - fay) * 0.15f;
            static uint32_t s_last_level_ms = 0;
            if (frame_start - s_last_level_ms >= 33u)
            {
                s_last_level_ms = frame_start;
                const int LEVEL_R = 74;
                /* The IMU is mounted rotated 90° vs the screen: accel Y drives the horizontal dot, accel X
                 * the vertical, with these signs so the dot rolls toward the low (down-tilted) side. */
                int ndx = (int)(-fay * LEVEL_R / 4096.0f);
                int ndy = (int)(fax * LEVEL_R / 4096.0f);
                ndx = ndx < -LEVEL_R ? -LEVEL_R : (ndx > LEVEL_R ? LEVEL_R : ndx);
                ndy = ndy < -LEVEL_R ? -LEVEL_R : (ndy > LEVEL_R ? LEVEL_R : ndy);
                static int ldx = 1000, ldy = 1000;
                if (ndx != ldx)
                {
                    ldx = ndx;
                    er_app_set_dotx(ndx);
                }
                if (ndy != ldy)
                {
                    ldy = ndy;
                    er_app_set_doty(ndy);
                }
            }
        }

        er_commit();
        er_pico_spi_lcd_present();

#if ER_BOARD_DEBUG
        /* 1 Hz heartbeat: frame rate over the last second, plus the running step count. FPS is the whole
         * loop (idle it's paced to ~60; during a swipe the full-screen repaint drops it — that's the
         * metric to watch for choppiness). */
        static uint32_t s_last_beat = 0;
        static uint32_t s_frames = 0;
        s_frames++;
        if (now_ms() - s_last_beat >= 1000U)
        {
            printf("beat t=%lus fps=%lu steps=%lu\n",
                   (unsigned long)(now_ms() / 1000U),
                   (unsigned long)s_frames,
                   (unsigned long)pedo.steps);
            s_frames = 0;
            s_last_beat = now_ms();
        }
#endif

        const uint32_t now = now_ms();
        const uint32_t dt = now - prev;
        embedded_renderer_tick(dt);
        er_app_tick((int)dt); /* advance app timers — the watch face's 1 Hz movement lives here */
        prev = now;

        /* Adaptive pacing: sleep the remainder up to the target frame period. */
        const uint32_t used = now_ms() - frame_start;
        if (used < ER_TARGET_FRAME_MS)
        {
            sleep_ms(ER_TARGET_FRAME_MS - used);
        }
    }
}
