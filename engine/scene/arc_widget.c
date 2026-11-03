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

/*----------------------------------------------------------------------------------------------------------------------
 - Arc widget (ER_NODE_ARC): node-level glue over the analytic sector rasterizer.
 *
 * One node draws a dial / gauge / progress ring: an optional wide backing band, the track over the whole
 * sweep, the value indicator over [start, value], and a knob at the value end. The value is a first-class
 * animatable property (ER_PROP_ARC_VALUE), and a value change damages only the sub-arc it swept plus the
 * knob's two footprints — not the node box — so a native ramp or a finger drag repaints a sliver.
 ---------------------------------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------------------------------
 - Includes
 ---------------------------------------------------------------------------------------------------------------------*/

#include "arc_widget.h"
#include "arc.h"
#include "image_scaler.h"
#include "renderer_internal.h"
#include "rrect.h"
#include <math.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Macros
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Minimum touch slop either side of the ring (px) — fingers are wider than a 4 px track. */
#define ARC_HIT_SLOP_MIN 8

/** @brief Extra touch radius around a drawn knob (px). */
#define ARC_KNOB_HIT_SLOP 4.0f

/** @brief Default colours, mirrored from the ERProps docs. */
#define ARC_DEFAULT_BAND_COLOR 0x33000000U
#define ARC_DEFAULT_TRACK_COLOR 0xFF3A3A3CU
#define ARC_DEFAULT_INDICATOR_COLOR 0xFF0A84FFU
#define ARC_DEFAULT_KNOB_COLOR 0xFFFFFFFFU

/** @brief Default geometry, mirrored from the ERProps docs. */
#define ARC_DEFAULT_SWEEP 270.0f       /**< Degrees. */
#define ARC_DEFAULT_START_ANGLE 135.0f /**< Degrees, used with the default sweep. */
#define ARC_DEFAULT_RANGE 100.0f       /**< max - min when max is not above min. */
#define ARC_DEFAULT_WIDTH_DIVISOR 10   /**< Default track width is the box's shorter side over this. */
#define ARC_MIN_WIDTH 2.0f             /**< Thinnest default track (px). */
#define ARC_DEFAULT_KNOB_GROWTH 8      /**< Default knob diameter past the track width (px). */
#define ARC_DEFAULT_GAP_ANGLE 2.0f     /**< Degrees between segments. */
#define ARC_DEFAULT_STEP 1.0f

/** @brief Antialiasing fringe past a knob's edge (px), for the overhang. */
#define ARC_AA_FRINGE 1.0f

/** @brief Padding on value-change damage (px): the antialiasing fringe plus rounding. */
#define ARC_DAMAGE_PAD 1.5f

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static inline float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

/** @brief The colour, or its default when it is 0. */
static inline uint32_t col_or(uint32_t color, uint32_t fallback)
{
    return color ? color : fallback;
}

/**
 * @brief Whether this arc's conic indicator ramp is anchored to the BAND (the lit indicator) rather than
 *        to the whole sweep.
 *
 * A RANGE band ramps across ITSELF, so it always covers exactly what is lit however wide it is; a
 * single-ended arc anchors to the whole sweep instead, so a colour belongs to a position on the dial and
 * does not shift as the value grows.
 *
 * This is the single source of truth for both the paint and the damage. A band-anchored ramp re-maps
 * every pixel between the indicator's ends whenever either end moves, so if the two ever disagreed a
 * value change would leave stale colours behind — keep them reading the same predicate.
 */
static bool arc_grad_band_anchored(const ERArcProps* props)
{
    if (props->gradient_type != ER_GRADIENT_CONIC || props->gradient_stop_count < 2U)
        return false;
    return props->range != 0U;
}

/** @brief The first child a knob-child arc anchors (display:none children are skipped). */
static ERNode* knob_child(const ERNode* node)
{
    uint16_t tag = node->first_child_tag;
    while (tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(tag);
        if (!child)
            return NULL;
        if (child->layout.display != ER_DISPLAY_NONE)
            return child;
        tag = child->next_sibling_tag;
    }
    return NULL;
}

