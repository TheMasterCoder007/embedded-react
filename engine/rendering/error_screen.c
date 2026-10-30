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

/*
 * The error screen's text, its layout and its drawing, and the no-init fault record. er_commit() calls
 * er_error_screen_draw() while the screen is up.
 */

#include "er_error_screen.h"
#include "renderer_internal.h"
#include "text_renderer.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/* A dark red panel, a white title and pale red text. */
#define ERROR_SCREEN_BACKGROUND 0xFF7F1D1Du
#define ERROR_SCREEN_TITLE_COLOR 0xFFFFFFFFu
#define ERROR_SCREEN_TEXT_COLOR 0xFFFECACAu
#define ERROR_SCREEN_DETAIL_COLOR 0xFFFCA5A5u
#define ERROR_SCREEN_PADDING 16
#define ERROR_SCREEN_GAP 8
#define ERROR_SCREEN_TITLE_SIZE 20u
#define ERROR_SCREEN_TEXT_SIZE 14u
#define ERROR_SCREEN_DETAIL_SIZE 12u
#define ERROR_SCREEN_DEFAULT_TITLE "Error"
#define ERROR_SCREEN_FONT_BOLD 1u

/* The fault record's check word. */
#define FAULT_MAGIC 0x45524654u /* "ERFT" */
#define FAULT_HASH_SEED 2166136261u
#define FAULT_HASH_PRIME 16777619u

/* What a recorded fault shows. */
#define FAULT_TITLE "Device restarted"
#define FAULT_HINT "A firmware or engine fault, not the app's."
#define FAULT_HEX_BYTES 32u

/** @brief The screen's lines, in the order they are drawn. */
enum
{
    LINE_TITLE,
    LINE_REASON,
    LINE_AMOUNTS,
    LINE_LOCATION,
    LINE_HINT,
    LINE_DETAIL,
    LINE_COUNT,
};

/** @brief How one line is drawn. */
typedef struct
{
    uint32_t color;
    uint8_t font_size;
    uint8_t font_weight;
} LineStyle;

static const LineStyle s_line_styles[LINE_COUNT] = {
    [LINE_TITLE] = {ERROR_SCREEN_TITLE_COLOR, ERROR_SCREEN_TITLE_SIZE, ERROR_SCREEN_FONT_BOLD},
    [LINE_REASON] = {ERROR_SCREEN_TEXT_COLOR, ERROR_SCREEN_TEXT_SIZE, 0u},
    [LINE_AMOUNTS] = {ERROR_SCREEN_TEXT_COLOR, ERROR_SCREEN_TEXT_SIZE, 0u},
    [LINE_LOCATION] = {ERROR_SCREEN_TEXT_COLOR, ERROR_SCREEN_TEXT_SIZE, 0u},
    [LINE_HINT] = {ERROR_SCREEN_TEXT_COLOR, ERROR_SCREEN_TEXT_SIZE, 0u},
    [LINE_DETAIL] = {ERROR_SCREEN_DETAIL_COLOR, ERROR_SCREEN_DETAIL_SIZE, 0u},
};

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

static bool s_active;
static int s_screen_w;
static int s_screen_h;
static char s_short_lines[LINE_DETAIL][ERUI_ERROR_SCREEN_LINE_BYTES];
static char s_detail[ERUI_ERROR_SCREEN_DETAIL_BYTES];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Copies @p source into @p destination, cut to fit on a UTF-8 character boundary.
 *
 * @param[out] destination  The buffer.
 * @param[in]  capacity     Its size, with room for the NUL.
 * @param[in]  source       The text, or NULL for none.
 */
