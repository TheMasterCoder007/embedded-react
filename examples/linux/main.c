/* Must be defined before any SDL header to disable SDL's main() macro on Windows. */
#define SDL_MAIN_HANDLED

#include "er_scene.h"
#include "native_renderer.h"
#include "sdl_backend.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SCREEN_W 480
#define SCREEN_H 320

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static float s_dpi_scale = 1.0f;
static ERNode* s_action_card = NULL;
static ERNode* s_action_label = NULL;
static bool s_action_enabled = false;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Converts a density-independent pixel value to physical pixels.
 *
 * Multiplies v by the DPI scale factor set during backend initialisation.
 * Returns v unchanged on displays where dpi_scale == 1.0 (1× / 96 dpi).
 *
 * @param[in] v Value in density-independent pixels.
 *
 * @return Equivalent value in physical pixels.
 */
static int16_t dp(int v)
{
    return (int16_t)(v * s_dpi_scale + 0.5f);
}

/**
 * @brief Converts an SDL window coordinate to physical framebuffer pixels.
 *
 * @param[in] v Window coordinate in SDL logical pixels.
 *
 * @return Equivalent framebuffer coordinate.
 */
static int event_px(int v)
{
    return (int)(v * s_dpi_scale + 0.5f);
}

/**
 * @brief Returns an ERProps struct with all layout fields set to ER_LAYOUT_AUTO.
 *
 * All int16_t layout fields default to ER_LAYOUT_AUTO (not specified). Visual and
 * text fields default to zero (transparent, no text). Set only the fields you need
 * on the returned struct before passing it to er_node_set_props().
 *
 * @return ERProps with all layout fields initialized to ER_LAYOUT_AUTO.
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
 * @brief Applies the current interactive card state to the scene graph.
 */
static void update_action_card(void)
{
    ERProps p;

    if (s_action_card)
    {
        p = props_default();
        p.height = dp(90);
        p.margin_top = dp(12);
        p.background_color = s_action_enabled ? 0xFF2A9D8F : 0xFFE94560;
        p.border_radius = dp(10);
        p.padding = dp(14);
        p.align_items = ER_ALIGN_STRETCH;
        p.justify_content = ER_JUSTIFY_CENTER;
        er_node_set_props(s_action_card, &p);
    }

    if (s_action_label)
    {
        p = props_default();
        p.color = 0xFFFFFFFF;
        p.font_size = (uint8_t)dp(16);
        strncpy(p.text,
                s_action_enabled
                    ? "Hit-testing active  --  click to toggle off"
                    : "Click me: hit-testing + press events",
                ER_TEXT_MAX);
        er_node_set_props(s_action_label, &p);
    }
}

/**
 * @brief Toggles the SDL demo action card when it receives a press event.
 *
 * @param[in] node       Node that received the press.
 * @param[in] data       Event payload.
 * @param[in] user_data  Opaque callback context.
 */
static void on_action_press(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_action_enabled = !s_action_enabled;
    update_action_card();
}

/**
 * @brief Builds the demo scene graph and sets it as the render root.
 *
 * Creates a simple UI demonstrating flexbox layout, rounded rectangles, and text
 * rendering via the pure-C scene graph API (er_scene.h). No React or JS is involved —
 * this is the C-driver validation path for the rendering stack.
 *
 * All layout values are expressed in density-independent pixels via dp() so the
 * scene scales correctly on HiDPI displays without changing any constants.
 *
 * @param[in] phys_w Physical framebuffer width in pixels.
 * @param[in] phys_h Physical framebuffer height in pixels.
 */
static void build_scene(int phys_w, int phys_h)
{
    ERProps p;

    /* Root: full-screen dark background, flex column, 20 dp padding. */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = (int16_t)phys_w;
    p.height = (int16_t)phys_h;
    p.background_color = 0xFF1A1A2E;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_STRETCH;
    p.padding = dp(20);
    er_node_set_props(root, &p);

    /* Title text. */
    ERNode* title = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.height = dp(36);
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(24);
    strncpy(p.text, "embedded-react", ER_TEXT_MAX);
    er_node_set_props(title, &p);

    /* Card 1: dark blue, rounded, holds a descriptive label. */
    ERNode* card1 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(90);
    p.margin_top = dp(16);
    p.background_color = 0xFF16213E;
    p.border_radius = dp(10);
    p.padding = dp(14);
    p.align_items = ER_ALIGN_STRETCH;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card1, &p);

    ERNode* card1_label = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(16);
    strncpy(p.text, "Scene graph  *  Yoga flexbox  *  Rounded rects", ER_TEXT_MAX);
    er_node_set_props(card1_label, &p);

    /* Card 2: interactive pressable demonstrating hit-testing and event dispatch. */
    ERNode* card2 = er_node_create(ER_NODE_PRESSABLE);
    p = props_default();
    p.height = dp(90);
    p.margin_top = dp(12);
    p.background_color = 0xFFE94560;
    p.border_radius = dp(10);
    p.padding = dp(14);
    p.align_items = ER_ALIGN_STRETCH;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card2, &p);
    er_event_set(card2, ER_EVENT_PRESS, on_action_press, NULL);

    ERNode* card2_label = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(16);
    strncpy(p.text, "Click me: hit-testing + press events", ER_TEXT_MAX);
    er_node_set_props(card2_label, &p);
    s_action_card = card2;
    s_action_label = card2_label;

    /* Assemble the tree. */
    er_tree_append_child(card1, card1_label);
    er_tree_append_child(card2, card2_label);
    er_tree_append_child(root, title);
    er_tree_append_child(root, card1);
    er_tree_append_child(root, card2);
    er_tree_set_root(root);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Application entry point.
 *
 * Creates an SDL2 window and renderer, initializes the embedded-react SDL2
 * backend, builds a demo scene, then runs the render loop until the window is
 * closed or ESC is pressed. Cleans up SDL and backend resources on exit.
 *
 * @return 0 on clean exit, 1 if SDL or backend initialization fails.
 */
int main(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("embedded-react",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          SCREEN_W,
                                          SCREEN_H,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int phys_w = SCREEN_W, phys_h = SCREEN_H;
    SDL_GetRendererOutputSize(renderer, &phys_w, &phys_h);
    s_dpi_scale = (float)phys_w / (float)SCREEN_W;

    if (!er_sdl_backend_init(renderer, phys_w, phys_h))
    {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    build_scene(phys_w, phys_h);

    bool running = true;
    uint32_t prev = SDL_GetTicks();

    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
                running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running = false;
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
                embedded_renderer_touch(0, ER_TOUCH_DOWN, event_px(ev.button.x), event_px(ev.button.y));
            if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
                embedded_renderer_touch(0, ER_TOUCH_UP, event_px(ev.button.x), event_px(ev.button.y));
            if (ev.type == SDL_MOUSEMOTION && (ev.motion.state & SDL_BUTTON_LMASK))
                embedded_renderer_touch(0, ER_TOUCH_MOVE, event_px(ev.motion.x), event_px(ev.motion.y));
        }

        er_commit();
        er_sdl_present();

        uint32_t now = SDL_GetTicks();
        embedded_renderer_tick(now - prev);
        prev = now;
    }

    er_sdl_backend_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
