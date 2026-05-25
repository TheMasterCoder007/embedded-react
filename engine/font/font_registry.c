#include "font_registry.h"
#include "font_blob.h"
#include "er_scene.h"
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One slot in the font registry representing a single named font family.
 */
typedef struct
{
    char             name[FONT_NAME_MAX + 1]; /**< Null-terminated family name. */
    const BitmapFont* sizes[FONT_FAMILY_MAX_SIZES]; /**< Registered sizes for this family. */
    uint8_t          size_count; /**< Number of valid entries in sizes[]. */
    bool             in_use;    /**< True when this slot holds an active family. */
} RegistryEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static RegistryEntry s_entries[FONT_REGISTRY_MAX];
static bool          s_initialised = false;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Picks the BitmapFont from a candidate list whose pixel_size is closest to target_px.
 *
 * When two candidates are equidistant, the larger size is preferred.
 *
 * @param[in] candidates  Array of pointers to candidate BitmapFonts.
 * @param[in] count       Number of entries in candidates.
 * @param[in] target_px   Desired pixel size.
 *
 * @return Pointer to the best-matching BitmapFont.
 */
static const BitmapFont* pick_closest(const BitmapFont* const* candidates, size_t count, uint8_t target_px)
{
    const BitmapFont* best      = candidates[0];
    int               best_diff = abs((int)target_px - (int)best->pixel_size);

    for (size_t i = 1; i < count; i++)
    {
        const BitmapFont* c    = candidates[i];
        const int         diff = abs((int)target_px - (int)c->pixel_size);
        if (diff < best_diff || (diff == best_diff && c->pixel_size > best->pixel_size))
        {
            best      = c;
            best_diff = diff;
        }
    }
    return best;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void font_registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_initialised = true;
}

bool font_registry_add(const char* name, const BitmapFont* font)
{
    if (!s_initialised || !name || !font)
        return false;

    int family_slot = -1;
    int free_slot   = -1;
    for (int i = 0; i < (int)FONT_REGISTRY_MAX; i++)
    {
        if (s_entries[i].in_use)
        {
            if (strncmp(s_entries[i].name, name, FONT_NAME_MAX) == 0)
            {
                family_slot = i;
                break;
            }
        }
        else if (free_slot < 0)
        {
            free_slot = i;
        }
    }

    if (family_slot < 0)
    {
        if (free_slot < 0)
            return false;
        family_slot         = free_slot;
        RegistryEntry* e    = &s_entries[family_slot];
        strncpy(e->name, name, FONT_NAME_MAX);
        e->name[FONT_NAME_MAX] = '\0';
        e->size_count          = 0;
        e->in_use              = true;
    }

    RegistryEntry* e = &s_entries[family_slot];
    if (e->size_count >= FONT_FAMILY_MAX_SIZES)
        return false;

    e->sizes[e->size_count++] = font;
    return true;
}

const BitmapFont* font_registry_get(const char* name, uint8_t pixel_size)
{
    if (pixel_size == 0U)
        pixel_size = 16U;

    if (s_initialised && name && name[0] != '\0')
    {
        for (int i = 0; i < (int)FONT_REGISTRY_MAX; i++)
        {
            if (s_entries[i].in_use && s_entries[i].size_count > 0U &&
                strncmp(s_entries[i].name, name, FONT_NAME_MAX) == 0)
            {
                return pick_closest(s_entries[i].sizes, s_entries[i].size_count, pixel_size);
            }
        }
    }

    return pick_closest(g_inter_sizes, g_inter_sizes_count, pixel_size);
}

void er_font_load(const char* name, const void* buf, size_t len)
{
    if (!name || !buf || len == 0)
        return;
    (void)font_blob_register(name, buf, (uint32_t)len);
}
