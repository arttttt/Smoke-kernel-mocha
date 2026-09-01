/*
 * ISP Trace — persistent categorized ring buffer for ISP debug tracing
 *
 * Single 16MB ring buffer with category prefixes per line.
 * /proc/isp_trace/all — everything
 * /proc/isp_trace/<category> — filtered view
 * /proc/isp_trace/reset — write "1" to clear
 *
 * Each line: [timestamp_us] CAT pid:comm message\n
 *
 * Copyright (c) 2026, Artem Bambalov. All rights reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "isp_trace.h"

const char * const isp_trace_cat_prefix[ISP_CAT_MAX] = {
	[ISP_CAT_NVMAP_CREATE]	= "NC",
	[ISP_CAT_NVMAP_ALLOC]	= "NA",
	[ISP_CAT_NVMAP_PIN]	= "NP",
	[ISP_CAT_NVMAP_CACHE]	= "NX",
	[ISP_CAT_NVMAP_OTHER]	= "NO",
	[ISP_CAT_CHANNEL]	= "CH",
	[ISP_CAT_CHANNEL_IOCTL]= "CI",
	[ISP_CAT_SUBMIT]	= "SB",
	[ISP_CAT_GATHER]	= "GA",
	[ISP_CAT_CDMA]		= "CD",
	[ISP_CAT_PIO]		= "PI",
	[ISP_CAT_SYNCPT]	= "SP",
	[ISP_CAT_ISR]		= "IR",
	[ISP_CAT_POWER]		= "PW",
	[ISP_CAT_REGDUMP]	= "RG",
	[ISP_CAT_SMMU]		= "SM",
	[ISP_CAT_VI_SUBMIT]	= "VS",
	[ISP_CAT_VI_GATHER]	= "VG",
};
EXPORT_SYMBOL(isp_trace_cat_prefix);

const char * const isp_trace_cat_name[ISP_CAT_MAX] = {
	[ISP_CAT_NVMAP_CREATE]	= "nvmap_create",
	[ISP_CAT_NVMAP_ALLOC]	= "nvmap_alloc",
	[ISP_CAT_NVMAP_PIN]	= "nvmap_pin",
	[ISP_CAT_NVMAP_CACHE]	= "nvmap_cache",
	[ISP_CAT_NVMAP_OTHER]	= "nvmap_other",
	[ISP_CAT_CHANNEL]	= "channel",
	[ISP_CAT_CHANNEL_IOCTL]= "channel_ioctl",
	[ISP_CAT_SUBMIT]	= "submit",
	[ISP_CAT_GATHER]	= "gather",
	[ISP_CAT_CDMA]		= "cdma",
	[ISP_CAT_PIO]		= "pio",
	[ISP_CAT_SYNCPT]	= "syncpt",
	[ISP_CAT_ISR]		= "isr",
	[ISP_CAT_POWER]		= "power",
	[ISP_CAT_REGDUMP]	= "regdump",
	[ISP_CAT_SMMU]		= "smmu",
	[ISP_CAT_VI_SUBMIT]	= "vi_submit",
	[ISP_CAT_VI_GATHER]	= "vi_gather",
};
EXPORT_SYMBOL(isp_trace_cat_name);

static phys_addr_t trace_phys;
static unsigned long trace_size;
static void *trace_base;
static struct isp_trace_header *trace_hdr;
static char *trace_data;
static int trace_is_phys;
static DEFINE_SPINLOCK(trace_lock);

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_all;
static struct proc_dir_entry *proc_reset;
static struct proc_dir_entry *proc_cat[ISP_CAT_MAX];

/* Set physical address — called from board init */
void isp_trace_set_phys(phys_addr_t phys, unsigned long size)
{
	trace_phys = phys;
	trace_size = size;
	pr_info("isp_trace: phys=0x%pa size=%lu\n", &phys, size);
}
EXPORT_SYMBOL(isp_trace_set_phys);

