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

#include "fault_record.h"

#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "xtensa_context.h"
#endif

#include <stddef.h>
#include <stdint.h>

/** @brief Starts each panic's reason. */
#define PANIC_PREFIX "panic: "

/** @brief The panic's record, in RAM a reset leaves alone. */
static __NOINIT_ATTR ERFaultRecord s_fault;

static void (*s_record_app)(ERFaultRecord* record);

void __real_esp_panic_handler(panic_info_t* info);

/** @brief Records @p info, as far as this core's registers are known here. */
static void record_panic(const panic_info_t* info)
{
    /* "panic: " and ESP-IDF's name for it, such as LoadProhibited. */
    char reason[ER_FAULT_REASON_BYTES] = PANIC_PREFIX;
    size_t length = sizeof(PANIC_PREFIX) - 1u;
    for (const char* text = info->reason; text && *text && length + 1u < sizeof(reason); text++)
    {
        reason[length++] = *text;
    }
    reason[length] = '\0';

#if CONFIG_IDF_TARGET_ARCH_XTENSA
    const XtExcFrame* frame = (const XtExcFrame*)info->frame;
    if (frame && !info->pseudo_excause)
    {
        er_fault_record(&s_fault, reason, (uint32_t)(uintptr_t)info->addr, (uint32_t)frame->excvaddr);
        const uint32_t status[] = {(uint32_t)frame->exccause, (uint32_t)frame->excvaddr};
        er_fault_record_registers(&s_fault,
                                  ER_FAULT_CORE_XTENSA,
                                  esp_cpu_process_stack_pc((uint32_t)frame->a0),
                                  (uint32_t)frame->a1,
                                  status,
                                  sizeof(status) / sizeof(status[0]));
    }
    else
    {
        er_fault_record(&s_fault, reason, (uint32_t)(uintptr_t)info->addr, 0u);
    }
    if (frame)
    {
        /* The callers, as the panic's own backtrace finds them. */
        uint32_t callers[ER_FAULT_STACK_WORDS];
        unsigned count = 0u;
        esp_backtrace_frame_t walk = {
            .pc = (uint32_t)frame->pc,
            .sp = (uint32_t)frame->a1,
            .next_pc = (uint32_t)frame->a0,
            .exc_frame = frame,
        };
        while (count < ER_FAULT_STACK_WORDS && walk.next_pc != 0u && esp_backtrace_get_next_frame(&walk))
        {
            callers[count++] = esp_cpu_process_stack_pc(walk.pc);
        }
        er_fault_record_stack(&s_fault, callers, count, true);
    }
#else
    er_fault_record(&s_fault, reason, (uint32_t)(uintptr_t)info->addr, 0u);
#endif
    if (s_record_app)
    {
        s_record_app(&s_fault);
    }
}

/** @brief The panic handler, in ESP-IDF's place: records the panic, then lets ESP-IDF print it and reset. */
void __wrap_esp_panic_handler(panic_info_t* info)
{
    record_panic(info);
    __real_esp_panic_handler(info);
}

void fault_record_install(void (*record_app)(ERFaultRecord* record))
{
    s_record_app = record_app;
}

bool fault_record_take(ERFaultRecord* record)
{
    const esp_reset_reason_t reset = esp_reset_reason();
    if (er_fault_recorded(&s_fault) && reset != ESP_RST_POWERON)
    {
        *record = s_fault;
        s_fault.magic = 0u;
        return true;
    }
    s_fault.magic = 0u;
    switch (reset)
    {
        case ESP_RST_PANIC:
            er_fault_record(record, "panic: a firmware or engine crash", 0u, 0u);
            return true;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            er_fault_record(record, "watchdog reset: a hang", 0u, 0u);
            return true;
        default:
            return false;
    }
}
