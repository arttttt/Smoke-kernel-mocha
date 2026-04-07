/*
 * ISP TPG enabler — enables CSI Test Pattern Generator for ISP debugging
 *
 * Writes to /proc/isp_tpg:
 *   echo "1" → enable TPG on CSI-B (port for OV5693/ISP-B)
 *   echo "0" → disable TPG
 *   cat /proc/isp_tpg → show status
 *
 * Handles pll_d clock enable + CSI TPG registers + VI routing.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/clk/tegra.h>
#include <linux/platform_device.h>
#include <linux/nvhost.h>
#include "nvhost_acm.h"

#define VI_BASE_ADDR		0x54080000
#define VI_SIZE			0x40000

/* CSI TPG registers for port B */
#define CSI_PG_CTRL_B		0xa9c
#define CSI_PG_BLANK_B		0xaa0
#define CSI_PG_PHASE_B		0xaa4
#define CSI_PG_RED_FREQ_B	0xaa8
#define CSI_PG_RED_RATE_B	0xaac
#define CSI_PG_GREEN_FREQ_B	0xab0
#define CSI_PG_GREEN_RATE_B	0xab4
#define CSI_PG_BLUE_FREQ_B	0xab8
#define CSI_PG_BLUE_RATE_B	0xabc

/* VI CSI-1 registers */
#define VI_CSI_1_IMAGE_DEF	0x20c
#define VI_CSI_1_IMAGE_SIZE	0x218
#define VI_CSI_1_IMAGE_DT	0x220
#define VI_CSI_1_IMAGE_WC	0x21c
#define VI_CSI_1_ISPINTF	0x264
#define VI_CSI_1_SINGLE_SHOT	0x204

/* CSI CIL */
#define CSI_PHY_CIL_COMMAND	0x908
#define CSI_CILE_PAD_CONFIG0	0x9c8
#define CSI_CLKEN_OVERRIDE	0x968

/* CSI pixel parser B */
#define CSI_PPB_COMMAND		0x86c
#define CSI_PPB_INT_MASK	0x870
#define CSI_PPB_CONTROL0	0x880
#define CSI_PPB_CONTROL1	0x884
#define CSI_PPB_GAP		0x888
#define CSI_INPUT_STREAM_B	0x89c

static void __iomem *vi_base;
static struct clk *pll_d_clk;
static struct platform_device *vi_pdev;
static int tpg_active;

static void vi_write(u32 offset, u32 val)
{
	writel(val, vi_base + offset);
}

static u32 vi_read(u32 offset)
{
	return readl(vi_base + offset);
}

static struct platform_device *find_vi_pdev(void)
{
	struct device *d = bus_find_device_by_name(&platform_bus_type, NULL, "vi.0");
	if (!d) {
		d = bus_find_device_by_name(&platform_bus_type, NULL, "vi");
		if (!d) return NULL;
	}
	return to_platform_device(d);
}

static int tpg_enable(int mode)
{
	int err;

	/* Power on VI module */
	if (!vi_pdev) {
		vi_pdev = find_vi_pdev();
		if (!vi_pdev) {
			pr_err("isp_tpg: can't find vi platform device\n");
			return -ENODEV;
		}
	}
	nvhost_module_busy(vi_pdev);
	pr_info("isp_tpg: VI powered on\n");

	/* Enable pll_d clock for CSI TPG */
	if (!pll_d_clk) {
		pll_d_clk = clk_get_sys(NULL, "pll_d");
		if (IS_ERR(pll_d_clk)) {
			pr_err("isp_tpg: can't get pll_d: %ld\n", PTR_ERR(pll_d_clk));
			pll_d_clk = NULL;
			return -ENODEV;
		}
	}

	err = clk_prepare_enable(pll_d_clk);
	if (err) {
		pr_err("isp_tpg: pll_d enable failed: %d\n", err);
		return err;
	}
	tegra_clk_cfg_ex(pll_d_clk, TEGRA_CLK_PLLD_CSI_OUT_ENB, 1);
	tegra_clk_cfg_ex(pll_d_clk, TEGRA_CLK_PLLD_DSI_OUT_ENB, 1);
	tegra_clk_cfg_ex(pll_d_clk, TEGRA_CLK_MIPI_CSI_OUT_ENB, 0);

	/* CSI pad config */
	vi_write(CSI_CILE_PAD_CONFIG0, 0x0);
	vi_write(CSI_CLKEN_OVERRIDE, 0x0);

	/* CSI pixel parser B */
	vi_write(CSI_PPB_COMMAND, 0xf007);
	vi_write(CSI_PPB_INT_MASK, 0x0);
	vi_write(CSI_PPB_CONTROL0, 0x280301f0);
	vi_write(CSI_PPB_COMMAND, 0xf007);
	vi_write(CSI_PPB_CONTROL1, 0x11);
	vi_write(CSI_PPB_GAP, 0x140000);
	vi_write(CSI_INPUT_STREAM_B, 0x3f0000); /* 1 lane */

	/* CIL command for TPG */
	vi_write(CSI_PHY_CIL_COMMAND, 0x22020202);

	/* TPG registers */
	vi_write(CSI_PG_PHASE_B, 0x0);
	vi_write(CSI_PG_RED_FREQ_B, 0x100010);
	vi_write(CSI_PG_RED_RATE_B, 0x0);
	vi_write(CSI_PG_GREEN_FREQ_B, 0x100010);
	vi_write(CSI_PG_GREEN_RATE_B, 0x0);
	vi_write(CSI_PG_BLUE_FREQ_B, 0x100010);
	vi_write(CSI_PG_BLUE_RATE_B, 0x0);
	vi_write(CSI_PG_BLANK_B, 0x0000FFFF);
	vi_write(CSI_PG_CTRL_B, ((mode - 1) << 2) | 0x1);

	/* VI CSI-1 config — RAW10 + DEST_ISP_B, bypass=0 */
	vi_write(VI_CSI_1_IMAGE_DEF, (0x20 << 16) | 0x04);
	vi_write(VI_CSI_1_IMAGE_SIZE, (1944 << 16) | 2592);
	vi_write(VI_CSI_1_IMAGE_DT, 0x2B); /* RAW10 */
	vi_write(VI_CSI_1_IMAGE_WC, 2592 * 2); /* word count */
	vi_write(VI_CSI_1_ISPINTF, 0x3); /* VI→ISP enable */

	tpg_active = mode;
	pr_info("isp_tpg: enabled mode %d\n", mode);
	pr_info("isp_tpg: IMAGE_DEF=0x%08x ISPINTF=0x%08x PG_CTRL=0x%08x\n",
		vi_read(VI_CSI_1_IMAGE_DEF),
		vi_read(VI_CSI_1_ISPINTF),
		vi_read(CSI_PG_CTRL_B));
	return 0;
}

