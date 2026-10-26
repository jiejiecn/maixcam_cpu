// SPDX-License-Identifier: GPL-2.0
/*
 * mpll_freq.c - CPU frequency control for Sophgo CV181x / SG2002 (MaixCAM Pro)
 *
 * The CV181x has no cpufreq driver: the CPU (RISC-V C906, hart 0) is clocked
 * from the MPLL through a mux/divider called "clk_c906_0".  This module asks
 * the kernel clock framework to retune the MPLL:
 *
 *     clk_get(NULL, "clk_mpll")  ->  clk_set_rate(mpll, 900 MHz)
 *
 * Going through clk_set_rate() matters: the vendor PLL driver recalculates the
 * PLL loop-filter field (ictrl) as well as the divider.  Poking MPLL_CSR with
 * devmem alone only changes the divider and does NOT change the output clock.
 *
 * The module also ioremaps the clock controller page and prints MPLL_CSR
 * before/after, so the effect can be verified from dmesg.
 *
 * Build: see ../build.sh
 * Use:   insmod mpll_freq.ko target=900000000 delay_ms=3000
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#define MPLL_REG_BASE		0x03002000
#define MPLL_REG_SIZE		0x1000
#define MPLL_CSR_OFF		0x908

static unsigned long target = 900000000;
module_param(target, ulong, 0644);
MODULE_PARM_DESC(target, "target rate for clk_mpll in Hz (default 900000000)");

static unsigned int delay_ms = 5000;
module_param(delay_ms, uint, 0644);
MODULE_PARM_DESC(delay_ms, "delay before touching the PLL, in ms (default 5000)");

static struct clk *mpll;
static struct delayed_work mpll_work;
static void __iomem *regs;

static void mpll_work_fn(struct work_struct *w)
{
	unsigned long before, after;
	u32 csr_before = 0, csr_after = 0;
	int ret;

	if (regs) {
		csr_before = readl(regs + MPLL_CSR_OFF);
		pr_info("mpll_freq: MPLL_CSR before = 0x%08x\n", csr_before);
	}

	before = clk_get_rate(mpll);
	pr_info("mpll_freq: rate before = %lu Hz, requesting %lu Hz\n",
		before, target);

	/* the real frequency change: goes through cv181x g6_pll_set_rate() */
	ret = clk_set_rate(mpll, target);

	after = clk_get_rate(mpll);
	if (regs) {
		csr_after = readl(regs + MPLL_CSR_OFF);
		pr_info("mpll_freq: MPLL_CSR after  = 0x%08x\n", csr_after);
	}
	pr_info("mpll_freq: clk_set_rate() = %d, rate after = %lu Hz\n",
		ret, after);
	pr_info("mpll_freq: note: rate after is the kernel cached value\n");
}

static int __init mpll_init(void)
{
	mpll = clk_get(NULL, "clk_mpll");
	if (IS_ERR(mpll)) {
		pr_err("mpll_freq: clk_get(clk_mpll) failed: %ld\n",
		       PTR_ERR(mpll));
		return PTR_ERR(mpll);
	}

	regs = ioremap(MPLL_REG_BASE, MPLL_REG_SIZE);
	if (!regs)
		pr_warn("mpll_freq: ioremap(0x%x) failed, register dump disabled\n",
			MPLL_REG_BASE);

	pr_info("mpll_freq: loaded, current rate = %lu Hz, will retune in %u ms\n",
		clk_get_rate(mpll), delay_ms);

	/*
	 * Run the change from a workqueue after a delay, so that insmod returns
	 * first and the "about to change frequency" message is visible in dmesg
	 * (or on the serial console) even if the board hangs.
	 */
	INIT_DELAYED_WORK(&mpll_work, mpll_work_fn);
	schedule_delayed_work(&mpll_work, msecs_to_jiffies(delay_ms));
	return 0;
}

static void __exit mpll_exit(void)
{
	cancel_delayed_work_sync(&mpll_work);
	if (regs)
		iounmap(regs);
	if (!IS_ERR_OR_NULL(mpll))
		clk_put(mpll);
	pr_info("mpll_freq: unloaded (frequency is left as-is)\n");
}

module_init(mpll_init);
module_exit(mpll_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("maixcam_cpu");
MODULE_DESCRIPTION("Set clk_mpll rate on CV181x/SG2002 (MaixCAM Pro)");
