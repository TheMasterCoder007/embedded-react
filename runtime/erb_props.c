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

#include "erb_props.h"

#include "erb_format.h"

#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - The ABI's props
 *
 * An id never changes meaning. Adding a prop takes the next free id and bumps ER_ABI; removing one leaves a
 * gap and raises ER_ABI_MIN. Ids 160 to 191 are the four spans' fields (see ERB_PROP_SPAN_FIRST).
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief One span's props. */
#define SPAN_PROPS(span)                                                                                               \
    [ERB_PROP_SPAN(                                                                                                    \
        span,                                                                                                          \
        ERB_SPAN_COLOR)] = {offsetof(ERProps, spans[span].color), ERB_PK_COLOR, ERB_NO_ANIM, 0},                       \
        [ERB_PROP_SPAN(span,                                                                                           \
                       ERB_SPAN_FONT_SIZE)] = {offsetof(ERProps, spans[span].font_size), ERB_PK_U8, ERB_NO_ANIM, 0},   \
        [ERB_PROP_SPAN(                                                                                                \
            span, ERB_SPAN_FONT_WEIGHT)] = {offsetof(ERProps, spans[span].font_weight), ERB_PK_U8, ERB_NO_ANIM, 0},    \
        [ERB_PROP_SPAN(span,                                                                                           \
                       ERB_SPAN_FONT_STYLE)] = {offsetof(ERProps, spans[span].font_style), ERB_PK_U8, ERB_NO_ANIM, 0}, \
        [ERB_PROP_SPAN(span, ERB_SPAN_TEXT_DECORATION)] = {offsetof(ERProps, spans[span].text_decoration),             \
                                                           ERB_PK_U8,                                                  \
                                                           ERB_NO_ANIM,                                                \
                                                           0},                                                         \
        [ERB_PROP_SPAN(span, ERB_SPAN_LETTER_SPACING)] = {                                                             \
            offsetof(ERProps, spans[span].letter_spacing), ERB_PK_I16, ERB_NO_ANIM, 0},

