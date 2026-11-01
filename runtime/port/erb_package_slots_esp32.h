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

#ifndef ERB_PACKAGE_SLOTS_ESP32_H
#define ERB_PACKAGE_SLOTS_ESP32_H

/*
 * The package slots on ESP-IDF: the partitions labelled erb_a and erb_b, memory-mapped for good, since a running
 * package is read in place. Only an ESP-IDF project compiles this file; the rest of runtime/ has no platform headers.
 */

#include "erb_package_slots.h"

#include <stdbool.h>

/**
 * @brief Finds and maps the slot partitions. A slot it can't map stays empty, and reads as having no package.
 *
 * @param[out] slots  Receives the slots.
 *
 * @return true when both are mapped.
 */
bool erb_package_slots_esp32_open(ErbPackageSlots* slots);

#endif
