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

/* Initialize /proc/isp_patch interface */
int isp_patch_init(void);

/* Remove /proc/isp_patch */
void isp_patch_cleanup(void);

/*
 * Patch a host1x gather buffer in-place.
 * Parses opcodes, finds target methods, replaces values.
 * Returns number of patches applied.
 *
 * @buf:   pointer to gather data (writable mapped memory)
 * @words: number of u32 words in gather
 */
int isp_patch_gather(u32 *buf, int words);

#endif /* __ISP_PATCH_H__ */