/* clang-format off */
static const ErbPropInfo s_props[] = {
    [1] = {offsetof(ERProps, left), ERB_PK_I16, ERB_NO_ANIM, 0},
    [2] = {offsetof(ERProps, top), ERB_PK_I16, ERB_NO_ANIM, 0},
    [3] = {offsetof(ERProps, right), ERB_PK_I16, ERB_NO_ANIM, 0},
    [4] = {offsetof(ERProps, bottom), ERB_PK_I16, ERB_NO_ANIM, 0},
    [5] = {offsetof(ERProps, width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [6] = {offsetof(ERProps, height), ERB_PK_I16, ERB_NO_ANIM, 0},
    [7] = {offsetof(ERProps, min_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [8] = {offsetof(ERProps, max_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [9] = {offsetof(ERProps, min_height), ERB_PK_I16, ERB_NO_ANIM, 0},
    [10] = {offsetof(ERProps, max_height), ERB_PK_I16, ERB_NO_ANIM, 0},
    [11] = {offsetof(ERProps, padding), ERB_PK_I16, ERB_NO_ANIM, 0},
    [12] = {offsetof(ERProps, padding_left), ERB_PK_I16, ERB_NO_ANIM, 0},
    [13] = {offsetof(ERProps, padding_top), ERB_PK_I16, ERB_NO_ANIM, 0},
    [14] = {offsetof(ERProps, padding_right), ERB_PK_I16, ERB_NO_ANIM, 0},
    [15] = {offsetof(ERProps, padding_bottom), ERB_PK_I16, ERB_NO_ANIM, 0},
    [16] = {offsetof(ERProps, margin), ERB_PK_I16, ERB_NO_ANIM, 0},
    [17] = {offsetof(ERProps, margin_left), ERB_PK_I16, ERB_NO_ANIM, 0},
    [18] = {offsetof(ERProps, margin_top), ERB_PK_I16, ERB_NO_ANIM, 0},
    [19] = {offsetof(ERProps, margin_right), ERB_PK_I16, ERB_NO_ANIM, 0},
    [20] = {offsetof(ERProps, margin_bottom), ERB_PK_I16, ERB_NO_ANIM, 0},
    [21] = {offsetof(ERProps, gap), ERB_PK_I16, ERB_NO_ANIM, 0},
    [22] = {offsetof(ERProps, row_gap), ERB_PK_I16, ERB_NO_ANIM, 0},
    [23] = {offsetof(ERProps, column_gap), ERB_PK_I16, ERB_NO_ANIM, 0},
    [24] = {offsetof(ERProps, flex_grow), ERB_PK_I16, ERB_NO_ANIM, 0},
    [25] = {offsetof(ERProps, flex_shrink), ERB_PK_I16, ERB_NO_ANIM, 0},
    [26] = {offsetof(ERProps, flex_basis), ERB_PK_I16, ERB_NO_ANIM, 0},
    [27] = {offsetof(ERProps, flex_direction), ERB_PK_ENUM, ERB_NO_ANIM, ER_FLEX_COL_REVERSE},
    [28] = {offsetof(ERProps, flex_wrap), ERB_PK_ENUM, ERB_NO_ANIM, ER_WRAP_WRAP_REVERSE},
    [29] = {offsetof(ERProps, align_items), ERB_PK_ENUM, ERB_NO_ANIM, ER_ALIGN_FLEX_END},
    [30] = {offsetof(ERProps, align_self), ERB_PK_ENUM, ERB_NO_ANIM, ER_ALIGN_FLEX_END},
    [31] = {offsetof(ERProps, align_content), ERB_PK_ENUM, ERB_NO_ANIM, ER_ALIGN_CONTENT_SPACE_AROUND},
    [32] = {offsetof(ERProps, justify_content), ERB_PK_ENUM, ERB_NO_ANIM, ER_JUSTIFY_SPACE_EVENLY},
    [33] = {offsetof(ERProps, position), ERB_PK_ENUM, ERB_NO_ANIM, ER_POS_ABSOLUTE},
    [34] = {offsetof(ERProps, display), ERB_PK_ENUM, ERB_NO_ANIM, ER_DISPLAY_NONE},
    [35] = {offsetof(ERProps, aspect_ratio), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [36] = {offsetof(ERProps, margin_horizontal), ERB_PK_I16, ERB_NO_ANIM, 0},
    [37] = {offsetof(ERProps, margin_vertical), ERB_PK_I16, ERB_NO_ANIM, 0},
    [38] = {offsetof(ERProps, padding_horizontal), ERB_PK_I16, ERB_NO_ANIM, 0},
    [39] = {offsetof(ERProps, padding_vertical), ERB_PK_I16, ERB_NO_ANIM, 0},
    [40] = {offsetof(ERProps, flex_basis_pct), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [41] = {offsetof(ERProps, width_pct), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [42] = {offsetof(ERProps, height_pct), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [43] = {offsetof(ERProps, left_pct), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [44] = {offsetof(ERProps, top_pct), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [45] = {offsetof(ERProps, right_pct), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [46] = {offsetof(ERProps, bottom_pct), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [47] = {offsetof(ERProps, background_color), ERB_PK_COLOR, ER_PROP_BACKGROUND_COLOR, 0},
    [48] = {offsetof(ERProps, border_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [49] = {offsetof(ERProps, border_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [50] = {offsetof(ERProps, border_radius), ERB_PK_I16, ERB_NO_ANIM, 0},
    [51] = {offsetof(ERProps, border_top_left_radius), ERB_PK_I16, ERB_NO_ANIM, 0},
    [52] = {offsetof(ERProps, border_top_right_radius), ERB_PK_I16, ERB_NO_ANIM, 0},
    [53] = {offsetof(ERProps, border_bottom_left_radius), ERB_PK_I16, ERB_NO_ANIM, 0},
    [54] = {offsetof(ERProps, border_bottom_right_radius), ERB_PK_I16, ERB_NO_ANIM, 0},
    [55] = {offsetof(ERProps, border_left_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [56] = {offsetof(ERProps, border_top_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [57] = {offsetof(ERProps, border_right_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [58] = {offsetof(ERProps, border_bottom_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [59] = {offsetof(ERProps, border_left_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [60] = {offsetof(ERProps, border_top_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [61] = {offsetof(ERProps, border_right_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [62] = {offsetof(ERProps, border_bottom_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [63] = {offsetof(ERProps, border_style), ERB_PK_ENUM, ERB_NO_ANIM, ER_BORDER_DOTTED},
    [64] = {offsetof(ERProps, z_index), ERB_PK_I16, ERB_NO_ANIM, 0},
    [65] = {offsetof(ERProps, opacity), ERB_PK_OPACITY, ER_PROP_OPACITY, 0},
    [66] = {offsetof(ERProps, overflow), ERB_PK_ENUM, ERB_NO_ANIM, ER_OVERFLOW_SCROLL},
    [67] = {offsetof(ERProps, pointer_events), ERB_PK_ENUM, ERB_NO_ANIM, ER_POINTER_EVENTS_BOX_NONE},
    [68] = {offsetof(ERProps, hit_slop_left), ERB_PK_I16, ERB_NO_ANIM, 0},
    [69] = {offsetof(ERProps, hit_slop_top), ERB_PK_I16, ERB_NO_ANIM, 0},
    [70] = {offsetof(ERProps, hit_slop_right), ERB_PK_I16, ERB_NO_ANIM, 0},
    [71] = {offsetof(ERProps, hit_slop_bottom), ERB_PK_I16, ERB_NO_ANIM, 0},
    [72] = {offsetof(ERProps, long_press_ms), ERB_PK_U16, ERB_NO_ANIM, 0},
    [73] = {offsetof(ERProps, text), ERB_PK_STRING, ERB_NO_ANIM, ER_TEXT_MAX},
    [74] = {offsetof(ERProps, font_family), ERB_PK_STRING, ERB_NO_ANIM, ER_FONT_FAMILY_MAX},
    [75] = {offsetof(ERProps, color), ERB_PK_COLOR, ER_PROP_COLOR, 0},
    [76] = {offsetof(ERProps, font_size), ERB_PK_U8, ERB_NO_ANIM, 0},
    [77] = {offsetof(ERProps, font_weight), ERB_PK_ENUM, ERB_NO_ANIM, ERB_FONT_WEIGHT_BOLD},
    [78] = {offsetof(ERProps, font_style), ERB_PK_ENUM, ERB_NO_ANIM, ER_FONT_STYLE_ITALIC},
    [79] = {offsetof(ERProps, text_align), ERB_PK_ENUM, ERB_NO_ANIM, ER_TEXT_ALIGN_RIGHT},
    [80] = {offsetof(ERProps, number_of_lines), ERB_PK_U8, ERB_NO_ANIM, 0},
    [81] = {offsetof(ERProps, ellipsize_mode), ERB_PK_ENUM, ERB_NO_ANIM, ER_TEXT_ELLIPSIZE_CLIP},
    [82] = {offsetof(ERProps, text_decoration), ERB_PK_ENUM, ERB_NO_ANIM, ER_TEXT_DECORATION_LINE_THROUGH},
    [83] = {offsetof(ERProps, line_height), ERB_PK_I16, ERB_NO_ANIM, 0},
    [84] = {offsetof(ERProps, letter_spacing), ERB_PK_I16, ERB_NO_ANIM, 0},
    [85] = {offsetof(ERProps, image_name), ERB_PK_IMAGE, ERB_NO_ANIM, ER_IMAGE_NAME_MAX},
    [86] = {offsetof(ERProps, resize_mode), ERB_PK_ENUM, ERB_NO_ANIM, ER_RESIZE_CENTER},
    [87] = {offsetof(ERProps, tint_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [88] = {offsetof(ERProps, transform_translate_x), ERB_PK_FLOAT, ER_PROP_TRANSLATE_X, 0},
    [89] = {offsetof(ERProps, transform_translate_y), ERB_PK_FLOAT, ER_PROP_TRANSLATE_Y, 0},
    [90] = {offsetof(ERProps, transform_scale_x), ERB_PK_FLOAT, ER_PROP_SCALE_X, 0},
    [91] = {offsetof(ERProps, transform_scale_y), ERB_PK_FLOAT, ER_PROP_SCALE_Y, 0},
    [92] = {offsetof(ERProps, transform_rotate_z), ERB_PK_FLOAT, ER_PROP_ROTATE_Z, 0},
    [93] = {offsetof(ERProps, transform_origin_x), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [94] = {offsetof(ERProps, transform_origin_y), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [95] = {offsetof(ERProps, transform_rotate_x), ERB_PK_FLOAT, ER_PROP_ROTATE_X, 0},
    [96] = {offsetof(ERProps, transform_rotate_y), ERB_PK_FLOAT, ER_PROP_ROTATE_Y, 0},
    [97] = {offsetof(ERProps, transform_perspective), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [98] = {offsetof(ERProps, shadow_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [99] = {offsetof(ERProps, shadow_offset_x), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [100] = {offsetof(ERProps, shadow_offset_y), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [101] = {offsetof(ERProps, shadow_opacity), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [102] = {offsetof(ERProps, shadow_radius), ERB_PK_U8, ERB_NO_ANIM, 0},
    [103] = {offsetof(ERProps, elevation), ERB_PK_U8, ERB_NO_ANIM, 0},
    [104] = {offsetof(ERProps, indicator_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [105] = {offsetof(ERProps, animating), ERB_PK_BOOL, ERB_NO_ANIM, 0},
    [106] = {offsetof(ERProps, switch_value), ERB_PK_BOOL, ER_PROP_SWITCH_THUMB, 0},
    [107] = {offsetof(ERProps, track_color_false), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [108] = {offsetof(ERProps, track_color_true), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [109] = {offsetof(ERProps, thumb_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [110] = {offsetof(ERProps, placeholder), ERB_PK_STRING, ERB_NO_ANIM, ER_PLACEHOLDER_MAX},
    [111] = {offsetof(ERProps, placeholder_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [112] = {offsetof(ERProps, cursor_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [113] = {offsetof(ERProps, editable), ERB_PK_BOOL, ERB_NO_ANIM, 0},
    [114] = {offsetof(ERProps, secure_text_entry), ERB_PK_BOOL, ERB_NO_ANIM, 0},
    [115] = {offsetof(ERProps, modal_visible), ERB_PK_BOOL, ERB_NO_ANIM, 0},
    [116] = {offsetof(ERProps, backdrop_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [117] = {offsetof(ERProps, arc_value), ERB_PK_FLOAT, ER_PROP_ARC_VALUE, 0},
    [118] = {offsetof(ERProps, arc_value_start), ERB_PK_FLOAT, ER_PROP_ARC_VALUE_START, 0},
    [119] = {offsetof(ERProps, arc_min_span), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [120] = {offsetof(ERProps, arc_min), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [121] = {offsetof(ERProps, arc_max), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [122] = {offsetof(ERProps, arc_start_angle), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [123] = {offsetof(ERProps, arc_sweep_angle), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [124] = {offsetof(ERProps, arc_step), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [125] = {offsetof(ERProps, arc_gap_angle), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [126] = {offsetof(ERProps, arc_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [127] = {offsetof(ERProps, arc_band_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [128] = {offsetof(ERProps, arc_knob_size), ERB_PK_I16, ERB_NO_ANIM, 0},
    [129] = {offsetof(ERProps, arc_knob_border_width), ERB_PK_I16, ERB_NO_ANIM, 0},
    [130] = {offsetof(ERProps, arc_track_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [131] = {offsetof(ERProps, arc_indicator_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [132] = {offsetof(ERProps, arc_band_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [133] = {offsetof(ERProps, arc_knob_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [134] = {offsetof(ERProps, arc_knob_border_color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [135] = {offsetof(ERProps, arc_segments), ERB_PK_U8, ERB_NO_ANIM, 0},
    [136] = {offsetof(ERProps, arc_cap), ERB_PK_ENUM, ERB_NO_ANIM, ER_ARC_CAP_ROUND},
    [137] = {offsetof(ERProps, arc_knob), ERB_PK_ENUM, ERB_NO_ANIM, ER_ARC_KNOB_CHILD},
    [138] = {offsetof(ERProps, arc_adjustable), ERB_PK_BOOL, ERB_NO_ANIM, 0},
    [139] = {offsetof(ERProps, arc_range), ERB_PK_BOOL, ERB_NO_ANIM, 0},
    [140] = {offsetof(ERProps, gradient_type), ERB_PK_ENUM, ERB_NO_ANIM, ER_GRADIENT_CONIC},
    [141] = {offsetof(ERProps, gradient_angle), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [142] = {offsetof(ERProps, gradient_stop_count), ERB_PK_ENUM, ERB_NO_ANIM, ER_GRADIENT_MAX_STOPS},
    [143] = {offsetof(ERProps, gradient_stops[0].color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [144] = {offsetof(ERProps, gradient_stops[1].color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [145] = {offsetof(ERProps, gradient_stops[2].color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [146] = {offsetof(ERProps, gradient_stops[3].color), ERB_PK_COLOR, ERB_NO_ANIM, 0},
    [147] = {offsetof(ERProps, gradient_stops[0].position), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [148] = {offsetof(ERProps, gradient_stops[1].position), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [149] = {offsetof(ERProps, gradient_stops[2].position), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    [150] = {offsetof(ERProps, gradient_stops[3].position), ERB_PK_FLOAT, ERB_NO_ANIM, 0},
    SPAN_PROPS(0)
    SPAN_PROPS(1)
    SPAN_PROPS(2)
    SPAN_PROPS(3)
};
/* clang-format on */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

const ErbPropInfo* erb_prop_info(uint16_t id)
{
    if (id >= sizeof(s_props) / sizeof(s_props[0]) || s_props[id].kind == 0u)
    {
        return NULL;
    }
    return &s_props[id];
}
