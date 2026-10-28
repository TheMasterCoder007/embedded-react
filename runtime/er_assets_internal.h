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

#ifndef ER_ASSETS_INTERNAL_H
#define ER_ASSETS_INTERNAL_H

/*
 * er_assets_internal — the pack walk shared by er_assets.c (checking, caller storage) and
 * er_assets_heap.c (heap font tables). The heap path is its own file so a firmware with no heap never
 * links the allocator.
 */

#include "er_assets.h"

/** @brief Font families a walk tells apart; more than this counts as one more. */
#define ER_ASSETS_MAX_FAMILIES 16u

typedef struct ErAssetsWalk ErAssetsWalk;

/**
 * @brief Gets the tables for one font: its BitmapFont and room for its glyph and extra-glyph records.
 *
 * @return false when there is no room, which fails the walk.
 */
typedef bool (*ErAssetsTablesFn)(ErAssetsWalk* walk,
                                 uint16_t glyph_count,
                                 uint16_t extras_count,
                                 BitmapFont** font,
                                 GlyphInfo** glyphs,
                                 ExtraGlyph** extras);

/** @brief Gives back tables from ErAssetsTablesFn for a font that wasn't registered; may be NULL. */
typedef void (*ErAssetsReleaseFn)(BitmapFont* font, GlyphInfo* glyphs, ExtraGlyph* extras);

/** @brief A font family seen so far: its name in the pack (as far as the registry compares it), and its sizes. */
typedef struct
{
    const uint8_t* name;
    uint8_t name_length;
    uint16_t sizes;
} ErAssetsFamily;

/** @brief One walk over a pack. */
struct ErAssetsWalk
{
    bool registering;          /**< Register each image and font; else only check and count. */
    ErAssetsTablesFn tables;   /**< Registering: where font tables come from. */
    ErAssetsReleaseFn release; /**< Registering: gives back a font's tables when it isn't registered. */
    ErAssetsStorage* storage;  /**< Caller storage, for tables that come from it. */
    ErAssetsInfo info;         /**< Counts so far. */
    const char* find;          /**< Non-NULL: an image name to look for. */
    bool found;                /**< The image named by find was seen. */
    ErAssetsFamily families[ER_ASSETS_MAX_FAMILIES];
};

/**
 * @brief Walks a pack: checks every record and, when registering, registers each image and font.
 *
 * @return true when the whole pack was read cleanly and every font's tables were found.
 */
bool er_assets_walk(const uint8_t* source, size_t length, ErAssetsWalk* walk);

#endif
