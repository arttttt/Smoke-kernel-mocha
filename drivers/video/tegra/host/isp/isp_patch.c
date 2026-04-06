/*
 * ISP Method Patcher
 *
 * Intercepts ISP host1x command buffers and patches method values
 * before hardware submission.
 *
 * Usage:
 *   echo "0xE00=0x12345678" > /proc/isp_patch   — set patch
 *   echo "0xE00=del" > /proc/isp_patch           — remove patch
 *   echo "clear" > /proc/isp_patch                — clear all
 *   cat /proc/isp_patch                           — list active patches
 *
 * Copyright (c) 2026, Smoke Team. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/ctype.h>

#include "isp_patch.h"
#include "isp_trace.h"

#define ISP_PATCH_MAX	64

struct isp_patch_entry {
	u16 method;
	u32 value;
	bool active;
};

static struct isp_patch_entry patches[ISP_PATCH_MAX];
static DEFINE_SPINLOCK(patch_lock);
static struct proc_dir_entry *proc_entry;
static int patch_enabled = 1;

/* ----------------------------------------------------------------
 * Patch list management
 * ---------------------------------------------------------------- */

static int patch_find(u16 method)
{
	int i;

	for (i = 0; i < ISP_PATCH_MAX; i++)
		if (patches[i].active && patches[i].method == method)
			return i;
	return -1;
}

static int patch_add(u16 method, u32 value)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&patch_lock, flags);

	/* Update existing */
	i = patch_find(method);
	if (i >= 0) {
		patches[i].value = value;
		spin_unlock_irqrestore(&patch_lock, flags);
		return 0;
	}

	/* Find free slot */
	for (i = 0; i < ISP_PATCH_MAX; i++) {
		if (!patches[i].active) {
			patches[i].method = method;
			patches[i].value = value;
			patches[i].active = true;
			spin_unlock_irqrestore(&patch_lock, flags);
			return 0;
		}
	}

	spin_unlock_irqrestore(&patch_lock, flags);
	return -ENOSPC;
}

static int patch_del(u16 method)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&patch_lock, flags);
	i = patch_find(method);
	if (i >= 0)
		patches[i].active = false;
	spin_unlock_irqrestore(&patch_lock, flags);
	return i >= 0 ? 0 : -ENOENT;
}

static void patch_clear(void)
{
	unsigned long flags;

	spin_lock_irqsave(&patch_lock, flags);
	memset(patches, 0, sizeof(patches));
	spin_unlock_irqrestore(&patch_lock, flags);
}

/* ----------------------------------------------------------------
 * /proc/isp_patch interface
 * ---------------------------------------------------------------- */

static int isp_patch_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i, count = 0;

	spin_lock_irqsave(&patch_lock, flags);
	for (i = 0; i < ISP_PATCH_MAX; i++) {
		if (patches[i].active) {
			seq_printf(m, "0x%03x=0x%08x\n",
				   patches[i].method, patches[i].value);
			count++;
		}
	}
	spin_unlock_irqrestore(&patch_lock, flags);

	if (!count)
		seq_puts(m, "(no patches)\n");

	seq_printf(m, "# enabled=%d\n", patch_enabled);
	return 0;
}

static int isp_patch_open(struct inode *inode, struct file *file)
{
	return single_open(file, isp_patch_show, NULL);
}

static ssize_t isp_patch_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char buf[64];
	unsigned int method;
	unsigned int value;
	int len;

	len = min_t(size_t, count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	/* Strip trailing newline */
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	/* "clear" — remove all patches */
	if (strcmp(buf, "clear") == 0) {
		patch_clear();
		pr_info("isp_patch: all patches cleared\n");
		return count;
	}

	/* "enable" / "disable" */
	if (strcmp(buf, "enable") == 0) {
		patch_enabled = 1;
		pr_info("isp_patch: enabled\n");
		return count;
	}
	if (strcmp(buf, "disable") == 0) {
		patch_enabled = 0;
		pr_info("isp_patch: disabled\n");
		return count;
	}

	/* "0xNNN=0xVVVVVVVV" — add/update patch (check BEFORE =del!) */
	if (sscanf(buf, "0x%x=0x%x", &method, &value) == 2 ||
	    sscanf(buf, "%x=%x", &method, &value) == 2) {
		if (patch_add(method & 0xFFF, value) == 0)
			pr_info("isp_patch: set 0x%03x = 0x%08x\n",
				method & 0xFFF, value);
		else
			pr_err("isp_patch: no free slots\n");
		return count;
	}

	/* "0xNNN=del" — remove patch */
	if (sscanf(buf, "0x%x=del", &method) == 1 ||
	    sscanf(buf, "%x=del", &method) == 1) {
		if (patch_del(method & 0xFFF) == 0)
			pr_info("isp_patch: removed 0x%03x\n", method & 0xFFF);
		else
			pr_warn("isp_patch: 0x%03x not found\n", method & 0xFFF);
		return count;
	}

	pr_warn("isp_patch: bad input: '%s'\n", buf);
	return count;
}

