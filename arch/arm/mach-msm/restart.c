/* Copyright (c) 2010-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/mfd/pmic8058.h>
#include <linux/mfd/pmic8901.h>
#include <linux/mfd/pm8xxx/misc.h>
#include <linux/qpnp/power-on.h>
#include <linux/of_address.h>
#include <soc/qcom/scm.h>

#include <asm/cacheflush.h>

#include <mach/msm_iomap.h>
#include <mach/restart.h>
#include <soc/qcom/socinfo.h>
#include <mach/irqs.h>
#include "msm_watchdog.h"
#include "timer.h"
#include "wdog_debug.h"

#ifdef CONFIG_SEC_DEBUG
#include <mach/sec_debug.h>
#include <linux/notifier.h>
#include <linux/ftrace.h>
#endif


#define WDT0_RST	0x38
#define WDT0_EN		0x40
#define WDT0_BARK_TIME	0x4C
#define WDT0_BITE_TIME	0x5C

#define PSHOLD_CTL_SU (MSM_TLMM_BASE + 0x820)

#define RESTART_REASON_ADDR 0x65C
#define DLOAD_MODE_ADDR     0x0
#define EMERGENCY_DLOAD_MODE_ADDR    0xFE0
#define EMERGENCY_DLOAD_MAGIC1    0x322A4F99
#define EMERGENCY_DLOAD_MAGIC2    0xC67E4350
#define EMERGENCY_DLOAD_MAGIC3    0x77777777

#define SCM_IO_DISABLE_PMIC_ARBITER	1

static int restart_mode;
void *restart_reason;

int pmic_reset_irq;
static void __iomem *msm_tmr0_base;

#ifdef CONFIG_MSM_DLOAD_MODE
#define EDL_MODE_PROP "qcom,msm-imem-emergency_download_mode"
#define DL_MODE_PROP "qcom,msm-imem-download_mode"

static int in_panic;
static void *dload_mode_addr;
static bool dload_mode_enabled;
static void *emergency_dload_mode_addr;

/* Download mode master kill-switch */
static int dload_set(const char *val, struct kernel_param *kp);
static int download_mode = 1;
module_param_call(download_mode, dload_set, param_get_int,
			&download_mode, 0644);
static int panic_prep_restart(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	in_panic = 1;
	return NOTIFY_DONE;
}

static struct notifier_block panic_blk = {
	.notifier_call	= panic_prep_restart,
};

void set_dload_mode(int on)
{
	if (dload_mode_addr) {
		__raw_writel(on ? 0xE47B337D : 0, dload_mode_addr);
		__raw_writel(on ? 0xCE14091A : 0,
		       dload_mode_addr + sizeof(unsigned int));
		mb();
		dload_mode_enabled = on;
#ifdef CONFIG_SEC_DEBUG
		pr_err("set_dload_mode <%d> ( %x )\n", on,
					(unsigned int) CALLER_ADDR0);
#endif		
	}
}

EXPORT_SYMBOL(set_dload_mode);

static bool get_dload_mode(void)
{
	return dload_mode_enabled;
}

static void enable_emergency_dload_mode(void)
{
	if (emergency_dload_mode_addr) {
		__raw_writel(EMERGENCY_DLOAD_MAGIC1,
				emergency_dload_mode_addr);
		__raw_writel(EMERGENCY_DLOAD_MAGIC2,
				emergency_dload_mode_addr +
				sizeof(unsigned int));
		__raw_writel(EMERGENCY_DLOAD_MAGIC3,
				emergency_dload_mode_addr +
				(2 * sizeof(unsigned int)));

		/* Need disable the pmic wdt, then the emergency dload mode
		 * will not auto reset. */
		qpnp_pon_wd_config(0);
		mb();
	}
}

static int dload_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = download_mode;

	ret = param_set_int(val, kp);

	if (ret)
		return ret;

	/* If download_mode is not zero or one, ignore. */
	if (download_mode >> 1) {
		download_mode = old_val;
		return -EINVAL;
	}

	set_dload_mode(download_mode);

	return 0;
}
#else
#define set_dload_mode(x) do {} while (0)

static void enable_emergency_dload_mode(void)
{
	printk(KERN_ERR "dload mode is not enabled on target\n");
}

static bool get_dload_mode(void)
{
	return false;
}
#endif

void msm_set_restart_mode(int mode)
{
	restart_mode = mode;
}
EXPORT_SYMBOL(msm_set_restart_mode);

static bool scm_pmic_arbiter_disable_supported;
/*
 * Force the SPMI PMIC arbiter to shutdown so that no more SPMI transactions
 * are sent from the MSM to the PMIC.  This is required in order to avoid an
 * SPMI lockup on certain PMIC chips if PS_HOLD is lowered in the middle of
 * an SPMI transaction.
 */
