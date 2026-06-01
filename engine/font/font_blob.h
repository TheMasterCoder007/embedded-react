#ifndef EMBEDDED_REACT_FONT_BLOB_H
#define EMBEDDED_REACT_FONT_BLOB_H

#include "font_bitmap.h"
#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FONT_BLOB_MAGIC_0 0x46U   /**< Wire-format magic byte 0: 'F'. */
#define FONT_BLOB_MAGIC_1 0x4FU   /**< Wire-format magic byte 1: 'O'. */
#define FONT_BLOB_MAGIC_2 0x4EU   /**< Wire-format magic byte 2: 'N'. */
#define FONT_BLOB_MAGIC_3 0x54U   /**< Wire-format magic byte 3: 'T'. */
#define FONT_BLOB_VERSION 2U      /**< Version of supported font blob. */
#define FONT_BLOB_HEADER_SIZE 17U /**< Fixed header size in bytes. */

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Result codes returned by font_blob_register().
 */
typedef enum
{
    FONT_BLOB_OK = 0,                /**< Registration succeeded. */
    FONT_BLOB_ERR_TOO_SHORT = 1,     /**< Blob is smaller than the minimum header size. */
    FONT_BLOB_ERR_BAD_MAGIC = 2,     /**< Magic bytes do not match the FONT signature. */
    FONT_BLOB_ERR_BAD_VERSION = 3,   /**< Blob version is not supported. */
    FONT_BLOB_ERR_BAD_RANGE = 4,     /**< Glyph range fields are inconsistent. */
    FONT_BLOB_ERR_SIZE_MISMATCH = 5, /**< Blob is smaller than the size implied by the header. */
    FONT_BLOB_ERR_OUT_OF_MEMORY = 6, /**< Not enough space in the font pool. */
    FONT_BLOB_ERR_REGISTRY_FULL = 7, /**< Font registry has no free slots. */
} FontBlobStatus;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initializes the font blob allocator, optionally reserving an initial byte range.
 *
 * Call this once before registering any blobs. The reserve_offset can be used
 * to skip over pre-occupied storage (e.g., by a boot-time font in ROM).
 *
 * @param[in] reserve_offset  Number of bytes at the start of the pool to reserve.
 */
void font_blob_init(uint32_t reserve_offset);

/**
 * @brief Returns the number of bytes consumed from the font pool.
 *
 * @return Bytes used so far by all registered font blobs.
 */
uint32_t font_blob_used_bytes(void);

/**
 * @brief Parses a FONT wire-format blob and registers it in the font registry.
 *
 * The blob data is copied into the static font pool (if ERUI_FONT_POOL_BYTES > 0),
 * and a BitmapFont entry is added to the registry under the given name.
 *
 * Re-registration of the same family name with a new pixel size simply appends another
 * size to the family (the normal multi-size workflow). Re-registration with a pixel size
 * that is already present replaces that size in place: when the new blob fits within the
 * byte footprint reserved by the prior registration it is repacked into the same pool
 * region (no extra bytes consumed); when it does not fit, fresh pool bytes are allocated
 * and the prior region's bytes are NOT reclaimed (the bump allocator has no free list).
 * Call font_blob_init() (alongside font_registry_init()) to reclaim the entire pool at once.
 *
 * @param[in] name       Null-terminated font family name.
 * @param[in] blob       Pointer to the FONT wire-format data.
 * @param[in] blob_size  Size of the blob in bytes.
 *
 * @return FONT_BLOB_OK on success, or a FontBlobStatus error code on failure.
 */
FontBlobStatus font_blob_register(const char* name, const uint8_t* blob, uint32_t blob_size);

#endif