static const struct file_operations isp_patch_fops = {
	.owner = THIS_MODULE,
	.open = isp_patch_open,
	.read = seq_read,
	.write = isp_patch_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ----------------------------------------------------------------
 * Host1x opcode parser + patcher
 * ---------------------------------------------------------------- */

int isp_patch_gather(u32 *buf, int words)
{
	int pos = 0;
	int applied = 0;
	u32 op;
	u8 opcode;
	u16 method;
	u16 count;
	int i, idx;
	unsigned long flags;

	if (!patch_enabled)
		return 0;

	spin_lock_irqsave(&patch_lock, flags);

	while (pos < words) {
		op = buf[pos];
		opcode = (op >> 28) & 0xF;

		switch (opcode) {
		case 0: /* SETCLASS: class_id[9:0], offset[15:0], mask */
			pos++;
			break;

		case 1: /* INCR: offset[11:0] at [27:16], count[15:0] */
			method = (op >> 16) & 0xFFF;
			count = op & 0xFFFF;
			pos++; /* skip opcode word */
			for (i = 0; i < count && pos < words; i++, pos++) {
				idx = patch_find(method + i);
				if (idx >= 0) {
					isp_trace_cat(ISP_CAT_GATHER,
						"PATCH 0x%03x: 0x%08x -> 0x%08x",
						method + i, buf[pos],
						patches[idx].value);
					buf[pos] = patches[idx].value;
					applied++;
				}
			}
			break;

		case 2: /* NONINCR: offset[11:0] at [27:16], count[15:0] */
			method = (op >> 16) & 0xFFF;
			count = op & 0xFFFF;
			pos++; /* skip opcode word */
			for (i = 0; i < count && pos < words; i++, pos++) {
				idx = patch_find(method);
				if (idx >= 0) {
					isp_trace_cat(ISP_CAT_GATHER,
						"PATCH 0x%03x[%d]: 0x%08x -> 0x%08x",
						method, i, buf[pos],
						patches[idx].value);
					buf[pos] = patches[idx].value;
					applied++;
				}
			}
			break;

		case 4: /* IMM: offset[11:0] at [27:16], value[15:0] */
			/* Immediate value embedded in opcode word itself.
			 * To patch: replace low 16 bits of opcode. */
			method = (op >> 16) & 0xFFF;
			idx = patch_find(method);
			if (idx >= 0) {
				u32 new_op = (op & 0xFFFF0000) |
					     (patches[idx].value & 0xFFFF);
				isp_trace_cat(ISP_CAT_GATHER,
					"PATCH_IMM 0x%03x: 0x%08x -> 0x%08x",
					method, op, new_op);
				buf[pos] = new_op;
				applied++;
			}
			pos++;
			break;

		case 6: /* GATHER */
			pos += 2; /* opcode + address */
			break;

		default:
			pos++;
			break;
		}
	}

	spin_unlock_irqrestore(&patch_lock, flags);
	return applied;
}

/* ----------------------------------------------------------------
 * Init / Cleanup
 * ---------------------------------------------------------------- */

int isp_patch_init(void)
{
	memset(patches, 0, sizeof(patches));

	proc_entry = proc_create("isp_patch", 0666, NULL, &isp_patch_fops);
	if (!proc_entry) {
		pr_err("isp_patch: failed to create /proc/isp_patch\n");
		return -ENOMEM;
	}

	pr_info("isp_patch: ready (/proc/isp_patch)\n");
	return 0;
}

void isp_patch_cleanup(void)
{
	if (proc_entry)
		remove_proc_entry("isp_patch", NULL);
	proc_entry = NULL;
	pr_info("isp_patch: removed\n");
}