/** @brief Unions a node-local rect into the node's pending vector_dirty rect (or starts one). */
static void union_local_dirty(ERNode* node, int left, int top, int right, int bottom)
{
    if (node->vector_has_dirty)
    {
        const int old_left = node->vector_dirty_x, old_top = node->vector_dirty_y;
        const int old_right = old_left + node->vector_dirty_width, old_bottom = old_top + node->vector_dirty_height;
        if (old_left < left)
            left = old_left;
        if (old_top < top)
            top = old_top;
        if (old_right > right)
            right = old_right;
        if (old_bottom > bottom)
            bottom = old_bottom;
    }
    node->vector_dirty_x = (int16_t)left;
    node->vector_dirty_y = (int16_t)top;
    node->vector_dirty_width = (int16_t)(right - left);
    node->vector_dirty_height = (int16_t)(bottom - top);
    node->vector_has_dirty = (right > left && bottom > top);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_arc_geom(const ERNode* node, int box_x, int box_y, int box_width, int box_height, ERArcGeom* geometry)
{
    const ERArcProps* props = &node->props.arc;

    er_layout_content_box(&node->layout, &box_x, &box_y, &box_width, &box_height);

    const int side = (box_width < box_height) ? box_width : box_height;
    const float outer_radius = (float)side * 0.5f;

    float width = (float)props->width;
    if (width <= 0.0f)
    {
        width = (float)(side / ARC_DEFAULT_WIDTH_DIVISOR);
        if (width < ARC_MIN_WIDTH)
            width = ARC_MIN_WIDTH;
    }
    if (width > outer_radius)
        width = outer_radius;
    float band = (float)(props->band_width > 0 ? props->band_width : 0);
    if (band > 2.0f * outer_radius)
        band = 2.0f * outer_radius;

    geometry->center_x = (float)box_x + (float)box_width * 0.5f;
    geometry->center_y = (float)box_y + (float)box_height * 0.5f;
    geometry->width = width;
    geometry->band = band;
    geometry->half_extent = ((band > width) ? band : width) * 0.5f;
    geometry->mid_radius = outer_radius - geometry->half_extent;
    if (geometry->mid_radius < 0.0f)
        geometry->mid_radius = 0.0f;

    float sweep = props->sweep_angle;
    float start_angle = props->start_angle;
    if (!(fabsf(start_angle) < INFINITY)) /* app maths can reach these, and a NaN would reach the knob's int casts */
        start_angle = 0.0f;
    if (!(sweep > 0.0f))
    {
        sweep = ARC_DEFAULT_SWEEP;
        if (start_angle == 0.0f)
            start_angle = ARC_DEFAULT_START_ANGLE;
    }
    if (sweep > 360.0f)
        sweep = 360.0f;
    geometry->start_angle = start_angle;
    geometry->sweep = sweep;

    geometry->min = props->min;
    geometry->max = (props->max > props->min) ? props->max : (props->min + ARC_DEFAULT_RANGE);

    if (props->knob == ER_ARC_KNOB_CIRCLE || props->knob == ER_ARC_KNOB_IMAGE)
    {
        int knob_size = props->knob_size;
        if (knob_size <= 0)
            knob_size = (int)width + ARC_DEFAULT_KNOB_GROWTH;
        geometry->knob_size = knob_size;
    }
    else
    {
        geometry->knob_size = 0;
    }
}

float er_arc_value_frac(const ERArcGeom* geometry, float value)
{
    const float span = geometry->max - geometry->min;
    if (!(span > 0.0f))
        return 0.0f;
    const float fraction = clampf((value - geometry->min) / span, 0.0f, 1.0f);
    return fraction == fraction ? fraction : 0.0f;
}

float er_arc_value_angle(const ERArcGeom* geometry, float value)
{
    return geometry->start_angle + geometry->sweep * er_arc_value_frac(geometry, value);
}

void er_arc_knob_center(const ERArcGeom* geometry, float value, float* knob_x, float* knob_y)
{
    const float angle = er_arc_value_angle(geometry, value) * ER_DEG2RAD;
    *knob_x = geometry->center_x + geometry->mid_radius * cosf(angle);
    *knob_y = geometry->center_y + geometry->mid_radius * sinf(angle);
}

int er_arc_refresh_overhang(ERNode* node)
{
    ERArcGeom geometry;
    er_arc_geom(node, 0, 0, (int)node->computed.width, (int)node->computed.height, &geometry);
    int overhang = 0;
    if (geometry.knob_size > 0)
    {
        const float reach = (float)geometry.knob_size * 0.5f + ARC_AA_FRINGE;
        const float excess = reach - geometry.half_extent;
        if (excess > 0.0f)
            overhang = (int)ceilf(excess);
    }
    node->arc_overhang = (int16_t)overhang;
    return overhang;
}

void er_arc_render(ERNode* node, int box_x, int box_y, int width, int height)
{
    const ERArcProps* props = &node->props.arc;
    ERArcGeom geometry;
    er_arc_geom(node, box_x, box_y, width, height, &geometry);
    er_arc_refresh_overhang(node);

    /* Rasterize only where this commit is repainting: the node box (plus the knob overhang) intersected
     * with the active damage scissor — the whole point of the tight value-change damage. */
    const int overhang = (int)node->arc_overhang;
    int clip_left = box_x - overhang, clip_top = box_y - overhang, clip_right = box_x + width + overhang,
        clip_bottom = box_y + height + overhang;
    int scissor_x, scissor_y, scissor_width, scissor_height;
    if (er_get_clip_rect(&scissor_x, &scissor_y, &scissor_width, &scissor_height))
    {
        if (scissor_x > clip_left)
            clip_left = scissor_x;
        if (scissor_y > clip_top)
            clip_top = scissor_y;
        if (scissor_x + scissor_width < clip_right)
            clip_right = scissor_x + scissor_width;
        if (scissor_y + scissor_height < clip_bottom)
            clip_bottom = scissor_y + scissor_height;
    }
    if (clip_right <= clip_left || clip_bottom <= clip_top)
    {
        node->arc_painted_value = node->arc_value;
        return;
    }

    ERArcSector sector;
    memset(&sector, 0, sizeof(sector));
    sector.cx = geometry.center_x;
    sector.cy = geometry.center_y;
    sector.cap = props->cap;
    sector.clip_x0 = clip_left;
    sector.clip_y0 = clip_top;
    sector.clip_x1 = clip_right;
    sector.clip_y1 = clip_bottom;
    if (props->segments > 1U)
    {
        sector.segments = props->segments;
        sector.seg_a0 = geometry.start_angle;
        sector.seg_a1 = geometry.start_angle + geometry.sweep;
        sector.gap_deg = (props->gap_angle > 0.0f) ? props->gap_angle : ARC_DEFAULT_GAP_ANGLE;
    }

    /* 1. Backing band (wider than the track, centred on the same mid-line). */
    if (geometry.band > 0.0f)
    {
        sector.r_outer = geometry.mid_radius + geometry.band * 0.5f;
        sector.r_inner = geometry.mid_radius - geometry.band * 0.5f;
        sector.a0 = geometry.start_angle;
        sector.a1 = geometry.start_angle + geometry.sweep;
        sector.color = col_or(props->band_color, ARC_DEFAULT_BAND_COLOR);
        sector.grad_type = ER_GRADIENT_NONE;
        er_arc_fill_sector(&sector);
    }

    /* 2. Track over the whole sweep. */
    sector.r_outer = geometry.mid_radius + geometry.width * 0.5f;
    sector.r_inner = geometry.mid_radius - geometry.width * 0.5f;
    sector.a0 = geometry.start_angle;
    sector.a1 = geometry.start_angle + geometry.sweep;
    sector.color = col_or(props->track_color, ARC_DEFAULT_TRACK_COLOR);
    sector.grad_type = ER_GRADIENT_NONE;
    er_arc_fill_sector(&sector);

    /* 3. Indicator. Single-ended it runs from the sweep start to the value; in RANGE mode it is the band
     *    BETWEEN the two values, so a dual-setpoint dial (a thermostat's AUTO lo..hi) is one node. */
    const float fraction = er_arc_value_frac(&geometry, node->arc_value);
    const float start_fraction = props->range ? er_arc_value_frac(&geometry, node->arc_value_start) : 0.0f;
    if (fraction > start_fraction)
    {
        sector.a0 = geometry.start_angle + geometry.sweep * start_fraction;
        sector.a1 = geometry.start_angle + geometry.sweep * fraction;
        sector.color = col_or(props->indicator_color, ARC_DEFAULT_INDICATOR_COLOR);
        if ((props->gradient_type == ER_GRADIENT_CONIC || props->gradient_type == ER_GRADIENT_RADIAL)
            && props->gradient_stop_count >= 2U)
        {
            sector.grad_type = props->gradient_type;
            sector.grad_stops = props->gradient_stops;
            sector.grad_stop_count = props->gradient_stop_count;
            /* Where the ramp is anchored — see arc_grad_band_anchored(). BAND spans the lit indicator
             * (a thermostat's AUTO warm→cool, always covering exactly what is drawn); SWEEP spans the
             * whole dial (a progress ring's 0-100 scale, colours fixed to positions). */
            const bool band_gradient = arc_grad_band_anchored(props);
            sector.grad_a0 = band_gradient ? sector.a0 : geometry.start_angle;
            sector.grad_a1 = band_gradient ? sector.a1 : (geometry.start_angle + geometry.sweep);
        }
        er_arc_fill_sector(&sector);
    }

    /* 4. Knob — one at the value end, plus one at the low end in RANGE mode. */
    if (geometry.knob_size > 0)
    {
        const int ends = props->range ? 2 : 1;
        for (int end_index = 0; end_index < ends; end_index++)
        {
            float knob_x, knob_y;
            er_arc_knob_center(&geometry, (end_index == 0) ? node->arc_value : node->arc_value_start, &knob_x, &knob_y);
            const int knob_size = geometry.knob_size;
            const int knob_left = (int)floorf(knob_x - (float)knob_size * 0.5f + 0.5f);
            const int knob_top = (int)floorf(knob_y - (float)knob_size * 0.5f + 0.5f);
            if (props->knob == ER_ARC_KNOB_CIRCLE)
            {
                const uint32_t fill = col_or(props->knob_color, ARC_DEFAULT_KNOB_COLOR);
                const uint32_t border_color =
                    col_or(props->knob_border_color, col_or(props->indicator_color, ARC_DEFAULT_INDICATOR_COLOR));
                er_rrect_fill_bordered(fill,
                                       border_color,
                                       (int)props->knob_border_width,
                                       knob_left,
                                       knob_top,
                                       knob_size,
                                       knob_size,
                                       knob_size / 2);
            }
            else /* ER_ARC_KNOB_IMAGE */
            {
                ERImageProps image_props;
                memset(&image_props, 0, sizeof(image_props));
                memcpy(image_props.image_name, props->image_name, sizeof(image_props.image_name));
                image_props.resize_mode = ER_RESIZE_CONTAIN;
                er_image_render(&image_props, knob_left, knob_top, knob_size, knob_size, NULL);
            }
        }
    }

    node->arc_painted_value = node->arc_value;
}

/**
 * @brief Writes ONE end of the arc and records the damage that move implies.
 *
 * Assignment only — the caller has already resolved every constraint, so this never moves the other end
 * and never re-enters. Keeping the write separate from the constraint solving is what makes the push in
 * arc_apply_end() terminate: an earlier version pushed by calling itself, and because the slot was not
 * assigned until the recursion unwound, each level re-read the stale endpoint and bounced back — a
 * min_span wide enough to move BOTH ends recursed until the stack ran out.
 *
 * @param[in,out] node   Arc node.
 * @param[in]     low    true for the RANGE band's low end (arc_value_start), false for the value end.
 * @param[in]     value  Final value for that end.
 *
 * @return true when the stored value actually changed.
 */
static bool arc_set_end(ERNode* node, bool low, float value)
{
    float* const slot = low ? &node->arc_value_start : &node->arc_value;
    const float old_value = *slot;
    if (value == old_value)
        return false;
    *slot = value;

    const int width = (int)node->computed.width, height = (int)node->computed.height;
    if (width <= 0 || height <= 0)
        return true; /* not laid out yet: the first paint covers everything */
    ERArcGeom geometry;
    er_arc_geom(node, 0, 0, width, height, &geometry);

    /* Damage: normally just the sub-arc this end swept (padded for the AA fringe and a round cap), plus
     * that end's knob in its old and new spots. Both radii of the WIDEST ring, so a band under the
     * indicator is repainted where the track/indicator fringe lands on it. The other end is damaged by
     * its own arc_set_end call when a push moves it too. */
    float old_angle = er_arc_value_angle(&geometry, old_value);
    float new_angle = er_arc_value_angle(&geometry, value);
    float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
    const float fringe = ARC_DAMAGE_PAD + ((node->props.arc.cap == ER_ARC_CAP_ROUND) ? geometry.width * 0.5f : 0.0f);

    /* EXCEPTION: a BAND-anchored conic ramp moves with the indicator, so shifting either end re-maps the
     * colour of every pixel between them — the swept sliver is NOT all that changed. Widen to cover the
     * old band and the new band (they share the end that did not move, so their union runs from it to
     * whichever of the moved end's two positions is further). Without this the rest of the band keeps the
     * previous frame's colours until something forces a full repaint, which on a panel reads as a
     * gradient that only catches up when the finger lifts. A solid, radial, or SWEEP-anchored paint
     * re-maps nothing, so those keep the tight sliver. */
    if (arc_grad_band_anchored(&node->props.arc))
    {
        /* The other end. On a single-ended arc the indicator starts at the sweep start, which never
         * moves. When a push moves BOTH ends, each call reads the other's already-final value, so the
         * two damage rects together still cover the old band and the new one. */
        const float fixed_angle = node->props.arc.range
                                      ? er_arc_value_angle(&geometry, low ? node->arc_value : node->arc_value_start)
                                      : geometry.start_angle;
        /* Span the extremes of all three angles, so this is right whichever direction the end moved. */
        float low_angle = old_angle, high_angle = old_angle;
        if (new_angle < low_angle)
            low_angle = new_angle;
        if (new_angle > high_angle)
            high_angle = new_angle;
        if (fixed_angle < low_angle)
            low_angle = fixed_angle;
        if (fixed_angle > high_angle)
            high_angle = fixed_angle;
        old_angle = low_angle;
        new_angle = high_angle;
    }
    er_arc_sector_bbox(geometry.center_x,
                       geometry.center_y,
                       geometry.mid_radius - geometry.half_extent,
                       geometry.mid_radius + geometry.half_extent,
                       old_angle,
                       new_angle,
                       fringe,
                       &left,
                       &top,
                       &right,
                       &bottom);
    if (geometry.knob_size > 0)
    {
        const float knob_radius = (float)geometry.knob_size * 0.5f + ARC_DAMAGE_PAD;
        const float end_values[2] = {old_value, value};
        for (int end_index = 0; end_index < 2; end_index++)
        {
            float knob_x, knob_y;
            er_arc_knob_center(&geometry, end_values[end_index], &knob_x, &knob_y);
            if (knob_x - knob_radius < left)
                left = knob_x - knob_radius;
            if (knob_y - knob_radius < top)
                top = knob_y - knob_radius;
            if (knob_x + knob_radius > right)
                right = knob_x + knob_radius;
            if (knob_y + knob_radius > bottom)
                bottom = knob_y + knob_radius;
        }
    }
    union_local_dirty(node, (int)floorf(left), (int)floorf(top), (int)ceilf(right), (int)ceilf(bottom));

    /* An anchored child is a real node at a new position: re-run layout so it is re-anchored and the
     * subtree prune bounds cover its new spot (its move then damages itself through the usual path). */
    if (node->props.arc.knob == ER_ARC_KNOB_CHILD)
        er_request_layout_pass();
    return true;
}

/**
 * @brief Resolves the range constraints for a requested endpoint value, then writes what they allow.
 *
 * RANGE mode: the ends bound each other, so a drag can never invert the band (which would paint a sector
 * running the wrong way round the dial). With no min_span they may meet and a drag stops at its
 * neighbour. With one set, the pair keeps that separation and a drag that closes the gap PUSHES the far
 * end along rather than stopping dead — what a dual-setpoint control wants (a thermostat's minimum
 * heat/cool spread), and what its +/- steppers already do.
 *
 * Both final values are computed here and written by arc_set_end() below, so at most two writes happen
 * and neither can trigger another: no recursion, and nothing reads a half-updated endpoint.
 *
 * @param[in,out] node   Arc node.
 * @param[in]     low    true to move the band's low end (arc_value_start), false for the value end.
 * @param[in]     value  Requested value; clamped to the range and to what min_span leaves free.
 *
 * @return true when either end actually changed.
 */
static bool arc_apply_end(ERNode* node, bool low, float value)
{
    if (!node || node->type != ER_NODE_ARC)
        return false;
    ERArcGeom geometry;
    er_arc_geom(node, 0, 0, (int)node->computed.width, (int)node->computed.height, &geometry);
    value = clampf(value, geometry.min, geometry.max);

    bool changed = false;
    if (node->props.arc.range)
    {
        /* A min_span wider than the range itself cannot be honoured — clamping it here (rather than
         * letting the endpoint arithmetic below run with it) is what keeps both ends inside [min, max].
         * Left unclamped, a min_span of 200 on a 0..100 arc stored 200 in the high end and -100 in the
         * low one, which then reached onChange and the animation driver as out-of-range values. */
        float min_span = node->props.arc.min_span;
        if (min_span < 0.0f)
            min_span = 0.0f;
        const float span = geometry.max - geometry.min;
        if (min_span > span)
            min_span = span;

        if (min_span > 0.0f)
        {
            /* Neither end may sit closer than min_span to the far end of the RANGE, or there would be no
             * room left for its partner. Push the other end first, so this end's damage sees it settled. */
            if (low)
            {
                if (value > geometry.max - min_span)
                    value = geometry.max - min_span;
                if (node->arc_value < value + min_span)
                    changed |= arc_set_end(node, false, value + min_span);
            }
            else
            {
                if (value < geometry.min + min_span)
                    value = geometry.min + min_span;
                if (node->arc_value_start > value - min_span)
                    changed |= arc_set_end(node, true, value - min_span);
            }
        }
        else if (low && value > node->arc_value)
        {
            value = node->arc_value;
        }
        else if (!low && value < node->arc_value_start)
        {
            value = node->arc_value_start;
        }
    }

    changed |= arc_set_end(node, low, value);
    return changed;
}

bool er_arc_apply_value(ERNode* node, float value)
{
    return arc_apply_end(node, false, value);
}

bool er_arc_apply_value_start(ERNode* node, float value)
{
    return arc_apply_end(node, true, value);
}

bool er_arc_hit(const ERNode* node, int x, int y)
{
    ERArcGeom geometry;
    er_arc_geom(node,
                (int)node->computed.x,
                (int)node->computed.y,
                (int)node->computed.width,
                (int)node->computed.height,
                &geometry);
    const float sample_x = (float)x + 0.5f, sample_y = (float)y + 0.5f;
    const float delta_x = sample_x - geometry.center_x, delta_y = sample_y - geometry.center_y;
    const float distance = sqrtf(delta_x * delta_x + delta_y * delta_y);

    /* Knob first: a big knob is the natural grab point and may reach past the ring. RANGE mode has one
     * per end. */
    if (geometry.knob_size > 0)
    {
        const int ends = node->props.arc.range ? 2 : 1;
        const float knob_radius = (float)geometry.knob_size * 0.5f + ARC_KNOB_HIT_SLOP;
        for (int end_index = 0; end_index < ends; end_index++)
        {
            float knob_x, knob_y;
            er_arc_knob_center(&geometry, (end_index == 0) ? node->arc_value : node->arc_value_start, &knob_x, &knob_y);
            if ((sample_x - knob_x) * (sample_x - knob_x) + (sample_y - knob_y) * (sample_y - knob_y)
                <= knob_radius * knob_radius)
                return true;
        }
    }

    float slop = geometry.half_extent;
    if (geometry.knob_size > 0 && (float)geometry.knob_size * 0.5f > slop)
        slop = (float)geometry.knob_size * 0.5f;
    if (slop < (float)ARC_HIT_SLOP_MIN)
        slop = (float)ARC_HIT_SLOP_MIN;
    if (distance < geometry.mid_radius - geometry.half_extent - slop
        || distance > geometry.mid_radius + geometry.half_extent + slop)
        return false;
    if (geometry.sweep >= 360.0f)
        return true;

    /* Inside the sweep (plus the slop expressed as an angle at this radius)? */
    float relative_angle = atan2f(delta_y, delta_x) * ER_RAD2DEG - geometry.start_angle;
    relative_angle -= 360.0f * floorf(relative_angle / 360.0f);
    const float slop_angle =
        (geometry.mid_radius > 1.0f) ? (asinf(clampf(slop / geometry.mid_radius, 0.0f, 1.0f)) * ER_RAD2DEG) : 180.0f;
    return (relative_angle <= geometry.sweep + slop_angle) || (relative_angle >= 360.0f - slop_angle);
}

float er_arc_value_at(ERNode* node, int x, int y, bool anti_wrap)
{
    ERArcGeom geometry;
    er_arc_geom(node,
                (int)node->computed.x,
                (int)node->computed.y,
                (int)node->computed.width,
                (int)node->computed.height,
                &geometry);
    const float delta_x = (float)x + 0.5f - geometry.center_x, delta_y = (float)y + 0.5f - geometry.center_y;
    float relative_angle = atan2f(delta_y, delta_x) * ER_RAD2DEG - geometry.start_angle;
    relative_angle -= 360.0f * floorf(relative_angle / 360.0f); /* [0, 360) past the start */

    /* A point ON the ring is unambiguous — use it, however far it is from the last sample (a fast finger,
     * or a move coalesced across several frames, legitimately jumps a long way in one dispatch).
     * Only a point in the UNSWEPT GAP is ambiguous, and there the answer is not the geometrically nearer
     * end but the end the finger came FROM: sliding off the max end and on round the gap must hold at max,
     * not snap to min once past the halfway point. A fresh touch has no history, so it takes the nearer. */
    float fraction;
    if (relative_angle <= geometry.sweep)
        fraction = relative_angle / geometry.sweep;
    else if (anti_wrap)
        fraction = (node->arc_drag_fraction > 0.5f) ? 1.0f : 0.0f;
    else
        fraction = ((relative_angle - geometry.sweep) < (360.0f - relative_angle)) ? 1.0f : 0.0f;
    node->arc_drag_fraction = fraction;

    float value = geometry.min + fraction * (geometry.max - geometry.min);
    const float step = (node->props.arc.step > 0.0f) ? node->props.arc.step : ARC_DEFAULT_STEP;
    value = geometry.min + floorf((value - geometry.min) / step + 0.5f) * step;
    return clampf(value, geometry.min, geometry.max);
}

bool er_arc_grab_low(const ERNode* node, int x, int y)
{
    if (!node || node->type != ER_NODE_ARC || !node->props.arc.range)
        return false;
    ERArcGeom geometry;
    er_arc_geom(node,
                (int)node->computed.x,
                (int)node->computed.y,
                (int)node->computed.width,
                (int)node->computed.height,
                &geometry);
    /* Compare in SWEEP FRACTION rather than value units so the choice is the visually nearer knob
     * regardless of the range's scale. */
    const float delta_x = (float)x + 0.5f - geometry.center_x, delta_y = (float)y + 0.5f - geometry.center_y;
    float relative_angle = atan2f(delta_y, delta_x) * ER_RAD2DEG - geometry.start_angle;
    relative_angle -= 360.0f * floorf(relative_angle / 360.0f);
    const float fraction = (relative_angle <= geometry.sweep)
                               ? (relative_angle / geometry.sweep)
                               : (((relative_angle - geometry.sweep) < (360.0f - relative_angle)) ? 1.0f : 0.0f);
    const float high_fraction = er_arc_value_frac(&geometry, node->arc_value);
    const float low_fraction = er_arc_value_frac(&geometry, node->arc_value_start);
    return fabsf(fraction - low_fraction) < fabsf(fraction - high_fraction);
}

/**
 * @brief Shifts a whole subtree by (shift_x, shift_y) — the knob child and everything laid out under it.
 *
 * Walks pre-order through the parent/sibling links rather than an explicit stack, so it has no depth or
 * breadth cap. An earlier version pushed pending children onto a fixed 64-slot array and simply stopped
 * pushing once it filled: a knob subtree bigger than that was left half-moved, split between the value's
 * old and new positions.
 *
 * @param[in,out] root     Subtree root (moved along with its descendants).
 * @param[in]     shift_x  X delta in pixels.
 * @param[in]     shift_y  Y delta in pixels.
 */
static void arc_shift_subtree(ERNode* root, int16_t shift_x, int16_t shift_y)
{
    ERNode* current = root;
    while (current)
    {
        current->computed.x = (int16_t)(current->computed.x + shift_x);
        current->computed.y = (int16_t)(current->computed.y + shift_y);
        current->animated.x = (int16_t)(current->animated.x + shift_x);
        current->animated.y = (int16_t)(current->animated.y + shift_y);

        ERNode* next = er_get_node(current->first_child_tag);
        if (next)
        {
            current = next;
            continue;
        }
        /* Leaf: move to the next sibling, climbing out of finished branches — but never past the root,
         * whose own siblings are not part of this subtree. */
        while (current != root)
        {
            if (er_get_node(current->next_sibling_tag))
                break;
            current = er_get_node(current->parent_tag);
            if (!current)
                return; /* broken parent link: stop rather than walk off the tree */
        }
        if (current == root)
            return;
        current = er_get_node(current->next_sibling_tag);
    }
}

void er_arc_anchor_child(ERNode* node)
{
    if (!node || node->type != ER_NODE_ARC || node->props.arc.knob != ER_ARC_KNOB_CHILD)
        return;
    ERNode* child = knob_child(node);
    if (!child)
        return;
    ERArcGeom geometry;
    er_arc_geom(node,
                (int)node->computed.x,
                (int)node->computed.y,
                (int)node->computed.width,
                (int)node->computed.height,
                &geometry);
    float knob_x, knob_y;
    er_arc_knob_center(&geometry, node->arc_value, &knob_x, &knob_y);
    const int16_t new_x = (int16_t)floorf(knob_x - (float)child->computed.width * 0.5f + 0.5f);
    const int16_t new_y = (int16_t)floorf(knob_y - (float)child->computed.height * 0.5f + 0.5f);
    const int16_t shift_x = (int16_t)(new_x - child->computed.x);
    const int16_t shift_y = (int16_t)(new_y - child->computed.y);
    if (shift_x == 0 && shift_y == 0)
        return;
    arc_shift_subtree(child, shift_x, shift_y);
}
