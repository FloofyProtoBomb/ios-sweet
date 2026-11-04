#pragma once
#include <stdint.h>

enum { DRE_UFS_CLOCKS = 4 };
struct dre_ufs_clock_state {
    uint32_t before[DRE_UFS_CLOCKS];
    uint32_t after[DRE_UFS_CLOCKS];
    uint32_t axi_cmd, axi_cfg, unipro_cmd, unipro_cfg;
};

/* GCC branch enables only. Caller verifies the Holi DT and powered domain.
 * With enable=0 this performs reads only. A failed enable rolls back all
 * branch-enable bits changed by this function. Clock rates stay as in ABL. */
const char *dre_ufs_clocks(struct dre_ufs_clock_state *state, int enable);
