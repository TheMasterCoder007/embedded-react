#include "er_scene.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Smoke test — verifies that er_commit() can be called without crashing.
 *
 * @return 0 on success.
 */
int main(void)
{
    er_commit();
    return 0;
}
