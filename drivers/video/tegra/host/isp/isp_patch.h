/*
 * ISP Method Patcher — Header
 *
 * Intercepts ISP host1x command buffers and patches method values
 * before hardware submission. Controlled via /proc/isp_patch.
 *
 * Copyright (c) 2026, Smoke Team. All rights reserved.
 */

#ifndef __ISP_PATCH_H__
#define __ISP_PATCH_H__

#include <linux/types.h>

/* Initialize /proc/isp_patch + /proc/isp_patch_override */
int isp_patch_init(void);

/* Remove proc entries */
void isp_patch_cleanup(void);

/*
 * Patch a host1x gather buffer in-place.
 * Parses opcodes, finds target methods, replaces values.
 * Returns number of patches applied.
 */
int isp_patch_gather(u32 *buf, int words);

/*
 * Check if a gather override is pending for ISP submit #N.
 * If so, copies override data into buf (up to max_words).
 * Returns override word count, or 0 if no override.
 */
int isp_patch_check_override(u32 *buf, int max_words, int gather_idx);

/* Count ISP submits (called from bus_client for each ISP submit) */
void isp_patch_submit_begin(void);

#endif /* __ISP_PATCH_H__ */