static void tpg_disable(void)
{
	vi_write(CSI_PG_CTRL_B, 0); /* disable TPG */
	if (pll_d_clk) {
		tegra_clk_cfg_ex(pll_d_clk, TEGRA_CLK_MIPI_CSI_OUT_ENB, 1);
		tegra_clk_cfg_ex(pll_d_clk, TEGRA_CLK_PLLD_CSI_OUT_ENB, 0);
		clk_disable_unprepare(pll_d_clk);
	}
	if (vi_pdev)
		nvhost_module_idle(vi_pdev);
	tpg_active = 0;
	pr_info("isp_tpg: disabled\n");
}

static int tpg_show(struct seq_file *m, void *v)
{
	seq_printf(m, "tpg=%d\n", tpg_active);
	if (vi_base) {
		seq_printf(m, "IMAGE_DEF=0x%08x\n", vi_read(VI_CSI_1_IMAGE_DEF));
		seq_printf(m, "ISPINTF=0x%08x\n", vi_read(VI_CSI_1_ISPINTF));
		seq_printf(m, "PG_CTRL=0x%08x\n", vi_read(CSI_PG_CTRL_B));
	}
	return 0;
}

static int tpg_open(struct inode *inode, struct file *file)
{
	return single_open(file, tpg_show, NULL);
}

static ssize_t tpg_write(struct file *file, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	char buf[16];
	int mode;
	int len = min_t(size_t, count, sizeof(buf) - 1);

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%d", &mode) == 1) {
		if (mode > 0)
			tpg_enable(mode);
		else
			tpg_disable();
	}
	return count;
}

static ssize_t tpg_trigger(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	if (vi_base) {
		vi_write(VI_CSI_1_SINGLE_SHOT, 0x1);
		pr_info("isp_tpg: SINGLE_SHOT fired\n");
	}
	return count;
}

static const struct file_operations tpg_fops = {
	.owner = THIS_MODULE,
	.open = tpg_open,
	.read = seq_read,
	.write = tpg_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations tpg_trigger_fops = {
	.owner = THIS_MODULE,
	.write = tpg_trigger,
};

static int __init isp_tpg_init(void)
{
	vi_base = ioremap(VI_BASE_ADDR, VI_SIZE);
	if (!vi_base) {
		pr_err("isp_tpg: ioremap failed\n");
		return -ENOMEM;
	}

	proc_create("isp_tpg", 0666, NULL, &tpg_fops);
	proc_create("isp_tpg_trigger", 0222, NULL, &tpg_trigger_fops);

	pr_info("isp_tpg: ready (/proc/isp_tpg, /proc/isp_tpg_trigger)\n");
	return 0;
}

static void __exit isp_tpg_exit(void)
{
	tpg_disable();
	remove_proc_entry("isp_tpg_trigger", NULL);
	remove_proc_entry("isp_tpg", NULL);
	if (vi_base)
		iounmap(vi_base);
	if (pll_d_clk)
		clk_put(pll_d_clk);
}

module_init(isp_tpg_init);
module_exit(isp_tpg_exit);
MODULE_LICENSE("GPL v2");
