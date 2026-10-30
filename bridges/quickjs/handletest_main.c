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
 * Node handle test: a handle whose node was destroyed does nothing, even once its slot holds another node.
 * Needs the parser (evaluates JS source).
 */

#include "er_node_internal.h"
#include "er_runtime.h"
#include "er_scene.h"
#include "host_harness.h"
#include "native_renderer.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_failed = 0;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL: %s\n", (message));                                                                           \
            s_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool load(const char* source)
{
    const bool loaded = er_runtime_load_source(source, strlen(source), "App.js");
    if (!loaded)
    {
        printf("load failed: %s\n", er_runtime_last_error());
    }
    return loaded;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_stale_handle_misses_the_new_node(void)
{
    const char* source = "const root = NativeUI.createNode('View');\n"
                         "NativeUI.setRoot(root);\n"
                         "const stale = NativeUI.createNode('View');\n"
                         "NativeUI.destroyNode(stale);\n"
                         "const live = NativeUI.createNode('View');\n"
                         "if (live === stale) throw new Error('the slot gave out the same handle again');\n"
                         "NativeUI.appendChild(root, live);\n"
                         "NativeUI.appendChild(live, NativeUI.createNode('View'));\n"
                         "NativeUI.appendChild(stale, NativeUI.createNode('View'));\n"
                         "NativeUI.appendChild(root, NativeUI.createNode('View'));\n"
                         "NativeUI.insertBefore(root, live, stale);\n"
                         "NativeUI.removeChild(root, stale);\n"
                         "NativeUI.destroyNode(stale);\n";
    CHECK(load(source), "the app loads");
    CHECK(er_node_in_use_count() == 5, "destroying by a stale handle doesn't destroy the slot's new node");

    /* Only the new node has a child, which tells it from its sibling. */
    const ERNode* first = er_node_first_child(er_get_root_node());
    const ERNode* first_child = er_node_first_child(first);
    CHECK(first_child != NULL, "inserting before a stale handle doesn't move the new node to the end");
    CHECK(er_node_next_sibling(first_child) == NULL, "appending to a stale handle doesn't reach the new node");
    CHECK(er_node_next_sibling(first) != NULL, "removing a stale handle doesn't remove the new node");
}

static void test_handles_stay_valid_as_generations_wrap(void)
{
    const char* source = "const root = NativeUI.createNode('View');\n"
                         "NativeUI.setRoot(root);\n"
                         "let handle = 0;\n"
                         "for (let round = 0; round < 70000; round++) {\n"
                         "  handle = NativeUI.createNode('View');\n"
                         "  if (!(handle > 0) || !Number.isInteger(handle)) throw new Error('bad handle ' + handle);\n"
                         "  NativeUI.destroyNode(handle);\n"
                         "}\n"
                         "handle = NativeUI.createNode('View');\n"
                         "NativeUI.appendChild(root, handle);\n";
    CHECK(load(source), "the app loads, with every handle a positive integer");
    CHECK(er_node_in_use_count() == 2, "the handles still reach their nodes");
    CHECK(er_node_first_child(er_get_root_node()) != NULL, "and the last one is in the tree");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    static const EmbeddedRenderBackend backend = ER_HOST_NOOP_BACKEND;
    embedded_renderer_set_backend(&backend);
    ErRuntimeConfig config = {0};
    config.screen_width = 320;
    config.screen_height = 240;
    config.screen_scale = 1.0f;
    if (!er_runtime_init(&config))
    {
        printf("FAIL: er_runtime_init\n");
        return 1;
    }

    test_stale_handle_misses_the_new_node();
    CHECK(er_runtime_reset(), "the runtime resets");
    test_handles_stay_valid_as_generations_wrap();

    er_runtime_shutdown();
    if (s_failed)
    {
        return 1;
    }
    printf("node handles: all tests passed\n");
    return 0;
}
