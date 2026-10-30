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

#include "host_common.h"

#include "er_error_screen.h"
#include "er_scene.h"
#include "esp32_lcd_backend.h"
#include "native_renderer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreatePinnedToCoreWithCaps */
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "embedded-react";

/** @brief Longest the error screen is held at boot; a tap ends it sooner. */
#define ERROR_HOLD_MS 10000U

void remap_touch(int* x, int* y)
{
    const int px = *x, py = *y;
    switch (ER_DISPLAY_ROTATION)
    {
        case 90:
            *x = py;
            *y = BOARD_LCD_WIDTH - 1 - px;
            break;
        case 180:
            *x = BOARD_LCD_WIDTH - 1 - px;
            *y = BOARD_LCD_HEIGHT - 1 - py;
            break;
        case 270:
            *x = BOARD_LCD_HEIGHT - 1 - py;
            *y = px;
            break;
        default:
            break;
    }
}

/** @brief The fault the last reset came from, or NULL when it wasn't one. */
static const char* reset_fault_reason(void)
{
    switch (esp_reset_reason())
    {
        case ESP_RST_PANIC:
            return "panic: a firmware or engine crash";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return "watchdog reset: a hang";
        default:
            return NULL;
    }
}

void hold_error_screen(bool touch)
{
    for (uint32_t waited = 0; waited < ERROR_HOLD_MS; waited += ER_TARGET_FRAME_MS)
    {
        er_commit();
        er_esp32_lcd_present();
        int touch_x = 0;
        int touch_y = 0;
        bool pressed = false;
        if (touch && board_touch_read(&touch_x, &touch_y, &pressed) && pressed)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ER_TARGET_FRAME_MS));
    }
    er_error_screen_hide();
}

void show_reset_fault(bool touch)
{
    const char* reason = reset_fault_reason();
    if (!reason)
    {
        return;
    }
    ESP_LOGE(TAG, "restarted after a fault: %s", reason);
    ERFaultRecord record;
    er_fault_record(&record, reason, 0u, 0u);
    er_fault_show_recorded(&record, SCREEN_W, SCREEN_H);
    hold_error_screen(touch);
}

#if ERUI_RENDER_WORKERS > 1
/*----------------------------------------------------------------------------------------------------------------------
 - Multi-core rendering: worker 1 on CPU core 1
 ---------------------------------------------------------------------------------------------------------------------*/

/* The engine never creates threads — this example provides render worker 1 as a FreeRTOS task
 * pinned to core 1 (the main/render task is pinned to core 0 by ESP-IDF's default main-task
 * affinity, so xPortGetCoreID doubles as the worker id). The engine forks each commit's render
 * into horizontal slices across both cores; see EmbeddedRenderWorkers in native_renderer.h. */
static TaskHandle_t s_render_worker;
static SemaphoreHandle_t s_render_worker_done;

/* Where the worker's stack lives. A WiFi build puts it in PSRAM, which frees the 24 KB of internal RAM WiFi
   is otherwise short of; the worker never touches flash, so a PSRAM stack is safe for it. */
#if CONFIG_ER_WIFI
#define ER_WORKER_STACK_CAPS MALLOC_CAP_SPIRAM
#else
#define ER_WORKER_STACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

static void render_worker_main(void* arg)
{
    (void)arg;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        er_render_worker_exec(1);
        xSemaphoreGive(s_render_worker_done);
    }
}

static void render_worker_dispatch(int worker, void* ctx)
{
    (void)worker; /* only worker 1 exists */
    (void)ctx;
    xTaskNotifyGive(s_render_worker);
}

static void render_worker_sync(void* ctx)
{
    (void)ctx;
    xSemaphoreTake(s_render_worker_done, portMAX_DELAY);
}

static int render_worker_id(void)
{
    return xPortGetCoreID(); /* workers are core-pinned: core id == worker id */
}

void install_render_workers(void)
{
    static const EmbeddedRenderWorkers workers = {
        .count = 2,
        .dispatch = render_worker_dispatch,
        .sync = render_worker_sync,
        .worker_id = render_worker_id,
        .ctx = NULL,
    };
    /* 24 KB: the worker runs the full render recursion (nested composites + transform emits with
     * their on-stack column arrays) but never QuickJS, so it needs render-depth headroom only —
     * 12 KB measurably overflowed on the banded-fade pages. */
    s_render_worker_done = xSemaphoreCreateBinary();
    if (!s_render_worker_done
        || xTaskCreatePinnedToCoreWithCaps(
               render_worker_main, "er_worker", 24576, NULL, 5, &s_render_worker, 1, ER_WORKER_STACK_CAPS)
               != pdPASS)
    {
        if (s_render_worker_done)
        {
            vSemaphoreDelete(s_render_worker_done);
            s_render_worker_done = NULL;
        }
        ESP_LOGW(TAG, "render worker creation failed — rendering stays single-core");
        return;
    }
    embedded_renderer_set_workers(&workers);
    ESP_LOGI(TAG, "multi-core rendering: worker 1 pinned to core 1");
}
#endif /* ERUI_RENDER_WORKERS > 1 */
