#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Packs a uint32_t into a float without numeric conversion.
 *
 * @param[in] bits  Raw bits to store in the float.
 *
 * @return Float carrying bits.
 */
static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/**
 * @brief Returns an ERProps struct with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return ERProps with layout defaults initialized.
 */
static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    return p;
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable failure description.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point for timing animation behaviour.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    embedded_renderer_set_backend(NULL);

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.width = 100;
    p.height = 40;
    p.background_color = 0xFFFF0000U;
    p.opacity = 255U;
    er_node_set_props(card, &p);

    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 100U;

    er_anim_start(card, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF00FF00U), &cfg);
    embedded_renderer_tick(50U);
    if (card->props.view.background_color != 0xFF808000U)
        return fail("background color did not interpolate at half duration");
    if (!card->dirty)
        return fail("animated node was not marked dirty");

    embedded_renderer_tick(50U);
    if (card->props.view.background_color != 0xFF00FF00U)
        return fail("background color did not finish at target");

    er_anim_start(card, ER_PROP_OPACITY, 0.0f, &cfg);
    embedded_renderer_tick(25U);
    er_anim_cancel(card, ER_PROP_OPACITY);
    const uint8_t cancelled_opacity = card->props.view.opacity;
    embedded_renderer_tick(100U);
    if (card->props.view.opacity != cancelled_opacity)
        return fail("cancelled opacity animation continued ticking");

    cfg.duration_ms = 0U;
    er_anim_start(card, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF0000FFU), &cfg);
    if (card->props.view.background_color != 0xFF0000FFU)
        return fail("zero-duration animation did not apply immediately");

    return EXIT_SUCCESS;
}