/*
 * Keeping the beginning instead of the end.
 *
 * A ring keeps whatever happened most recently, and for this hardware that
 * is the wrong half. The camera stack writes its entire configuration once,
 * when it opens the device, and then submits a gather per frame for as long
 * as the camera is open. By the time anyone reads the buffer the opening
 * sequence has been overwritten many times over -- and it is the only part
 * that says how the pipeline was set up, which is exactly what we have
 * spent weeks unable to see.
 *
 * So: when armed, the trace records until the buffer is full and then stops,
 * rather than wrapping over its own start.
 */
static int trace_stop_when_full;
static int trace_is_full;

/* Write raw bytes into ring buffer — caller must hold trace_lock */
static void trace_write_raw(const char *buf, int len)
{
	u32 pos, data_size, space;

	if (!trace_data || !trace_hdr)
		return;
	if (trace_is_full)
		return;

	data_size = trace_hdr->data_size;
	pos = trace_hdr->write_pos;

	if (trace_stop_when_full && pos + len > data_size) {
		trace_is_full = 1;
		pr_info("isp_trace: buffer full, recording stopped (%u entries)\n",
			trace_hdr->entry_count);
		return;
	}

	space = data_size - pos;
	if (len <= space) {
		memcpy(trace_data + pos, buf, len);
		pos += len;
		if (pos >= data_size) {
			pos = 0;
			trace_hdr->wrap_count++;
		}
	} else {
		memcpy(trace_data + pos, buf, space);
		memcpy(trace_data, buf + space, len - space);
		pos = len - space;
		trace_hdr->wrap_count++;
	}

	trace_hdr->write_pos = pos;
	trace_hdr->entry_count++;
}

/* Main categorized logging function */
void isp_trace_cat(enum isp_trace_cat cat, const char *fmt, ...)
{
	va_list args;
	char line[512];
	int len;
	unsigned long flags;
	s64 ts;
	const char *prefix;

	/*
	 * Cheapest possible gate, checked before any formatting.  Sites that
	 * only call in here need no guard of their own -- and must not grow
	 * one that swallows real work: in nvmap_dev.c several of these calls
	 * sit in the same block as the ioctl dispatch they trace, so a guard
	 * placed around the block would stop the allocation itself.
	 */
	if (!isp_trace_enabled)
		return;

	if (!trace_data || cat >= ISP_CAT_MAX)
		return;

	prefix = isp_trace_cat_prefix[cat];
	ts = ktime_to_us(ktime_get());

	va_start(args, fmt);
	len = snprintf(line, sizeof(line), "[%lld] %s %d:%s ",
		       ts, prefix, current->pid, current->comm);
	len += vsnprintf(line + len, sizeof(line) - len, fmt, args);
	va_end(args);

	if (len >= sizeof(line))
		len = sizeof(line) - 1;
	if (len > 0 && line[len - 1] != '\n')
		line[len++] = '\n';

	spin_lock_irqsave(&trace_lock, flags);
	trace_write_raw(line, len);
	spin_unlock_irqrestore(&trace_lock, flags);
}
EXPORT_SYMBOL(isp_trace_cat);

/* Categorized hex dump */
void isp_trace_cat_hex(enum isp_trace_cat cat, const char *tag,
		       const u32 *data, int words)
{
	char line[256];
	int i, j, len, remaining;
	unsigned long flags;
	s64 ts;
	const char *prefix;

	/* same gate as isp_trace_cat(): hex dumps are the heavier of the two */
	if (!isp_trace_enabled)
		return;

	if (!trace_data || !data || cat >= ISP_CAT_MAX)
		return;

	prefix = isp_trace_cat_prefix[cat];
	ts = ktime_to_us(ktime_get());

	for (i = 0; i < words; i += 8) {
		remaining = words - i;
		if (remaining > 8)
			remaining = 8;

		len = snprintf(line, sizeof(line),
			       "[%lld] %s %d:%s %s[%d]:",
			       ts, prefix, current->pid, current->comm,
			       tag, i);
		for (j = 0; j < remaining; j++)
			len += snprintf(line + len, sizeof(line) - len,
					" %08x", data[i + j]);
		if (len < sizeof(line) - 1)
			line[len++] = '\n';

		spin_lock_irqsave(&trace_lock, flags);
		trace_write_raw(line, len);
		spin_unlock_irqrestore(&trace_lock, flags);
	}
}
EXPORT_SYMBOL(isp_trace_cat_hex);

