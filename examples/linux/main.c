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
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Returns an ERProps struct with all layout fields set to ER_LAYOUT_AUTO.
 *
 * All int16_t layout fields default to ER_LAYOUT_AUTO (not specified). Visual and
 * text fields default to zero (transparent, no text). Set only the fields you need
 * on the returned struct before passing it to er_node_set_props().
 *
 * @return ERProps with all layout fields initialized to ER_LAYOUT_AUTO.
 */
static ERProps props_default(void) {
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
 * @brief Builds the demo scene graph and sets it as the render root.
 *
 * Creates a simple UI demonstrating flexbox layout, rounded rectangles, and text
 * rendering via the pure-C scene graph API (er_scene.h). No React or JS is involved —
 * this is the C-driver validation path for the rendering stack.
 */
static void build_scene(void) {
    ERProps p;

    /* Root: full-screen dark background, flex column, 20 px padding. */
    ERNode *root = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = SCREEN_W;
    p.height = SCREEN_H;
    p.background_color = 0xFF1A1A2E;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_STRETCH;
    p.padding = 20;
    er_node_set_props(root, &p);

    /* Title text. */
    ERNode *title = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.height = 36;
    p.color = 0xFFFFFFFF;
    p.font_size = 24;
    strncpy(p.text, "embedded-react", ER_TEXT_MAX);
    er_node_set_props(title, &p);

    /* Card 1: dark blue, rounded, holds a descriptive label. */
    ERNode *card1 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = 90;
    p.margin_top = 16;
    p.background_color = 0xFF16213E;
    p.border_radius = 10;
    p.padding = 14;
    p.align_items = ER_ALIGN_STRETCH;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card1, &p);

    ERNode *card1_label = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = 16;
    strncpy(p.text, "Scene graph  *  Yoga flexbox  *  Rounded rects", ER_TEXT_MAX);
    er_node_set_props(card1_label, &p);

    /* Card 2: red accent, rounded, holds the quit hint. */
    ERNode *card2 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = 90;
    p.margin_top = 12;
    p.background_color = 0xFFE94560;
    p.border_radius = 10;
    p.padding = 14;
    p.align_items = ER_ALIGN_STRETCH;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card2, &p);

    ERNode *card2_label = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = 16;
    strncpy(p.text, "SDL2 backend active  --  press ESC to quit", ER_TEXT_MAX);
    er_node_set_props(card2_label, &p);

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
int main(void) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "embedded-react",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H,
        SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!er_sdl_backend_init(renderer, SCREEN_W, SCREEN_H)) {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    build_scene();

    bool running = true;
    uint32_t prev = SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running = false;
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