static void copy_text(char* destination, size_t capacity, const char* source)
{
    size_t length = source ? strlen(source) : 0u;
    if (length >= capacity)
    {
        length = capacity - 1u;
        while (length > 0u && ((unsigned char)source[length] & 0xC0u) == 0x80u)
        {
            length--;
        }
    }
    if (length > 0u)
    {
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

/** @brief The text of line @p line. */
static const char* line_text(int line)
{
    return line == LINE_DETAIL ? s_detail : s_short_lines[line];
}

/** @brief Sets up the text parameters for line @p line, with no clip yet. */
static void line_params(int line, ERTextRenderParams* params)
{
    memset(params, 0, sizeof(*params));
    params->text = line_text(line);
    params->color = s_line_styles[line].color;
    params->font_size = s_line_styles[line].font_size;
    params->font_weight = s_line_styles[line].font_weight;
}

/** @brief How tall line @p line is when wrapped to @p width. */
static int line_height(int line, int width)
{
    ERTextRenderParams params;
    line_params(line, &params);
    int widest = 0;
    int wrapped_lines = 0;
    er_text_measure_lines(&params, width, &widest, &wrapped_lines);
    int text_w = 0;
    int text_h = 0;
    er_text_measure("", params.font_size, NULL, 0, params.font_weight, &text_w, &text_h);
    return wrapped_lines * text_h;
}

/** @brief The check word for a fault record's contents. */
static uint32_t fault_check(const ERFaultRecord* record)
{
    uint32_t hash = FAULT_HASH_SEED ^ record->magic ^ record->pc ^ record->address;
    for (size_t index = 0u; index < sizeof(record->reason); index++)
    {
        hash = (hash ^ (unsigned char)record->reason[index]) * FAULT_HASH_PRIME;
    }
    return hash;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Internal
 ---------------------------------------------------------------------------------------------------------------------*/

void er_error_screen_size(int* screen_w, int* screen_h)
{
    *screen_w = s_screen_w;
    *screen_h = s_screen_h;
}

void er_error_screen_draw(void)
{
    const int text_w = s_screen_w - 2 * ERROR_SCREEN_PADDING;
    er_push_clip_rect(0, 0, s_screen_w, s_screen_h);
    er_blit_fill(ERROR_SCREEN_BACKGROUND, 0, 0, s_screen_w, s_screen_h);

    /* Centred when it all fits, else from the top, with the detail cut at the bottom edge. */
    int heights[LINE_COUNT];
    int total_h = 0;
    for (int line = 0; line < LINE_COUNT; line++)
    {
        heights[line] = line_text(line)[0] ? line_height(line, text_w) : 0;
        total_h += heights[line] ? heights[line] + (total_h ? ERROR_SCREEN_GAP : 0) : 0;
    }
    const int room_h = s_screen_h - 2 * ERROR_SCREEN_PADDING;
    int top = ERROR_SCREEN_PADDING + (total_h < room_h ? (room_h - total_h) / 2 : 0);
    const int bottom = s_screen_h - ERROR_SCREEN_PADDING;

    for (int line = 0; line < LINE_COUNT && top < bottom; line++)
    {
        if (!heights[line])
        {
            continue;
        }
        ERTextRenderParams params;
        line_params(line, &params);
        params.clip.x = ERROR_SCREEN_PADDING;
        params.clip.y = top;
        params.clip.w = text_w;
        params.clip.h = top + heights[line] < bottom ? heights[line] : bottom - top;
        er_text_render(&params);
        top += heights[line] + ERROR_SCREEN_GAP;
    }
    er_pop_clip_rect();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_error_screen_show(int screen_w, int screen_h, const ERErrorScreen* screen)
{
    const ERErrorScreen none = {0};
    if (!screen)
    {
        screen = &none;
    }
    copy_text(s_short_lines[LINE_TITLE],
              sizeof(s_short_lines[LINE_TITLE]),
              screen->title ? screen->title : ERROR_SCREEN_DEFAULT_TITLE);
    copy_text(s_short_lines[LINE_REASON], sizeof(s_short_lines[LINE_REASON]), screen->reason);
    copy_text(s_short_lines[LINE_AMOUNTS], sizeof(s_short_lines[LINE_AMOUNTS]), screen->amounts);
    copy_text(s_short_lines[LINE_LOCATION], sizeof(s_short_lines[LINE_LOCATION]), screen->location);
    copy_text(s_short_lines[LINE_HINT], sizeof(s_short_lines[LINE_HINT]), screen->hint);
    copy_text(s_detail, sizeof(s_detail), screen->detail);
    s_screen_w = screen_w > 0 ? screen_w : 0;
    s_screen_h = screen_h > 0 ? screen_h : 0;
    s_active = true;
    /* A gesture in progress, and any move parked for it, ends here, so none reaches an app after it. */
    er_input_reset();
    er_force_full_repaint();
}

void er_error_screen_hide(void)
{
    if (s_active)
    {
        s_active = false;
        er_force_full_repaint();
    }
}

bool er_error_screen_active(void)
{
    return s_active;
}

void er_fault_record(ERFaultRecord* record, const char* reason, uint32_t pc, uint32_t address)
{
    record->magic = FAULT_MAGIC;
    record->pc = pc;
    record->address = address;
    size_t index = 0u;
    for (; reason && reason[index] && index + 1u < sizeof(record->reason); index++)
    {
        record->reason[index] = reason[index];
    }
    for (; index < sizeof(record->reason); index++)
    {
        record->reason[index] = '\0';
    }
    record->check = fault_check(record);
}

bool er_fault_recorded(const ERFaultRecord* record)
{
    return record->magic == FAULT_MAGIC && record->check == fault_check(record);
}

bool er_fault_show_recorded(ERFaultRecord* record, int screen_w, int screen_h)
{
    if (!er_fault_recorded(record))
    {
        return false;
    }
    char location[FAULT_HEX_BYTES] = "";
    char detail[FAULT_HEX_BYTES] = "";
    if (record->pc)
    {
        snprintf(location, sizeof(location), "pc 0x%08lx", (unsigned long)record->pc);
    }
    if (record->address)
    {
        snprintf(detail, sizeof(detail), "address 0x%08lx", (unsigned long)record->address);
    }
    const ERErrorScreen screen = {
        .title = FAULT_TITLE,
        .reason = record->reason,
        .location = location,
        .hint = FAULT_HINT,
        .detail = detail,
    };
    er_error_screen_show(screen_w, screen_h, &screen);
    record->magic = 0u;
    return true;
}