/* ===== /proc read helpers ===== */

struct isp_trace_filter {
	int cat;	/* -1 = all, 0..ISP_CAT_MAX-1 = specific */
};

/* Linearize ring buffer content into a temp buffer, optionally filtering */
static ssize_t isp_trace_filtered_read(struct file *file, char __user *buf,
					size_t count, loff_t *ppos)
{
	struct isp_trace_filter *filt = PDE_DATA(file_inode(file));
	u32 pos, data_size, wrap_count, total, start;
	char *tmp;
	ssize_t out_len, ret;
	loff_t off = *ppos;
	u32 ring_pos, i;
	int cat_filter;
	const char *cat_prefix;

	if (!trace_hdr || !trace_data)
		return -ENODEV;

	cat_filter = filt ? filt->cat : -1;

	pos = trace_hdr->write_pos;
	data_size = trace_hdr->data_size;
	wrap_count = trace_hdr->wrap_count;

	total = wrap_count > 0 ? data_size : pos;
	start = wrap_count > 0 ? pos : 0;

	if (total == 0)
		return 0;

	/* For unfiltered "all" — direct copy like before */
	if (cat_filter < 0) {
		if (off >= total)
			return 0;
		if (off + count > total)
			count = total - off;

		ring_pos = (start + (u32)off) % data_size;
		{
			u32 first_chunk = data_size - ring_pos;
			if (count <= first_chunk) {
				if (copy_to_user(buf, trace_data + ring_pos,
						 count))
					return -EFAULT;
			} else {
				if (copy_to_user(buf, trace_data + ring_pos,
						 first_chunk))
					return -EFAULT;
				if (copy_to_user(buf + first_chunk, trace_data,
						 count - first_chunk))
					return -EFAULT;
			}
		}
		*ppos = off + count;
		return count;
	}

	/* Filtered read: scan ring buffer for lines matching category.
	 * We allocate a temp buffer, fill it with matching lines, then
	 * copy the requested [off..off+count) window to user. */
	cat_prefix = isp_trace_cat_prefix[cat_filter];

	/* Allocate temp — cap at 256KB per read to avoid OOM */
	tmp = vmalloc(min_t(size_t, total, 256 * 1024));
	if (!tmp)
		return -ENOMEM;

	out_len = 0;
	ring_pos = start;

	/* Scan line by line */
	for (i = 0; i < total && out_len < 256 * 1024 - 512; ) {
		char line[512];
		int line_len = 0;
		char c;

		/* Read one line from ring buffer */
		while (i < total && line_len < sizeof(line) - 1) {
			c = trace_data[ring_pos % data_size];
			ring_pos = (ring_pos + 1) % data_size;
			i++;
			line[line_len++] = c;
			if (c == '\n')
				break;
		}
		line[line_len] = '\0';

		/* Check if line matches category prefix.
		 * Format: [timestamp] XX pid:comm message
		 * XX is at position after "] " */
		{
			char *p = strchr(line, ']');
			if (p && p[1] == ' ' &&
			    p[2] == cat_prefix[0] &&
			    p[3] == cat_prefix[1]) {
				if (out_len + line_len <= 256 * 1024) {
					memcpy(tmp + out_len, line, line_len);
					out_len += line_len;
				}
			}
		}
	}

	/* Now copy the window [off..off+count) from tmp to user */
	if (off >= out_len) {
		ret = 0;
	} else {
		if (off + count > out_len)
			count = out_len - off;
		if (copy_to_user(buf, tmp + off, count))
			ret = -EFAULT;
		else {
			*ppos = off + count;
			ret = count;
		}
	}

	vfree(tmp);
	return ret;
}

static const struct file_operations isp_trace_read_fops = {
	.read = isp_trace_filtered_read,
};

/* /proc/isp_trace/reset — write "1" to clear */
static ssize_t isp_trace_reset_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	unsigned long flags;

	if (!trace_hdr)
		return -ENODEV;

	spin_lock_irqsave(&trace_lock, flags);
	trace_hdr->magic = ISP_TRACE_MAGIC;
	trace_hdr->write_pos = 0;
	trace_hdr->wrap_count = 0;
	trace_hdr->data_size = ISP_TRACE_DATA_SIZE;
	trace_hdr->entry_count = 0;
	memset(trace_data, 0, ISP_TRACE_DATA_SIZE);
	trace_is_full = 0;
	spin_unlock_irqrestore(&trace_lock, flags);

	pr_info("isp_trace: buffer reset\n");
	return count;
}

