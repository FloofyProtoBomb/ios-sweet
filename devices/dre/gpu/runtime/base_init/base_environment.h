/* SPDX-License-Identifier: MIT
 * Narrow selected306 construction profile. Does not initialize private state.
 */
#ifndef DRE_METAL306_BASE_ENVIRONMENT_H
#define DRE_METAL306_BASE_ENVIRONMENT_H
#include <stdlib.h>
/* The captured real base helper tests pointer presence, so even an empty
 * variable selects its dump branch. Those inherited writer paths are not
 * qualified by this port. This checkpoint does not lock process-wide setenv.
 */
static inline int dre_metal306_base_environment_allowed(void)
{
    return getenv("MTL_DUMP_PIPELINES_TO_JSON_FILE")==NULL&&
        getenv("MTL_DUMP_LIBRARIES_TO_DIRECTORY")==NULL;
}
#endif
