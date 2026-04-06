/*
 * ISP Trace — persistent categorized ring buffer for ISP debug tracing
 *
 * 16MB reserved physical memory region, survives reboot.
 * Read via /proc/isp_trace/ directory — one file per category.
 *
 * Copyright (c) 2026, Artem Bambalov. All rights reserved.
 */

#ifndef __ISP_TRACE_H__
#define __ISP_TRACE_H__

#include <linux/types.h>

#define ISP_TRACE_BUF_SIZE	(64 * 1024 * 1024)	/* 64MB */
#define ISP_TRACE_MAGIC		0x49535055		/* 'ISPU' v2 */

/* Trace categories — stored as prefix in each line */
enum isp_trace_cat {
	ISP_CAT_NVMAP_CREATE = 0,	/* nvmap CREATE/FROM_FD */
	ISP_CAT_NVMAP_ALLOC,		/* nvmap ALLOC (heap, flags, align) */
	ISP_CAT_NVMAP_PIN,		/* nvmap PIN/UNPIN → IOVA */
	ISP_CAT_NVMAP_CACHE,		/* nvmap cache maintenance */
	ISP_CAT_NVMAP_OTHER,		/* nvmap MMAP, GET_PARAM, FREE, GET_FD */
	ISP_CAT_CHANNEL,		/* channel open/close */
	ISP_CAT_CHANNEL_IOCTL,		/* SET_NVMAP_FD, SET_CLK_RATE, etc */
	ISP_CAT_SUBMIT,			/* submit meta + fence result */
	ISP_CAT_GATHER,			/* gather hex dumps */
	ISP_CAT_CDMA,			/* push buffer words */
	ISP_CAT_PIO,			/* PIO register R/W */
	ISP_CAT_SYNCPT,			/* syncpoint read/wait/incr */
	ISP_CAT_ISR,			/* ISP interrupt status */
	ISP_CAT_POWER,			/* busy/idle/emc */
	ISP_CAT_REGDUMP,		/* ISP MMIO register dumps */
	ISP_CAT_SMMU,			/* SMMU IOVA→phys, domain info */
	ISP_CAT_VI_SUBMIT,		/* VI submit meta + fence result */
	ISP_CAT_VI_GATHER,		/* VI gather hex dumps */
	ISP_CAT_MAX
};

/* Short category prefixes for text lines — defined in isp_trace.c */
extern const char * const isp_trace_cat_prefix[ISP_CAT_MAX];

/* Proc file names under /proc/isp_trace/ — defined in isp_trace.c */
extern const char * const isp_trace_cat_name[ISP_CAT_MAX];

/* Ring buffer header — lives at start of reserved region */
struct isp_trace_header {
	u32 magic;
	u32 write_pos;		/* byte offset after header */
	u32 wrap_count;
	u32 data_size;		/* total data area size */
	u32 entry_count;
	u32 reserved[3];
};

#define ISP_TRACE_DATA_SIZE	(ISP_TRACE_BUF_SIZE - sizeof(struct isp_trace_header))

/* Init/cleanup */
void isp_trace_set_phys(phys_addr_t phys, unsigned long size);
int isp_trace_init(void);
void isp_trace_cleanup(void);

/* Categorized logging — safe from any context */
void isp_trace_cat(enum isp_trace_cat cat, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Categorized hex dump */
void isp_trace_cat_hex(enum isp_trace_cat cat, const char *tag,
		       const u32 *data, int words);

/* Legacy API — maps to ISP_CAT_SUBMIT for backward compat */
static inline void isp_trace_log(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
static inline void isp_trace_log(const char *fmt, ...)
{
	/* Stub — callers should migrate to isp_trace_cat() */
}

static inline void isp_trace_hex(const char *tag, const u32 *data, int words)
{
	/* Stub — callers should migrate to isp_trace_cat_hex() */
}

#endif /* __ISP_TRACE_H__ */
