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
 * The Cortex-M fault handler: see er_fault_cortex_m.h. er_cortex_m_fault_fill() is plain C, so a host test runs
 * it; the vector, the status registers and the reset only build for a Cortex-M core.
 */

#include "er_fault_cortex_m.h"

#include <stdbool.h>
#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/* The registers the core stacks on an exception, in words. */
#define FRAME_LR 5u
#define FRAME_PC 6u
#define FRAME_XPSR 7u
#define FRAME_WORDS 8u
#define FRAME_FLOAT_WORDS 18u /* s0-s15, FPSCR and a reserved word, after the basic frame */

#define EXC_RETURN_BASIC_FRAME 0x10u /* clear when the float registers were stacked too */
#define XPSR_ALIGNED 0x200u          /* the core added a word to align the stack */

/* The status words er_cortex_m_fault_fill() takes. */
#define STATUS_CFSR 0u
#define STATUS_HFSR 1u
#define STATUS_MMFAR 2u
#define STATUS_BFAR 3u
#define STATUS_WORDS 4u

/* CFSR: the MemManage byte, the BusFault byte and the UsageFault half. */
#define CFSR_MEMORY_MASK 0x000000FFu
#define CFSR_MMARVALID 0x00000080u
#define CFSR_MSTKERR 0x00000010u
#define CFSR_BUS_MASK 0x0000FF00u
#define CFSR_BFARVALID 0x00008000u
#define CFSR_STKERR 0x00001000u
#define CFSR_UNDEFINSTR 0x00010000u
#define CFSR_INVSTATE 0x00020000u
#define CFSR_STKOF 0x00100000u
#define CFSR_DIVBYZERO 0x02000000u
#define CFSR_USAGE_MASK 0xFFFF0000u

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief The end of the stack RAM that holds @p words words at @p address.
 *
 * @return One past its last byte, or 0 when no stack RAM holds them all.
 */
static uintptr_t readable_end(const ERCortexMFaultConfig* config, uintptr_t address, size_t words)
{
    if (address % sizeof(uint32_t) != 0u)
    {
        return 0u;
    }
    for (unsigned index = 0u; index < config->stack_count; index++)
    {
        const ERCortexMFaultStack* stack = &config->stacks[index];
        if (address >= stack->start && address <= stack->end && words * sizeof(uint32_t) <= stack->end - address)
        {
            return stack->end;
        }
    }
    return 0u;
}

