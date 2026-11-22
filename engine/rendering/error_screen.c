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
 * The error screen's text, its layout and its drawing. er_commit() calls er_error_screen_draw() while the screen
 * is up. The fault record it shows is filled by er_fault_record.c.
 */

#include "er_error_screen.h"
#include "renderer_internal.h"
#include "text_renderer.h"

#include <stdarg.h>
#include <stddef.h>
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

/* What a recorded fault shows. */
#define FAULT_TITLE "Device restarted"
#define FAULT_HINT "A firmware or engine fault, not the app's."
#define FAULT_DETAIL_BYTES 48u

/* A UTF-8 continuation byte, which a cut must not land before. */
#define UTF8_CONTINUATION_MASK 0xC0u
#define UTF8_CONTINUATION_BITS 0x80u

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
static int s_screen_width;
static int s_screen_height;
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
        while (length > 0u && ((unsigned char)source[length] & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_BITS)
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
    int text_width = 0;
    int text_height = 0;
    er_text_measure("", params.font_size, NULL, 0, params.font_weight, &text_width, &text_height);
    return wrapped_lines * text_height;
}

/**
 * @brief Appends formatted text to a line.
 *
 * @return The line's new length.
 */
static size_t fault_append(char* buffer, size_t capacity, size_t used, const char* format, ...)
{
    if (used + 1u >= capacity)
    {
        return used;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(buffer + used, capacity - used, format, arguments);
    va_end(arguments);
    if (written < 0)
    {
        return used;
    }
    return (size_t)written < capacity - used ? used + (size_t)written : capacity - 1u;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Internal
 ---------------------------------------------------------------------------------------------------------------------*/

void er_error_screen_size(int* screen_width, int* screen_height)
{
    *screen_width = s_screen_width;
    *screen_height = s_screen_height;
}

void er_error_screen_draw(void)
{
    const int text_width = s_screen_width - 2 * ERROR_SCREEN_PADDING;
    er_push_clip_rect(0, 0, s_screen_width, s_screen_height);
    er_blit_fill(ERROR_SCREEN_BACKGROUND, 0, 0, s_screen_width, s_screen_height);

    /* Centred when it all fits, else from the top, with the detail cut at the bottom edge. */
    int heights[LINE_COUNT];
    int total_height = 0;
    for (int line = 0; line < LINE_COUNT; line++)
    {
        heights[line] = line_text(line)[0] ? line_height(line, text_width) : 0;
        total_height += heights[line] ? heights[line] + (total_height ? ERROR_SCREEN_GAP : 0) : 0;
    }
    const int room_height = s_screen_height - 2 * ERROR_SCREEN_PADDING;
    int top = ERROR_SCREEN_PADDING + (total_height < room_height ? (room_height - total_height) / 2 : 0);
    const int bottom = s_screen_height - ERROR_SCREEN_PADDING;

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
        params.clip.width = text_width;
        params.clip.height = top + heights[line] < bottom ? heights[line] : bottom - top;
        er_text_render(&params);
        top += heights[line] + ERROR_SCREEN_GAP;
    }
    er_pop_clip_rect();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_error_screen_show(int screen_width, int screen_height, const ERErrorScreen* screen)
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
    s_screen_width = screen_width > 0 ? screen_width : 0;
    s_screen_height = screen_height > 0 ? screen_height : 0;
    s_active = true;
    /* A gesture in progress, and any move parked for it, ends here, so none reaches an app after it. */
    er_input_reset();
    er_force_full_repaint();
}

void er_error_screen_resize(int screen_width, int screen_height)
{
    if (s_active)
    {
        s_screen_width = screen_width > 0 ? screen_width : 0;
        s_screen_height = screen_height > 0 ? screen_height : 0;
        er_force_full_repaint();
    }
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

bool er_fault_show_recorded(ERFaultRecord* record, int screen_width, int screen_height)
{
    if (!er_fault_recorded(record))
    {
        return false;
    }
    /* The app's line beside the fault address, or the numbers `embedded-react where` takes. */
    char location[ERUI_ERROR_SCREEN_LINE_BYTES] = "";
    size_t used = 0u;
    if ((record->flags & ER_FAULT_FLAG_APP) && record->app_file[0])
    {
        used =
            fault_append(location, sizeof(location), used, "%s:%lu", record->app_file, (unsigned long)record->app_line);
    }
    else if (record->flags & ER_FAULT_FLAG_APP)
    {
        used = fault_append(location,
                            sizeof(location),
                            used,
                            "function %u at %lu (build %08lx)",
                            (unsigned)record->app_function,
                            (unsigned long)record->app_offset,
                            (unsigned long)record->app_build_id);
    }
    if (record->program_counter)
    {
        used = fault_append(
            location, sizeof(location), used, "%spc 0x%08lx", used ? "  " : "", (unsigned long)record->program_counter);
    }
    char detail[FAULT_DETAIL_BYTES] = "";
    used = 0u;
    if (record->address)
    {
        used = fault_append(detail, sizeof(detail), used, "address 0x%08lx", (unsigned long)record->address);
    }
    if (record->link_register)
    {
        fault_append(
            detail, sizeof(detail), used, "%slr 0x%08lx", used ? "  " : "", (unsigned long)record->link_register);
    }
    const ERErrorScreen screen = {
        .title = FAULT_TITLE,
        .reason = record->reason,
        .location = location,
        .hint = FAULT_HINT,
        .detail = detail,
    };
    er_error_screen_show(screen_width, screen_height, &screen);
    er_fault_clear(record);
    return true;
}