static void halt_spmi_pmic_arbiter(void)
{
	if (scm_pmic_arbiter_disable_supported) {
		pr_crit("Calling SCM to disable SPMI PMIC arbiter\n");
		scm_call_atomic1(SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER, 0);
	}
}

static void __msm_power_off(int lower_pshold)
{
	printk(KERN_CRIT "Powering off the SoC\n");
#ifdef CONFIG_MSM_DLOAD_MODE
	set_dload_mode(0);
#endif
	pm8xxx_reset_pwr_off(0);
	qpnp_pon_system_pwr_off(PON_POWER_OFF_SHUTDOWN+1);

	if (lower_pshold) {
		halt_spmi_pmic_arbiter();
		__raw_writel(0, MSM_MPM2_PSHOLD_BASE);

		mdelay(10000);
		printk(KERN_ERR "Powering off has failed\n");
	}
	return;
}

static void msm_power_off(void)
{
	/* MSM initiated power off, lower ps_hold */
	__msm_power_off(1);
}

#ifdef CONFIG_SAMSUNG_LPM_MODE
extern int poweroff_charging;
#endif
static void msm_restart_prepare(const char *cmd)
{
	unsigned long value;

#ifndef CONFIG_SEC_DEBUG
#ifdef CONFIG_MSM_DLOAD_MODE

	/* This looks like a normal reboot at this point. */
	set_dload_mode(0);

	/* Write download mode flags if we're panic'ing */
	set_dload_mode(in_panic);

	/* Write download mode flags if restart_mode says so */
	if (restart_mode == RESTART_DLOAD)
		set_dload_mode(1);

	/* Kill download mode if master-kill switch is set */
	if (!download_mode)
		set_dload_mode(0);
#endif
#endif

#ifdef CONFIG_SEC_DEBUG_LOW_LOG
#ifdef CONFIG_MSM_DLOAD_MODE
#ifdef CONFIG_SEC_DEBUG
	if (sec_debug_is_enabled()
	&& ((restart_mode == RESTART_DLOAD) || in_panic))
		set_dload_mode(1);
	else
		set_dload_mode(0);
#else
	set_dload_mode(0);
	set_dload_mode(in_panic);
	if (restart_mode == RESTART_DLOAD)
		set_dload_mode(1);
#endif
#endif
#endif

	pm8xxx_reset_pwr_off(1);
#if 0 /* FIXME */
	/* Hard reset the PMIC unless memory contents must be maintained. */
	if (get_dload_mode() || (cmd != NULL && cmd[0] != '\0'))
		qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
	else
		qpnp_pon_system_pwr_off(PON_POWER_OFF_HARD_RESET);
#else
		get_dload_mode(); // Only for suppressing a warning message
#ifdef CONFIG_SAMSUNG_LPM_MODE
	if (poweroff_charging) {
		if (in_panic) {
			qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
		} else {
#ifdef CONFIG_MAINTENANCE_MODE
/* When user push pwd key to turn on Device.
 * Because of Hard reset device turn to Maintenance mode.
*/
			qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
#else
			qpnp_pon_system_pwr_off(PON_POWER_OFF_HARD_RESET+1);
#endif
		}
		pr_err("%s : LPM Charging Mode!!, [%d]\n", __func__, in_panic);
	} else {
		qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
		pr_err("%s : Normal mode Mode!!\n", __func__);
	}
#else
	qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
#endif /* CONFIG_SAMSUNG_LPM_MODE */
#endif /* FIXME */

	if (cmd != NULL) {
		if (!strncmp(cmd, "bootloader", 10)) {
			__raw_writel(0x77665500, restart_reason);
		} else if (!strncmp(cmd, "recovery", 8)) {
			__raw_writel(0x77665502, restart_reason);
		} else if (!strcmp(cmd, "rtc")) {
			__raw_writel(0x77665503, restart_reason);
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned long code;
			code = simple_strtoul(cmd + 4, NULL, 16) & 0xff;
			__raw_writel(0x6f656d00 | code, restart_reason);
#ifdef CONFIG_SEC_DEBUG
		} else if (!strncmp(cmd, "sec_debug_hw_reset", 18)) {
			__raw_writel(0x776655ee, restart_reason);
#endif
		} else if (!strncmp(cmd, "download", 8)) {
		    __raw_writel(0x12345671, restart_reason);
		} else if (!strncmp(cmd, "sud", 3)) {
			__raw_writel(0xabcf0000 | (cmd[3] - '0'),
					restart_reason);
		} else if (!strncmp(cmd, "edl", 3)) {
			enable_emergency_dload_mode();
		} else if (!strncmp(cmd, "debug", 5)
				&& !kstrtoul(cmd + 5, 0, &value)) {
			__raw_writel(0xabcd0000 | value, restart_reason);
#ifdef CONFIG_SEC_SSR_DEBUG_LEVEL_CHK
		} else if (!strncmp(cmd, "cpdebug", 7) /* set cp debug level */
				&& !kstrtoul(cmd + 7, 0, &value)) {
			__raw_writel(0xfedc0000 | value, restart_reason);
#endif
#if defined(CONFIG_SWITCH_DUAL_MODEM) || defined(CONFIG_MUIC_SUPPORT_RUSTPROOF)
		} else if (!strncmp(cmd, "swsel", 5) /* set switch value */
				&& !kstrtoul(cmd + 5, 0, &value)) {
			__raw_writel(0xabce0000 | value, restart_reason);
#endif
		} else if (strlen(cmd) == 0 ) {
			pr_notice("%s : value of cmd is NULL.\n",__func__);
			__raw_writel(0x12345678, restart_reason);
		} else if (strlen(cmd) == 0) {
		    printk(KERN_NOTICE "%s : value of cmd is NULL.\n", __func__);
		    __raw_writel(0x12345678, restart_reason);
#ifdef CONFIG_SEC_PERIPHERAL_SECURE_CHK
		} else if (!strncmp(cmd, "peripheral_hw_reset", 19)) {
			__raw_writel(0x77665507, restart_reason);
#endif
		} else {
			__raw_writel(0x77665501, restart_reason);
		}
	} 
#ifdef CONFIG_SEC_DEBUG	
	else {
			pr_notice("%s : clear reset flag.\n",__func__);
			__raw_writel(0x12345678, restart_reason);
	}
#endif

	flush_cache_all();
	outer_flush_all();
}

