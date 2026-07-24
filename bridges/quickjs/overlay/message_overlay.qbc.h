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

#ifndef ER_MESSAGE_OVERLAY_QBC_H
#define ER_MESSAGE_OVERLAY_QBC_H

/*
 * Precompiled bytecode of overlay/message_overlay.js (see that file for what it renders and how to
 * regenerate). Bytecode so the error overlay works on parser-less (QJS_DISABLE_PARSER) builds; the
 * format is tied to the pinned QuickJS release (ER_QUICKJS_TAG) — regenerate on an engine bump.
 */

#include <stdint.h>

extern const uint8_t er_overlay_qbc[];
extern const unsigned int er_overlay_qbc_len;

#endif
