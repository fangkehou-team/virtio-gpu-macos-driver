/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#ifndef VMTL_SAMPLER_DESC_H
#define VMTL_SAMPLER_DESC_H
#include <stdint.h>
typedef struct {
    uint32_t min_filter, mag_filter, mip_filter;
    uint32_t address_s, address_t, address_r;
    uint32_t compare_function, max_anisotropy, border_color;
    float lod_min, lod_max;
} vmtl_sampler_desc;
#endif