void msm_restart(char mode, const char *cmd)
{
	printk(KERN_NOTICE "Going down for restart now\n");

	msm_restart_prepare(cmd);

	/* Needed to bypass debug image on some chips */
	msm_disable_wdog_debug();
	halt_spmi_pmic_arbiter();
	__raw_writel(0, MSM_MPM2_PSHOLD_BASE);

	mdelay(10000);
	printk(KERN_ERR "Restarting has failed\n");
}

#ifdef CONFIG_SEC_DEBUG
static int dload_mode_normal_reboot_handler(struct notifier_block *nb,
				unsigned long l, void *p)
{
	set_dload_mode(0);
	return 0;
}

static struct notifier_block dload_reboot_block = {
	.notifier_call = dload_mode_normal_reboot_handler
};
#endif

static int __init msm_restart_init(void)
{
	struct device_node *np;
	int ret = 0;

#ifdef CONFIG_MSM_DLOAD_MODE
	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);
	np = of_find_compatible_node(NULL, NULL, DL_MODE_PROP);
	if (!np) {
		pr_err("unable to find DT imem download mode node\n");
		ret = -ENODEV;
		goto err_dl_mode;
	}
	dload_mode_addr = of_iomap(np, 0);
	if (!dload_mode_addr) {
		pr_err("unable to map imem download model offset\n");
		ret = -ENOMEM;
		goto err_dl_mode;
	}

	np = of_find_compatible_node(NULL, NULL, EDL_MODE_PROP);
	if (!np) {
		pr_err("unable to find DT imem emergency download mode node\n");
		ret = -ENODEV;
		goto err_edl_mode;
	}
	emergency_dload_mode_addr = of_iomap(np, 0);
	if (!emergency_dload_mode_addr) {
		pr_err("unable to map imem emergency download model offset\n");
		ret = -ENOMEM;
		goto err_edl_mode;
	}

#ifdef CONFIG_SEC_DEBUG
	register_reboot_notifier(&dload_reboot_block);
#endif
#ifdef CONFIG_SEC_DEBUG_LOW_LOG
	if (!sec_debug_is_enabled()) {
		set_dload_mode(0);
	} else
#endif
	set_dload_mode(download_mode);
#endif
	msm_tmr0_base = msm_timer_get_timer0_base();
	np = of_find_compatible_node(NULL, NULL, "qcom,msm-imem-restart_reason");
	if (!np) {
		pr_err("unable to find DT imem restart reason node\n");
		ret = -ENODEV;
		goto err_restart_reason;
	}
	restart_reason = of_iomap(np, 0);
	if (!restart_reason) {
		pr_err("unable to map imem restart reason offset\n");
		ret = -ENOMEM;
		goto err_restart_reason;
	}
	pm_power_off = msm_power_off;

	if (scm_is_call_available(SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER) > 0)
		scm_pmic_arbiter_disable_supported = true;

	return 0;

err_restart_reason:
#ifdef CONFIG_MSM_DLOAD_MODE
	iounmap(emergency_dload_mode_addr);
err_edl_mode:
	iounmap(dload_mode_addr);
err_dl_mode:
#endif
	return ret;
}
early_initcall(msm_restart_init);