/** @brief What the fault was, from the CFSR. */
static const char* fault_reason(uint32_t cfsr)
{
    if (cfsr & (CFSR_STKERR | CFSR_MSTKERR | CFSR_STKOF))
    {
        return "hard fault: stack overflow";
    }
    if (cfsr & CFSR_DIVBYZERO)
    {
        return "hard fault: divide by zero";
    }
    if (cfsr & (CFSR_UNDEFINSTR | CFSR_INVSTATE))
    {
        return "hard fault: bad instruction";
    }
    if (cfsr & CFSR_USAGE_MASK)
    {
        return "hard fault: usage fault";
    }
    if (cfsr & CFSR_BUS_MASK)
    {
        return "hard fault: bus error";
    }
    if (cfsr & CFSR_MEMORY_MASK)
    {
        return "hard fault: memory access";
    }
    return "hard fault";
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_cortex_m_fault_fill(const ERCortexMFaultConfig* config,
                            const uint32_t* frame,
                            uint32_t exc_return,
                            const uint32_t* status)
{
    ERFaultRecord* record = config->record;
    const uint32_t cfsr = status ? status[STATUS_CFSR] : 0u;
    uint32_t address = 0u;
    if (cfsr & CFSR_BFARVALID)
    {
        address = status[STATUS_BFAR];
    }
    else if (cfsr & CFSR_MMARVALID)
    {
        address = status[STATUS_MMFAR];
    }

    if (!readable_end(config, (uintptr_t)frame, FRAME_WORDS))
    {
        er_fault_record(record, "hard fault: bad stack pointer", 0u, address);
        er_fault_record_registers(
            record, ER_FAULT_CORE_CORTEX_M, 0u, (uint32_t)(uintptr_t)frame, status, status ? STATUS_WORDS : 0u);
        return;
    }
    er_fault_record(record, fault_reason(cfsr), frame[FRAME_PC], address);

    /* Where the stack was before the core stacked its registers. */
    const uint32_t* stack = frame + FRAME_WORDS;
    if (!(exc_return & EXC_RETURN_BASIC_FRAME))
    {
        stack += FRAME_FLOAT_WORDS;
    }
    if (frame[FRAME_XPSR] & XPSR_ALIGNED)
    {
        stack++;
    }
    er_fault_record_registers(record,
                              ER_FAULT_CORE_CORTEX_M,
                              frame[FRAME_LR],
                              (uint32_t)(uintptr_t)stack,
                              status,
                              status ? STATUS_WORDS : 0u);

    const uintptr_t stack_end = readable_end(config, (uintptr_t)stack, 0u);
    if (stack_end)
    {
        const size_t available = (stack_end - (uintptr_t)stack) / sizeof(uint32_t);
        er_fault_record_stack(
            record, stack, available < ER_FAULT_STACK_WORDS ? (unsigned)available : ER_FAULT_STACK_WORDS, false);
    }
}

#if defined(__arm__) && defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'M'

/*----------------------------------------------------------------------------------------------------------------------
 - The handler
 ---------------------------------------------------------------------------------------------------------------------*/

/* The System Control Block. */
#define SCB_AIRCR (*(volatile uint32_t*)0xE000ED0Cu)
#define SCB_CFSR (*(volatile uint32_t*)0xE000ED28u)
#define SCB_HFSR (*(volatile uint32_t*)0xE000ED2Cu)
#define SCB_MMFAR (*(volatile uint32_t*)0xE000ED34u)
#define SCB_BFAR (*(volatile uint32_t*)0xE000ED38u)
#define AIRCR_RESET 0x05FA0004u /* the write key and SYSRESETREQ */

/* ARMv6-M and the ARMv8-M baseline have Thumb-1 only, and no fault status registers. */
#if defined(__ARM_ARCH_ISA_THUMB) && __ARM_ARCH_ISA_THUMB == 1
#define HAS_FAULT_STATUS 0
#else
#define HAS_FAULT_STATUS 1
#endif

static ERCortexMFaultConfig s_config;

/* The handler's own stack, so recording a fault never needs the stack that may have overflowed. */
static uint8_t __attribute__((aligned(8))) s_fault_stack[ER_CORTEX_M_FAULT_STACK_BYTES];
static uint8_t* const __attribute__((used)) s_fault_stack_top = s_fault_stack + ER_CORTEX_M_FAULT_STACK_BYTES;

void er_cortex_m_fault_install(const ERCortexMFaultConfig* config)
{
    s_config = *config;
}

/**
 * @brief Records the fault and resets.
 *
 * @param[in] frame       The registers the core stacked.
 * @param[in] exc_return  The handler's lr.
 */
void __attribute__((used, noreturn)) er_cortex_m_fault_record(const uint32_t* frame, uint32_t exc_return)
{
    if (s_config.record)
    {
#if HAS_FAULT_STATUS
        const uint32_t status[STATUS_WORDS] = {
            [STATUS_CFSR] = SCB_CFSR,
            [STATUS_HFSR] = SCB_HFSR,
            [STATUS_MMFAR] = SCB_MMFAR,
            [STATUS_BFAR] = SCB_BFAR,
        };
        er_cortex_m_fault_fill(&s_config, frame, exc_return, status);
#else
        er_cortex_m_fault_fill(&s_config, frame, exc_return, NULL);
#endif
        if (s_config.recorded)
        {
            s_config.recorded(s_config.record);
        }
    }
    SCB_AIRCR = AIRCR_RESET;
    for (;;)
    {
    }
}

/* Finds the stacked registers on the stack that was in use, moves to the handler's own stack, and hands them on. */
void __attribute__((naked)) er_cortex_m_fault_handler(void)
{
    __asm volatile("movs r0, #4\n"
                   "mov r1, lr\n"
                   "tst r0, r1\n"
                   "beq 1f\n"
                   "mrs r0, psp\n"
                   "b 2f\n"
                   "1: mrs r0, msp\n"
                   "2: ldr r2, =s_fault_stack_top\n"
                   "ldr r2, [r2]\n"
                   "msr msp, r2\n"
                   "ldr r2, =er_cortex_m_fault_record\n"
                   "bx r2\n"
                   ".ltorg\n");
}

#endif
