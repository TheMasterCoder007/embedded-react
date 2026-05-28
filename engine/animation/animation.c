#include "er_node_internal.h"
#include "renderer_internal.h"
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#ifndef ERUI_MAX_ANIMATIONS
#define ERUI_MAX_ANIMATIONS 64
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Active timing animation slot.
 */
typedef struct
{
    bool active;
    uint16_t node_tag;
    ERAnimProp prop;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    bool loop;
    bool color_value;
    float from_value;
    float to_value;
    uint32_t from_color;
    uint32_t to_color;
} ERAnimation;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERAnimation s_animations[ERUI_MAX_ANIMATIONS];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Converts a float carrying raw uint32_t bits back into an integer.
 *
 * @param[in] value  Float whose bytes contain a uint32_t.
 *
 * @return Recovered uint32_t value.
 */
static uint32_t bits_from_float(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/**
 * @brief Returns true when an animation property is packed ARGB8888 color.
 *
 * @param[in] prop  Animation property.
 *
 * @return true for color properties.
 */
static bool is_color_prop(ERAnimProp prop)
{
    return prop == ER_PROP_BACKGROUND_COLOR || prop == ER_PROP_COLOR;
}

/**
 * @brief Returns true when a node has view visual properties.
 *
 * @param[in] node  Node to inspect.
 *
 * @return true if the node stores ERViewProps.
 */
static bool is_view_node(const ERNode* node)
{
    return node->type == ER_NODE_VIEW || node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_PRESSABLE
           || node->type == ER_NODE_MODAL;
}

/**
 * @brief Reads the current numeric value for an animatable property.
 *
 * @param[in] node   Node to inspect.
 * @param[in] prop   Property to read.
 * @param[out] value  Output numeric value.
 *
 * @return true if the property is supported on the node.
 */
static bool read_numeric_value(const ERNode* node, ERAnimProp prop, float* value)
{
    if (!value)
        return false;

    switch (prop)
    {
        case ER_PROP_OPACITY:
            if (!is_view_node(node))
                return false;
            *value = (float)node->props.view.opacity / 255.0f;
            return true;
        case ER_PROP_TRANSLATE_X:
            *value = node->tp_translate_x;
            return true;
        case ER_PROP_TRANSLATE_Y:
            *value = node->tp_translate_y;
            return true;
        case ER_PROP_SCALE_X:
            *value = node->tp_scale_x;
            return true;
        case ER_PROP_SCALE_Y:
            *value = node->tp_scale_y;
            return true;
        case ER_PROP_ROTATE_Z:
            *value = node->tp_rotate_z;
            return true;
        default:
            return false;
    }
}

/**
 * @brief Reads the current color value for an animatable property.
 *
 * @param[in] node   Node to inspect.
 * @param[in] prop   Property to read.
 * @param[out] color  Output ARGB8888 color.
 *
 * @return true if the property is supported on the node.
 */
static bool read_color_value(const ERNode* node, ERAnimProp prop, uint32_t* color)
{
    if (!color)
        return false;

    switch (prop)
    {
        case ER_PROP_BACKGROUND_COLOR:
            if (!is_view_node(node))
                return false;
            *color = node->props.view.background_color;
            return true;
        case ER_PROP_COLOR:
            if (node->type != ER_NODE_TEXT)
                return false;
            *color = node->props.text.color;
            return true;
        default:
            return false;
    }
}

/**
 * @brief Applies a numeric value to a node property.
 *
 * @param[in,out] node   Target node.
 * @param[in] prop       Property to update.
 * @param[in] value      Numeric value.
 *
 * @return true if the property was applied.
 */
/**
 * @brief Recomputes node->has_transform from the current tp_* fields.
 *
 * @param[in,out] node  Node to update.
 */
static void update_has_transform(ERNode* node)
{
    node->has_transform = (node->tp_translate_x != 0.0f || node->tp_translate_y != 0.0f || node->tp_scale_x != 0.0f
                           || node->tp_scale_y != 0.0f || node->tp_rotate_z != 0.0f);
}

static bool apply_numeric_value(ERNode* node, ERAnimProp prop, float value)
{
    if (!node)
        return false;

    switch (prop)
    {
        case ER_PROP_OPACITY:
            if (!is_view_node(node))
                return false;
            if (value < 0.0f)
                value = 0.0f;
            if (value > 1.0f)
                value = 1.0f;
            node->props.view.opacity = (uint8_t)(value * 255.0f + 0.5f);
            return true;
        case ER_PROP_TRANSLATE_X:
            node->tp_translate_x = value;
            update_has_transform(node);
            return true;
        case ER_PROP_TRANSLATE_Y:
            node->tp_translate_y = value;
            update_has_transform(node);
            return true;
        case ER_PROP_SCALE_X:
            node->tp_scale_x = value;
            update_has_transform(node);
            return true;
        case ER_PROP_SCALE_Y:
            node->tp_scale_y = value;
            update_has_transform(node);
            return true;
        case ER_PROP_ROTATE_Z:
            node->tp_rotate_z = value;
            update_has_transform(node);
            return true;
        default:
            return false;
    }
}

/**
 * @brief Linearly interpolates a color channel.
 *
 * @param[in] from  Start channel value.
 * @param[in] to    Target channel value.
 * @param[in] t     Interpolation fraction in range 0.0-1.0.
 *
 * @return Interpolated channel value.
 */
static uint8_t lerp_channel(uint8_t from, uint8_t to, float t)
{
    const float value = (float)from + ((float)to - (float)from) * t;
    return (uint8_t)(value + 0.5f);
}

/**
 * @brief Linearly interpolates an ARGB8888 color.
 *
 * @param[in] from  Start ARGB8888 color.
 * @param[in] to    Target ARGB8888 color.
 * @param[in] t     Interpolation fraction in range 0.0-1.0.
 *
 * @return Interpolated ARGB8888 color.
 */
static uint32_t lerp_color(uint32_t from, uint32_t to, float t)
{
    const uint8_t a = lerp_channel((uint8_t)(from >> 24), (uint8_t)(to >> 24), t);
    const uint8_t r = lerp_channel((uint8_t)(from >> 16), (uint8_t)(to >> 16), t);
    const uint8_t g = lerp_channel((uint8_t)(from >> 8), (uint8_t)(to >> 8), t);
    const uint8_t b = lerp_channel((uint8_t)from, (uint8_t)to, t);

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/**
 * @brief Applies a color value to a node property.
 *
 * @param[in,out] node   Target node.
 * @param[in] prop       Property to update.
 * @param[in] color      ARGB8888 color.
 *
 * @return true if the property was applied.
 */
static bool apply_color_value(ERNode* node, ERAnimProp prop, uint32_t color)
{
    if (!node)
        return false;

    switch (prop)
    {
        case ER_PROP_BACKGROUND_COLOR:
            if (!is_view_node(node))
                return false;
            node->props.view.background_color = color;
            return true;
        case ER_PROP_COLOR:
            if (node->type != ER_NODE_TEXT)
                return false;
            node->props.text.color = color;
            return true;
        default:
            return false;
    }
}

/**
 * @brief Cancels any active animation matching node and prop.
 *
 * @param[in] node_tag  Node tag to match.
 * @param[in] prop      Property to match.
 */
static void cancel_by_tag(uint16_t node_tag, ERAnimProp prop)
{
    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        if (s_animations[i].active && s_animations[i].node_tag == node_tag && s_animations[i].prop == prop)
            s_animations[i].active = false;
    }
}

/**
 * @brief Returns a free animation slot.
 *
 * @return Free slot, or NULL if the static animation pool is full.
 */
static ERAnimation* alloc_slot(void)
{
    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        if (!s_animations[i].active)
            return &s_animations[i];
    }
    return NULL;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_anim_start(ERNode* node, ERAnimProp prop, float value, const ERAnimConfig* cfg)
{
    if (!node || !node->in_use)
        return;

    const ERAnimType type = cfg ? cfg->type : ER_ANIM_TIMING;
    if (type != ER_ANIM_TIMING)
        return;

    cancel_by_tag(node->tag, prop);

    ERAnimation* anim = alloc_slot();
    if (!anim)
        return;

    memset(anim, 0, sizeof(*anim));
    anim->active = true;
    anim->node_tag = node->tag;
    anim->prop = prop;
    anim->duration_ms = cfg ? cfg->duration_ms : 0U;
    anim->loop = cfg ? cfg->loop : false;
    anim->color_value = is_color_prop(prop);

    if (anim->color_value)
    {
        if (!read_color_value(node, prop, &anim->from_color))
        {
            anim->active = false;
            return;
        }
        anim->to_color = bits_from_float(value);
    }
    else
    {
        if (!read_numeric_value(node, prop, &anim->from_value))
        {
            anim->active = false;
            return;
        }
        anim->to_value = value;
    }

    if (anim->duration_ms == 0U)
    {
        if (anim->color_value)
            (void)apply_color_value(node, prop, anim->to_color);
        else
            (void)apply_numeric_value(node, prop, anim->to_value);
        er_mark_dirty_upward(node);
        anim->active = false;
    }
}

void er_anim_cancel(ERNode* node, ERAnimProp prop)
{
    if (!node)
        return;

    cancel_by_tag(node->tag, prop);
}

void er_anim_tick(uint32_t delta_ms)
{
    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        ERAnimation* anim = &s_animations[i];
        if (!anim->active)
            continue;

        ERNode* node = er_get_node(anim->node_tag);
        if (!node)
        {
            anim->active = false;
            continue;
        }

        anim->elapsed_ms += delta_ms;
        float t = 1.0f;
        if (anim->duration_ms > 0U && anim->elapsed_ms < anim->duration_ms)
            t = (float)anim->elapsed_ms / (float)anim->duration_ms;

        if (anim->color_value)
        {
            const uint32_t color = lerp_color(anim->from_color, anim->to_color, t);
            (void)apply_color_value(node, anim->prop, color);
        }
        else
        {
            const float value = anim->from_value + (anim->to_value - anim->from_value) * t;
            (void)apply_numeric_value(node, anim->prop, value);
        }
        er_mark_dirty_upward(node);

        if (t >= 1.0f)
        {
            if (anim->loop)
            {
                anim->elapsed_ms = 0U;
                if (anim->color_value)
                {
                    const uint32_t from = anim->from_color;
                    anim->from_color = anim->to_color;
                    anim->to_color = from;
                }
                else
                {
                    const float from = anim->from_value;
                    anim->from_value = anim->to_value;
                    anim->to_value = from;
                }
            }
            else
            {
                anim->active = false;
            }
        }
    }
}
