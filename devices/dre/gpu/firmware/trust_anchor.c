/* SPDX-License-Identifier: MIT -- UNVALIDATED optional private anchor TU.
 * The default has no anchor and refuses firmware; no private values in git. */
#include "package.h"
#ifdef DRE_GPU_FW_PRIVATE_TRUST_HEADER
#include DRE_GPU_FW_PRIVATE_TRUST_HEADER
static const struct dre_gpu_fw_trust private_anchor __attribute__((used))=
    DRE_GPU_FW_TRUST_INITIALIZER;
const struct dre_gpu_fw_trust *dre_gpu_fw_trusted_anchor(void)
{ return &private_anchor; }
#else
const struct dre_gpu_fw_trust *dre_gpu_fw_trusted_anchor(void) { return 0; }
#endif
