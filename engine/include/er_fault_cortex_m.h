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

#ifndef EMBEDDED_REACT_ER_FAULT_CORTEX_M_H
#define EMBEDDED_REACT_ER_FAULT_CORTEX_M_H

/*
 * The fault handler for Cortex-M firmware: fills an ERFaultRecord with the program counter, the link register,
 * the fault status registers and the words on the stack, then resets. It runs on a stack of its own and
 * allocates nothing.
 *
 *     static ERFaultRecord s_fault __attribute__((section(".noinit")));
 *
 *     ER_CORTEX_M_FAULT_VECTOR(HardFault_Handler)
 *
 *     static const ERCortexMFaultStack stacks[] = {{(uintptr_t)&_sdata, (uintptr_t)&_estack}};
 *     const ERCortexMFaultConfig fault = {
 *         .record = &s_fault,
 *         .stacks = stacks, // the RAM the stacks are in
 *         .stack_count = 1,
 *         .recorded = erb_fault_record_app, // Flow B: where the app was
 *     };
 *     er_cortex_m_fault_install(&fault);
 *
 * At the next boot, er_fault_text() gives the record for a log and er_fault_show_recorded() shows it.
 *
 * A fault the firmware gives a vector of its own never reaches the hard fault's: turning the MPU on enables the
 * memory fault, for one. Give each such vector the handler too. And where the record is in RAM behind a data
 * cache, clean its cache lines in `recorded`, or a reset loses what was written last.
 * A stack overflow that faults while the core stacks its registers locks the core up before the handler
 * runs, so it leaves no record: a watchdog has to catch that.
 */

#include "er_error_screen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief The handler's own stack, in bytes: a multiple of 8. Recording takes about 100 on a Cortex-M0+. */
#ifndef ER_CORTEX_M_FAULT_STACK_BYTES
#define ER_CORTEX_M_FAULT_STACK_BYTES 256
#endif

    /** @brief RAM that holds stacks: a main stack, an RTOS's task stacks, a stack the firmware switches to. */
    typedef struct
    {
        uintptr_t start;
        uintptr_t end; /**< One past its last byte. */
    } ERCortexMFaultStack;

    /** @brief What the handler records into, and what it may read. */
    typedef struct
    {
        ERFaultRecord* record; /**< In no-init RAM. */
        /** The RAM the stacks are in. The handler reads the stacked registers and the stack words only inside the
         *  one the stack pointer is in. Kept by address: it must outlive the firmware. */
        const ERCortexMFaultStack* stacks;
        unsigned stack_count;
        /** Called with the record once it is filled, to add to it (erb_fault_record_app()) or to reset the board its
         *  own way. May be NULL. When it returns, the handler resets the core. */
        void (*recorded)(ERFaultRecord* record);
    } ERCortexMFaultConfig;

    /**
     * @brief Sets what the fault handler records into. Call it early in boot; faults before it reset with no
     *        record.
     *
     * @param[in] config  Copied.
     */
    void er_cortex_m_fault_install(const ERCortexMFaultConfig* config);

    /**
     * @brief Fills a record from a fault's stacked registers. The handler calls it; it is apart so it can be
     *        tested on a host.
     *
     * @param[in]  config      What to record into, and the RAM the stack words may be read from.
     * @param[in]  frame       The registers the core stacked: r0-r3, r12, lr, pc, xpsr.
     * @param[in]  exc_return  The handler's lr, which says how much the core stacked.
     * @param[in]  status      CFSR, HFSR, MMFAR and BFAR, or NULL on a core without them.
     */
    void er_cortex_m_fault_fill(const ERCortexMFaultConfig* config,
                                const uint32_t* frame,
                                uint32_t exc_return,
                                const uint32_t* status);

    /** @brief The handler. A vector reaches it through ER_CORTEX_M_FAULT_VECTOR. */
    void er_cortex_m_fault_handler(void);

/**
 * @brief Defines the vector @p name (HardFault_Handler with CMSIS, isr_hardfault with the pico-sdk) as a jump to
 *        the handler. BusFault_Handler, MemManage_Handler and UsageFault_Handler can be given the same way where the
 *        firmware enables them.
 */
#define ER_CORTEX_M_FAULT_VECTOR(name)                                                                                 \
    void __attribute__((naked)) name(void)                                                                             \
    {                                                                                                                  \
        __asm volatile("ldr r0, =er_cortex_m_fault_handler\n"                                                          \
                       "bx r0\n"                                                                                       \
                       ".ltorg\n");                                                                                    \
    }

#ifdef __cplusplus
}
#endif

#endif
