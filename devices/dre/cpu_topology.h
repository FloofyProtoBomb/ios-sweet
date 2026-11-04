/* SPDX-License-Identifier: MIT */
#pragma once
/* Validate the eight-core Holi PSCI layout before exposing secondary CPUs. */
const char *dre_cpu_topology(const void *dt, unsigned requested);