static const struct file_operations isp_trace_reset_fops = {
	.write = isp_trace_reset_write,
};

/*
 * /proc/isp_trace/once — record from here until the buffer is full, then
 * stop.
 *
 * Write 1 to arm it: the buffer is cleared, tracing is switched on, and
 * recording stops of its own accord when the sixty-four megabytes are used
 * up. Open the camera after arming and the opening sequence is kept
 * whatever happens afterwards. Write 0 to go back to an ordinary ring.
 */
static int isp_trace_once_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", !trace_stop_when_full ? "off"
			      : trace_is_full ? "full" : "armed");
	return 0;
}

static int isp_trace_once_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_trace_once_show, NULL);
}

static ssize_t isp_trace_once_write(struct file *file,
				    const char __user *buf,
				    size_t count, loff_t *ppos)
{
	unsigned long flags;
	char kbuf[8];
	size_t n = min(count, sizeof(kbuf) - 1);

	if (!trace_hdr)
		return -ENODEV;
	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = 0;

	if (kbuf[0] == '1') {
		spin_lock_irqsave(&trace_lock, flags);
		trace_hdr->magic = ISP_TRACE_MAGIC;
		trace_hdr->write_pos = 0;
		trace_hdr->wrap_count = 0;
		trace_hdr->data_size = ISP_TRACE_DATA_SIZE;
		trace_hdr->entry_count = 0;
		memset(trace_data, 0, ISP_TRACE_DATA_SIZE);
		trace_stop_when_full = 1;
		trace_is_full = 0;
		spin_unlock_irqrestore(&trace_lock, flags);
		isp_trace_enabled = 1;
		pr_info("isp_trace: armed, keeping the first %lu bytes\n",
			(unsigned long)ISP_TRACE_DATA_SIZE);
	} else if (kbuf[0] == '0') {
		trace_stop_when_full = 0;
		trace_is_full = 0;
		pr_info("isp_trace: back to an ordinary ring\n");
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct file_operations isp_trace_once_fops = {
	.open = isp_trace_once_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = isp_trace_once_write,
};

/*
 * Master switch, default OFF.
 *
 * The submit-path instrumentation is not free: for every gather of every
 * ISP/VI submit it maps the command buffer into kernel space, walks it and
 * unmaps it again -- inside the submit path.  Left always-on that is enough
 * to make the stock camera miss its syncpoint deadlines (ispa_memory and
 * vi0_flash stuck, cdma_timeout on ispa_stream).  An instrument that breaks
 * what it measures is worse than no instrument, so tracing is opt-in:
 *
 *   echo 1 > /proc/isp_trace/enable    -- arm it for the run of interest
 *   echo 0 > /proc/isp_trace/enable    -- back to a stock-speed submit path
 *
 * Callers must gate the WHOLE block on this, mapping included -- gating only
 * the record-emitting calls would leave the expensive part in place.
 */
int isp_trace_enabled;
EXPORT_SYMBOL(isp_trace_enabled);

static int isp_trace_enable_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", isp_trace_enabled);
	return 0;
}

static int isp_trace_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_trace_enable_show, NULL);
}

static ssize_t isp_trace_enable_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char kbuf[8];
	size_t n = min(count, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = 0;

	if (kbuf[0] == '1')
		isp_trace_enabled = 1;
	else if (kbuf[0] == '0')
		isp_trace_enabled = 0;
	else
		return -EINVAL;

	pr_info("isp_trace: %s\n",
		isp_trace_enabled ? "enabled" : "disabled");
	return count;
}

static const struct file_operations isp_trace_enable_fops = {
	.open = isp_trace_enable_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = isp_trace_enable_write,
};

/* Filter data — allocated per category */
static struct isp_trace_filter filter_all = { .cat = -1 };
static struct isp_trace_filter filters[ISP_CAT_MAX];

