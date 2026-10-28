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

#ifndef ERB_PROPS_H
#define ERB_PROPS_H

/*
 * erb_props — the engine ABI's prop ids, mapped to ERProps fields. A package names a prop by id, never by
 * offset, so a firmware with a different ERProps layout runs the same package.
 */

#include "er_scene.h"

#include <stdint.h>

/** @brief Span props: ids from ERB_PROP_SPAN_FIRST, ERB_PROP_SPAN_STRIDE per span. */
#define ERB_PROP_SPAN_FIRST 160u
#define ERB_PROP_SPAN_STRIDE 8u
#define ERB_PROP_SPAN(span, field) (ERB_PROP_SPAN_FIRST + (span) * ERB_PROP_SPAN_STRIDE + (field))

/** @brief A span's fields, in its ids. */
enum
{
    ERB_SPAN_COLOR = 0,
    ERB_SPAN_FONT_SIZE = 1,
    ERB_SPAN_FONT_WEIGHT = 2,
    ERB_SPAN_FONT_STYLE = 3,
    ERB_SPAN_TEXT_DECORATION = 4,
    ERB_SPAN_LETTER_SPACING = 5,
};

/** @brief ErbPropInfo::anim of a prop nothing animates. */
#define ERB_NO_ANIM (-1)

/** @brief Prop values the engine has no name for. */
#define ERB_FONT_WEIGHT_BOLD 1u
#define ERB_OPACITY_OPAQUE 255u /**< The engine's opacity byte at 1. */
#define ERB_FULL_PERCENT 100.0f

/** @brief One prop. */
typedef struct
{
    uint16_t offset; /**< offsetof(ERProps, field). */
    uint8_t kind;    /**< ERB_PK_ value kind. */
    int8_t anim;     /**< The ERAnimProp that animates it, or ERB_NO_ANIM. */
    uint16_t max;    /**< ENUM: largest value. STRING and IMAGE: cap in bytes. */
} ErbPropInfo;

/**
 * @brief Looks a prop up by id.
 *
 * @return Its entry, or NULL when this firmware has no such prop.
 */
const ErbPropInfo* erb_prop_info(uint16_t id);

#endif
