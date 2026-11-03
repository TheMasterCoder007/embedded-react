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
#include "er_scene.h"

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
#define USB_RX_RING_BYTES (16 * 1024)
#define USB_TX_RING_BYTES (4 * 1024)
#define READ_CHUNK_BYTES (4 * 1024)
#define READ_TIMEOUT_MS 50
#define RX_TASK_STACK_BYTES 4096
#define RX_TASK_PRIORITY (tskIDLE_PRIORITY + 5)

/** @brief An upload logs its progress every this many bytes. */
#define PROGRESS_LOG_BYTES (256 * 1024)

#define BYTES_PER_KB 1024

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

/* Single PSRAM staging buffer. The RX task parses an upload into it; the frame loop loads from it. The
 * running app does NOT reference this buffer — er_hotreload_apply loads with copy_assets=true, so a
 * loaded app's assets live in their own block and its bytecode in the JS heap. That means the RX task
 * can refill s_buffer with the next upload while the old app keeps running (and stays on screen), with no
 * teardown until the moment a complete frame is applied. */
static uint8_t* s_buffer = NULL;
static size_t s_capacity = 0;

static QueueHandle_t s_ready_queue = NULL;        /* RX task → frame loop: payload length of a complete frame */
static SemaphoreHandle_t s_done_semaphore = NULL; /* frame loop → RX: the frame has been applied */

/** @brief Hands a completed frame's length to the frame loop and blocks until it has been applied. */
static void rx_submit_apply(size_t length)
{
    xQueueSend(s_ready_queue, &length, portMAX_DELAY);
    xSemaphoreTake(s_done_semaphore, portMAX_DELAY);
}

/*----------------------------------------------------------------------------------------------------------------------
 - RX task
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads USB-Serial-JTAG bytes and parses uploads; hands each complete frame to the frame loop to
 *        apply. No teardown while receiving — the running app doesn't reference s_buffer (see above), so the
 *        old UI stays live until the new app is loaded.
 */
static void er_hotreload_rx_task(void* parameter)
{
    (void)parameter;
    static uint8_t chunk[READ_CHUNK_BYTES];
    ErHotReload reload;
    er_hotreload_init(&reload, s_buffer, s_capacity);
    size_t received_total = 0;
    size_t received_logged = 0;
    bool receiving = false; /* tracks the "upload starting" / progress logging across reads */

    for (;;)
    {
        const int received = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (received <= 0)
        {
            continue;
        }
        if (!receiving)
        {
            ESP_LOGI(TAG, "upload starting — receiving over USB");
            receiving = true;
            received_total = 0;
            received_logged = 0;
        }
        received_total += (size_t)received;
        if (received_total - received_logged >= PROGRESS_LOG_BYTES)
        {
            received_logged = received_total;
            ESP_LOGI(TAG, "rx %u KB", (unsigned)(received_total / BYTES_PER_KB));
        }

        size_t offset = 0;
        while (offset < (size_t)received)
        {
            size_t consumed = 0;
            size_t out_length = 0;
            const ErHotReloadStatus status =
                er_hotreload_feed(&reload, chunk + offset, (size_t)received - offset, &consumed, &out_length);
            offset += consumed;
            if (status == ER_HR_NEED_MORE)
            {
                break;
            }
            if (status != ER_HR_FRAME_READY)
            {
                ESP_LOGW(TAG, "dropped frame: %s", er_hotreload_status_str(status));
                continue;
            }
            if (reload.msg_type != ER_HR_MSG_LOAD_CONTAINER)
            {
                ESP_LOGW(TAG, "hot reload: this firmware runs Flow A: use dev --device without --aot");
                continue;
            }
            /* Complete frame: apply it on the frame-loop task (which resets + loads, copying the assets so
             * s_buffer is free to reuse the moment it returns). Block until done, then keep draining. */
            ESP_LOGI(TAG, "frame received (%u bytes) — applying", (unsigned)out_length);
            rx_submit_apply(out_length);
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
    s_capacity = max_container_bytes;
    s_buffer = (uint8_t*)heap_caps_malloc(s_capacity, MALLOC_CAP_SPIRAM);
    if (!s_buffer)
    {
        ESP_LOGE(TAG, "PSRAM alloc failed (%u bytes) — hot reload disabled", (unsigned)s_capacity);
        return false;
    }

    s_ready_queue = xQueueCreate(1, sizeof(size_t));
    s_done_semaphore = xSemaphoreCreateBinary();
    if (!s_ready_queue || !s_done_semaphore)
    {
        ESP_LOGE(TAG, "queue/sem alloc failed — hot reload disabled");
        if (s_ready_queue)
            vQueueDelete(s_ready_queue);
        if (s_done_semaphore)
            vSemaphoreDelete(s_done_semaphore);
        s_ready_queue = NULL;
        s_done_semaphore = NULL;
        heap_caps_free(s_buffer);
        s_buffer = NULL;
        s_capacity = 0;
        return false;
    }

    /* Attach the USB-Serial-JTAG driver so we can read raw upload bytes. With the console bound to USJ,
     * ESP-IDF may have installed it already (INVALID_STATE) — that's fine, we read the same driver; logs
     * keep flowing out over it. ESP_LOG output reaches the host because USJ IS the console. */
    usb_serial_jtag_driver_config_t usb_config = {
        /* non-const: the install API takes a mutable pointer */
        .rx_buffer_size = USB_RX_RING_BYTES,
        .tx_buffer_size = USB_TX_RING_BYTES,
    };
    const esp_err_t error = usb_serial_jtag_driver_install(&usb_config);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "usb_serial_jtag install failed: %s — hot reload disabled", esp_err_to_name(error));
        vQueueDelete(s_ready_queue);
        vSemaphoreDelete(s_done_semaphore);
        s_ready_queue = NULL;
        s_done_semaphore = NULL;
        heap_caps_free(s_buffer);
        s_buffer = NULL;
        s_capacity = 0;
        return false;
    }

    if (xTaskCreate(er_hotreload_rx_task, "er_hr_rx", RX_TASK_STACK_BYTES, NULL, RX_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "RX task create failed — hot reload disabled");
        if (error == ESP_OK)
            usb_serial_jtag_driver_uninstall();
        vQueueDelete(s_ready_queue);
        vSemaphoreDelete(s_done_semaphore);
        s_ready_queue = NULL;
        s_done_semaphore = NULL;
        heap_caps_free(s_buffer);
        s_buffer = NULL;
        s_capacity = 0;
        return false;
    }

    ESP_LOGI(TAG,
             "on-device hot reload ready (native USB-Serial-JTAG, %u KB PSRAM staging; %u KB PSRAM free)",
             (unsigned)(s_capacity / BYTES_PER_KB),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / BYTES_PER_KB));
    return true;
}

void er_hotreload_usb_pump(void)
{
    if (!s_ready_queue)
    {
        return;
    }
    size_t length = 0;
    if (xQueueReceive(s_ready_queue, &length, 0) != pdTRUE)
    {
        return;
    }
    /* Apply the completed upload: reset + load (copying the assets out of s_buffer). The old app was on
     * screen until now; its last frame stays up through the load, then the new app's first commit
     * replaces it — no "Reloading…" panel. */
    const ErContainerStatus status = er_hotreload_apply(s_buffer, length);
    ESP_LOGI(TAG,
             "hot reload: %s (%u bytes; %u KB PSRAM free; %d nodes)",
             er_runtime_container_status_str(status),
             (unsigned)length,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / BYTES_PER_KB),
             er_node_in_use_count());
    xSemaphoreGive(s_done_semaphore);
}
