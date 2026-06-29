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

#include "hotreload_usb.h"

#include "er_hotreload.h"
#include "er_runtime.h"

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdint.h>

static const char* TAG = "er-hotreload";

/* Transport: the chip's native USB-Serial-JTAG controller (GPIO19/20), which on this board is the second
 * USB-C port. The console is bound to it (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG, see sdkconfig.defaults),
 * which keeps the native USB enumerated after boot AND installs the usb_serial_jtag driver — so the SAME
 * port carries logs OUT and hot-reload uploads IN, at full-speed USB rates (not baud-limited) with USB
 * flow control (the device NAKs when its RX buffer is full, so the host never overruns it — no dropped
 * bytes, no host-side pacing). The ERHR frame's magic + CRC still let the parser pick uploads out of the
 * byte stream and ignore log echoes. Flashing stays on the separate CH343/UART0 port. */
#define ER_HR_USB_RX_RING (16 * 1024)
#define ER_HR_USB_TX_RING (4 * 1024)
#define ER_HR_READ_CHUNK (4 * 1024)

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

/* Single PSRAM staging buffer. The RX task parses an upload into it; the frame loop loads from it. The
 * running app does NOT reference this buffer — er_hotreload_apply loads with copy_assets=true, so a
 * loaded app's assets live in their own block and its bytecode in the JS heap. That means the RX task
 * can refill s_buf with the next upload while the old app keeps running (and stays on screen), with no
 * teardown until the moment a complete frame is applied. */
static uint8_t* s_buf = NULL;
static size_t s_cap = 0;

static QueueHandle_t s_ready_q = NULL;      /* RX task → frame loop: payload length of a complete frame */
static SemaphoreHandle_t s_done_sem = NULL; /* frame loop → RX: the frame has been applied */

/** @brief Hands a completed frame's length to the frame loop and blocks until it has been applied. */
static void rx_submit_apply(size_t len)
{
    xQueueSend(s_ready_q, &len, portMAX_DELAY);
    xSemaphoreTake(s_done_sem, portMAX_DELAY);
}

/*----------------------------------------------------------------------------------------------------------------------
 - RX task
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads USB-Serial-JTAG bytes and parses uploads; hands each complete frame to the frame loop to
 *        apply. No teardown while receiving — the running app doesn't reference s_buf (see above), so the
 *        old UI stays live until the new app is loaded.
 */
static void er_hotreload_rx_task(void* arg)
{
    (void)arg;
    static uint8_t chunk[ER_HR_READ_CHUNK];
    ErHotReload hr;
    er_hotreload_init(&hr, s_buf, s_cap);
    size_t rx_total = 0;
    size_t rx_logged = 0;
    bool receiving = false; /* tracks the "upload starting" / progress logging across reads */

    for (;;)
    {
        const int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (n <= 0)
        {
            continue;
        }
        if (!receiving)
        {
            ESP_LOGI(TAG, "upload starting — receiving over USB");
            receiving = true;
            rx_total = 0;
            rx_logged = 0;
        }
        rx_total += (size_t)n;
        if (rx_total - rx_logged >= 256 * 1024)
        {
            rx_logged = rx_total;
            ESP_LOGI(TAG, "rx %u KB", (unsigned)(rx_total / 1024));
        }

        size_t off = 0;
        while (off < (size_t)n)
        {
            size_t consumed = 0;
            size_t out_len = 0;
            const ErHotReloadStatus st = er_hotreload_feed(&hr, chunk + off, (size_t)n - off, &consumed, &out_len);
            off += consumed;
            if (st == ER_HR_NEED_MORE)
            {
                break;
            }
            if (st != ER_HR_FRAME_READY)
            {
                ESP_LOGW(TAG, "dropped frame: %s", er_hotreload_status_str(st));
                continue;
            }
            /* Complete frame: apply it on the frame-loop task (which resets + loads, copying the assets so
             * s_buf is free to reuse the moment it returns). Block until done, then keep draining. */
            ESP_LOGI(TAG, "frame received (%u bytes) — applying", (unsigned)out_len);
            rx_submit_apply(out_len);
            receiving = false; /* next inbound byte begins a fresh upload */
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_hotreload_usb_start(size_t max_container_bytes)
{
    if (max_container_bytes == 0)
    {
        return false;
    }
    s_cap = max_container_bytes;
    s_buf = (uint8_t*)heap_caps_malloc(s_cap, MALLOC_CAP_SPIRAM);
    if (!s_buf)
    {
        ESP_LOGE(TAG, "PSRAM alloc failed (%u bytes) — hot reload disabled", (unsigned)s_cap);
        return false;
    }

    s_ready_q = xQueueCreate(1, sizeof(size_t));
    s_done_sem = xSemaphoreCreateBinary();
    if (!s_ready_q || !s_done_sem)
    {
        ESP_LOGE(TAG, "queue/sem alloc failed — hot reload disabled");
        return false;
    }

    /* Attach the USB-Serial-JTAG driver so we can read raw upload bytes. With the console bound to USJ,
     * ESP-IDF may have installed it already (INVALID_STATE) — that's fine, we read the same driver; logs
     * keep flowing out over it. ESP_LOG output reaches the host because USJ IS the console. */
    usb_serial_jtag_driver_config_t usb_cfg = {
        /* non-const: the install API takes a mutable pointer */
        .rx_buffer_size = ER_HR_USB_RX_RING,
        .tx_buffer_size = ER_HR_USB_TX_RING,
    };
    const esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "usb_serial_jtag install failed: %s — hot reload disabled", esp_err_to_name(err));
        return false;
    }

    if (xTaskCreate(er_hotreload_rx_task, "er_hr_rx", 4096, NULL, tskIDLE_PRIORITY + 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "RX task create failed — hot reload disabled");
        return false;
    }

    ESP_LOGI(TAG,
             "on-device hot reload ready (native USB-Serial-JTAG, %u KB PSRAM staging; %u KB PSRAM free)",
             (unsigned)(s_cap / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return true;
}

void er_hotreload_usb_pump(void)
{
    if (!s_ready_q)
    {
        return;
    }
    size_t len = 0;
    if (xQueueReceive(s_ready_q, &len, 0) != pdTRUE)
    {
        return;
    }
    /* Apply the completed upload: reset + load (copying the assets out of s_buf). The old app was on
     * screen until now; its last frame stays up through the load, then the new app's first commit
     * replaces it — no "Reloading…" panel. */
    const ErContainerStatus st = er_hotreload_apply(s_buf, len);
    ESP_LOGI(TAG,
             "hot reload: %s (%u bytes; %u KB PSRAM free)",
             er_runtime_container_status_str(st),
             (unsigned)len,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    xSemaphoreGive(s_done_sem);
}
