#ifndef EMBEDDED_REACT_FONT_REGISTRY_H
#define EMBEDDED_REACT_FONT_REGISTRY_H

#include "font_bitmap.h"
#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum number of distinct font families that can be registered. */
#define FONT_REGISTRY_MAX 8U

/** @brief Maximum number of pixel sizes registered per font family. */
#define FONT_FAMILY_MAX_SIZES 16U

/** @brief Maximum length of a font family name, excluding the null terminator. */
#define FONT_NAME_MAX 31U

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initializes the font registry, clearing all previously registered fonts.
 */
void font_registry_init(void);

/**
 * @brief Adds a BitmapFont to the registry under the given family name.
 *
 * If a family with this name already exists, a new size entry is appended to it.
 * If the name is new, a fresh family slot is allocated.
 *
 * @param[in] name  Null-terminated font family name (max FONT_NAME_MAX characters).
 * @param[in] font  Pointer to the BitmapFont to register.
 *
 * @return true on success, false if the registry or the family's size table is full.
 */
bool font_registry_add(const char* name, const BitmapFont* font);

/**
 * @brief Retrieves the best-matching BitmapFont for a given family name and pixel size.
 *
 * If the name is NULL or empty, or no matching family is found, it falls back to the
 * built-in Inter Regular font at the closest available size.
 *
 * @param[in] name        Null-terminated font family name, or NULL for the default.
 * @param[in] pixel_size  Desired font size in pixels (0 defaults to 16).
 *
 * @return Pointer to the closest matching BitmapFont (never NULL).
 */
const BitmapFont* font_registry_get(const char* name, uint8_t pixel_size);

#endif
