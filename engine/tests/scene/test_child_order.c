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
 * Paint order of a node's children (ERChildCursor): ascending zIndex, append order within one zIndex.
 * Every zIndex sequence up to MAX_KIDS children is walked and compared with a stable sort of the same
 * children, so interleaved levels (the lowest one not first, a level split by others) are all covered.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Includes
 ---------------------------------------------------------------------------------------------------------------------*/

#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define MAX_KIDS 5

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static void null_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief Builds a parent with one child per zIndex in z[], walks it, and checks the order.
 *
 * @param[in] z      zIndex of each child, in append order.
 * @param[in] count  Number of children.
 *
 * @return 0 when the walk matches a stable sort by zIndex, 1 otherwise.
 */
static int check_order(const int16_t* z, int count)
{
    er_reset();
    ERNode* parent = er_node_create(ER_NODE_VIEW);
    ERNode* kids[MAX_KIDS];
    for (int i = 0; i < count; i++)
    {
        kids[i] = er_node_create(ER_NODE_VIEW);
        kids[i]->z_index = z[i];
        er_tree_append_child(parent, kids[i]);
    }

    /* Expected: a stable insertion sort of the append order by zIndex. */
    int expected[MAX_KIDS];
    for (int i = 0; i < count; i++)
    {
        int j = i - 1;
        while (j >= 0 && z[expected[j]] > z[i])
        {
            expected[j + 1] = expected[j];
            j--;
        }
        expected[j + 1] = i;
    }

    ERChildCursor cur;
    int seen = 0;
    for (ERNode* c = er_child_first(&cur, parent); c; c = er_child_next(&cur))
    {
        if (seen >= count || c != kids[expected[seen]])
        {
            break;
        }
        seen++;
    }
    if (seen == count && er_child_next(&cur) == NULL)
        return 0;

    fprintf(stderr, "FAIL: zIndex [");
    for (int i = 0; i < count; i++)
        fprintf(stderr, "%s%d", i ? ", " : "", z[i]);
    fprintf(stderr, "] walked out of order at position %d\n", seen);
    return 1;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    EmbeddedRenderBackend be;
    memset(&be, 0, sizeof(be));
    be.fill_rect = null_fill;
    embedded_renderer_set_backend(&be);

    int failures = 0;

    /* The lowest level is not the first child, and the higher level is split around it. */
    static const int16_t k_split[] = {10, 0, 10};
    failures += check_order(k_split, 3);

    /* Every sequence over three levels, 0 to MAX_KIDS children long. */
    static const int16_t k_levels[] = {-1, 0, 2};
    for (int count = 0; count <= MAX_KIDS; count++)
    {
        int combos = 1;
        for (int i = 0; i < count; i++)
            combos *= 3;
        for (int combo = 0; combo < combos; combo++)
        {
            int16_t z[MAX_KIDS];
            int rest = combo;
            for (int i = 0; i < count; i++)
            {
                z[i] = k_levels[rest % 3];
                rest /= 3;
            }
            failures += check_order(z, count);
        }
    }

    if (failures)
    {
        fprintf(stderr, "%d child-order case(s) FAILED\n", failures);
        return EXIT_FAILURE;
    }
    printf("All child-order tests PASSED\n");
    return EXIT_SUCCESS;
}