int isp_trace_init(void)
{
	int i;

	if (!trace_phys || !trace_size) {
		pr_warn("isp_trace: no reserved memory, using vmalloc\n");
		trace_base = vzalloc(ISP_TRACE_BUF_SIZE);
		if (!trace_base)
			return -ENOMEM;
		trace_is_phys = 0;
		trace_hdr = (struct isp_trace_header *)trace_base;
		trace_data = (char *)trace_base + sizeof(struct isp_trace_header);
		goto init_header;
	}

	trace_base = ioremap_cached(trace_phys, trace_size);
	if (!trace_base) {
		pr_warn("isp_trace: ioremap_cached failed, trying ioremap\n");
		trace_base = ioremap(trace_phys, trace_size);
	}
	if (!trace_base) {
		pr_err("isp_trace: ioremap failed, using vmalloc\n");
		trace_base = vzalloc(ISP_TRACE_BUF_SIZE);
		if (!trace_base)
			return -ENOMEM;
		trace_is_phys = 0;
		trace_hdr = (struct isp_trace_header *)trace_base;
		trace_data = (char *)trace_base + sizeof(struct isp_trace_header);
		goto init_header;
	}
	trace_is_phys = 1;

	trace_hdr = (struct isp_trace_header *)trace_base;
	trace_data = (char *)trace_base + sizeof(struct isp_trace_header);

	if (trace_hdr->magic == ISP_TRACE_MAGIC) {
		pr_info("isp_trace: found %u entries from previous boot\n",
			trace_hdr->entry_count);
		goto create_proc;
	}

init_header:
	trace_hdr->magic = ISP_TRACE_MAGIC;
	trace_hdr->write_pos = 0;
	trace_hdr->wrap_count = 0;
	trace_hdr->data_size = ISP_TRACE_DATA_SIZE;
	trace_hdr->entry_count = 0;

create_proc:
	proc_dir = proc_mkdir("isp_trace", NULL);
	if (!proc_dir) {
		pr_err("isp_trace: failed to create /proc/isp_trace/\n");
		return -ENOMEM;
	}

	proc_all = proc_create_data("all", 0444, proc_dir,
				    &isp_trace_read_fops, &filter_all);

	for (i = 0; i < ISP_CAT_MAX; i++) {
		filters[i].cat = i;
		proc_cat[i] = proc_create_data(isp_trace_cat_name[i], 0444,
						proc_dir,
						&isp_trace_read_fops,
						&filters[i]);
	}

	proc_reset = proc_create("reset", 0200, proc_dir,
				 &isp_trace_reset_fops);

	proc_create("enable", 0666, proc_dir, &isp_trace_enable_fops);
	proc_create("once", 0666, proc_dir, &isp_trace_once_fops);

	isp_trace_cat(ISP_CAT_POWER, "=== ISP TRACE v2 STARTED ===");
	pr_info("isp_trace: ready, %d categories, phys=0x%pa\n",
		ISP_CAT_MAX, &trace_phys);
	return 0;
}
EXPORT_SYMBOL(isp_trace_init);

void isp_trace_cleanup(void)
{
	int i;

	if (proc_dir) {
		if (proc_all)
			remove_proc_entry("all", proc_dir);
		for (i = 0; i < ISP_CAT_MAX; i++)
			if (proc_cat[i])
				remove_proc_entry(isp_trace_cat_name[i],
						  proc_dir);
		if (proc_reset)
			remove_proc_entry("reset", proc_dir);
		remove_proc_entry("isp_trace", NULL);
	}

	if (trace_base) {
		if (trace_is_phys)
			iounmap(trace_base);
		else
			vfree(trace_base);
		trace_base = NULL;
	}
	trace_hdr = NULL;
	trace_data = NULL;
}
EXPORT_SYMBOL(isp_trace_cleanup);

extern phys_addr_t tegra_isp_trace_get_phys(void);
extern unsigned long tegra_isp_trace_get_size(void);

static int __init isp_trace_module_init(void)
{
	phys_addr_t phys = tegra_isp_trace_get_phys();
	unsigned long size = tegra_isp_trace_get_size();

	if (phys && size)
		isp_trace_set_phys(phys, size);

	return isp_trace_init();
}

subsys_initcall(isp_trace_module_init);
