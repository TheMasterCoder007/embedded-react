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

#ifndef FAULT_RECORD_H
#define FAULT_RECORD_H

/*
 * Records a panic for the next boot. The panic handler fills an ERFaultRecord in no-init RAM with what it is given:
 * the program counter, the address, the exception cause and the callers its own unwinder finds. The firmware's
 * CMakeLists.txt puts this file's handler ahead of ESP-IDF's with `-Wl,--wrap=esp_panic_handler`.
 */

#include "er_error_screen.h"

#include <stdbool.h>

/**
 * @brief Sets what adds the app's place to a panic's record: erb_fault_record_app() in a Flow B firmware.
 *
 * @param[in] record_app  Called from the panic handler; NULL for none.
 */
void fault_record_install(void (*record_app)(ERFaultRecord* record));

/**
 * @brief Takes the fault the last reset came from: the panic's record, or one that names a watchdog reset.
 *
 * @param[out] record  Receives it.
 *
 * @return false when the last reset wasn't a fault.
 */
bool fault_record_take(ERFaultRecord* record);

#endif
