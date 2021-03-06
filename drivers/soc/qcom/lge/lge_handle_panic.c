/*
 * lge_handle_panic.c
 *
 * Copyright (C) 2014 LGE, Inc
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <asm/setup.h>
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/lge/lge_handle_panic.h>
#ifdef CONFIG_LGE_LAF_MODE
#include <soc/qcom/lge/lge_laf_mode.h>
#endif
#include <soc/qcom/scm.h>
#include <linux/msm_thermal.h>

#ifdef CONFIG_LGE_USE_PON_REBOOT_REG
#include <linux/qpnp/power-on.h>
#endif

#if defined(CONFIG_ARCH_MSM8916) | defined(CONFIG_ARCH_MSM8909)
#define NO_ADSP
#endif

#define PANIC_HANDLER_NAME        "panic-handler"

#define RESTART_REASON_ADDR       0x65C
#define DLOAD_MODE_ADDR           0x0

#define CRASH_HANDLER_MAGIC_VALUE 0x4c474500
#define CRASH_HANDLER_MAGIC_ADDR  0x28
#define RAM_CONSOLE_ADDR_ADDR     0x2C
#define RAM_CONSOLE_SIZE_ADDR     0x30
#define FB_ADDR_ADDR              0x34

#define RESTART_REASON      (msm_imem_base + RESTART_REASON_ADDR)
#define CRASH_HANDLER_MAGIC (msm_imem_base + CRASH_HANDLER_MAGIC_ADDR)
#define RAM_CONSOLE_ADDR    (msm_imem_base + RAM_CONSOLE_ADDR_ADDR)
#define RAM_CONSOLE_SIZE    (msm_imem_base + RAM_CONSOLE_SIZE_ADDR)
#define FB_ADDR             (msm_imem_base + FB_ADDR_ADDR)

static void *msm_imem_base;
static int dummy_arg;

static int subsys_crash_magic = 0x0;

void lge_set_subsys_crash_reason(const char *name, int type)
{
	const char *subsys_name[] = { "adsp", "mba", "modem", "wcnss", "venus" };
	int i = 0;

	if (!name)
		return;

	for (i = 0; i < ARRAY_SIZE(subsys_name); i++) {
		if (!strncmp(subsys_name[i], name, 40)) {
			subsys_crash_magic = LGE_RB_MAGIC | ((i+1) << 12)
					| type;
			break;
		}
	}

	return;
}

void lge_set_ram_console_addr(unsigned int addr, unsigned int size)
{
	__raw_writel(addr, RAM_CONSOLE_ADDR);
	__raw_writel(size, RAM_CONSOLE_SIZE);
}

void lge_set_fb_addr(unsigned int addr)
{
	__raw_writel(addr, FB_ADDR);
}

void lge_set_restart_reason(unsigned int reason)
{
#if defined(CONFIG_LAF_G_DRIVER) && defined(CONFIG_LGE_LAF_MODE)
	if ((lge_get_laf_mode() == LGE_LAF_MODE_LAF) && (reason != LAF_DLOAD_MODE)) {
#ifdef CONFIG_LGE_USE_PON_REBOOT_REG
		qpnp_pon_set_restart_reason(
			PON_RESTART_REASON_LGE_ERR_LAF);
#endif
		__raw_writel(LGE_RB_MAGIC | LGE_ERR_LAF, RESTART_REASON);
	}
	else {
#ifdef CONFIG_LGE_USE_PON_REBOOT_REG
		if (reason == LAF_DLOAD_MODE)
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_LGE_LAF_DLOAD_MODE);
#endif

#ifdef CONFIG_LGE_USE_PON_REBOOT_REG
	if ((reason & 0xFFFF0000) == LGE_RB_MAGIC) {
		qpnp_pon_set_restart_reason(
			PON_RESTART_REASON_LGE_CRASH_MODE);
	}
#endif

		__raw_writel(reason, RESTART_REASON);
	}
#else
#ifdef CONFIG_LGE_USE_PON_REBOOT_REG
	if (reason == LAF_DLOAD_MODE)
		qpnp_pon_set_restart_reason(
			PON_RESTART_REASON_LGE_LAF_DLOAD_MODE);
#endif

#ifdef CONFIG_LGE_USE_PON_REBOOT_REG
	if ((reason & 0xFFFF0000) == LGE_RB_MAGIC) {
		qpnp_pon_set_restart_reason(
			PON_RESTART_REASON_LGE_CRASH_MODE);
	}
#endif

	__raw_writel(reason, RESTART_REASON);
#endif
}

void lge_set_panic_reason(void)
{
	if (subsys_crash_magic == 0)
		lge_set_restart_reason(LGE_RB_MAGIC | LGE_ERR_KERN);
	else
		lge_set_restart_reason(subsys_crash_magic);
}

static int gen_bug(const char *val, struct kernel_param *kp)
{
	BUG();
	return 0;
}
module_param_call(gen_bug, gen_bug, param_get_bool, &dummy_arg,
		S_IWUSR | S_IRUGO);

static int gen_panic(const char *val, struct kernel_param *kp)
{
	panic("generate test-panic");
	return 0;
}
module_param_call(gen_panic, gen_panic, param_get_bool, &dummy_arg,
		S_IWUSR | S_IRUGO);

#ifndef NO_ADSP
static int gen_adsp_panic(const char *val, struct kernel_param *kp)
{
	subsystem_restart("adsp");
	return 0;
}
module_param_call(gen_adsp_panic, gen_adsp_panic, param_get_bool, &dummy_arg,
		S_IWUSR | S_IRUGO);
#endif

static int gen_mba_panic(const char *val, struct kernel_param *kp)
{
	subsystem_restart("mba");
	return 0;
}
module_param_call(gen_mba_panic, gen_mba_panic, param_get_bool, &dummy_arg,
		S_IWUSR | S_IRUGO);

static int gen_modem_panic(const char *val, struct kernel_param *kp)
{
	subsystem_restart("modem");
	return 0;
}
module_param_call(gen_modem_panic, gen_modem_panic, param_get_bool, &dummy_arg,
		S_IWUSR | S_IRUGO);

static int gen_wcnss_panic(const char *val, struct kernel_param *kp)
{
	subsystem_restart("wcnss");
	return 0;
}
module_param_call(gen_wcnss_panic, gen_wcnss_panic, param_get_bool, &dummy_arg,
		S_IWUSR | S_IRUGO);

static int gen_venus_panic(const char *val, struct kernel_param *kp)
{
	subsystem_restart("venus");
	return 0;
}
module_param_call(gen_venus_panic, gen_venus_panic, param_get_bool, &dummy_arg,
		S_IWUSR | S_IRUGO);

#define MDELAY_TIME    15000

static int gen_wdt_bark(const char *val, struct kernel_param *kp)
{
	preempt_disable();
	mdelay(MDELAY_TIME);
	preempt_enable();

	return 0;
}
module_param_call(gen_wdt_bark, gen_wdt_bark, param_get_bool,
		&dummy_arg, S_IWUSR | S_IRUGO);

static int gen_wdt_bite(const char *val, struct kernel_param *kp)
{
	local_irq_disable();
	mdelay(MDELAY_TIME);
	local_irq_enable();

	return 0;
}
module_param_call(gen_wdt_bite, gen_wdt_bite, param_get_bool,
		&dummy_arg, S_IWUSR | S_IRUGO);

#define REG_MPM2_WDOG_BASE            0x004AA000
#define REG_OFFSET_MPM2_WDOG_RESET    0x0
#define REG_OFFSET_MPM2_WDOG_BITE_VAL 0x10

#define REG_VAL_WDOG_RESET_DO_RESET   0x1
#define REG_VAL_WDOG_BITE_VAL         0x400

#define SCM_SVC_SEC_WDOG_TRIG         0x8

#ifdef CONFIG_HOTPLUG_CPU
static void bring_other_cpus_down(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		cpu_down(cpu);
	}
}
#else
static void bring_other_cpus_down(void)
{
}
#endif

static int gen_sec_wdt_scm(const char *val, struct kernel_param *kp)
{
	int ret;
	int scm_ret = 0;
	struct scm_desc desc = {0};

	desc.args[0] = 0;
	desc.arginfo = SCM_ARGS(1);

	if (!is_scm_armv8()) {
		u8 trigger = 0;
		ret = scm_call(SCM_SVC_BOOT, SCM_SVC_SEC_WDOG_TRIG, &trigger,
		sizeof(trigger), NULL, 0);
	} else {
		ret = scm_call2(SCM_SIP_FNID(SCM_SVC_BOOT,
		SCM_SVC_SEC_WDOG_TRIG), &desc);
		scm_ret = desc.ret[0];
	}
	if (ret || scm_ret)
		pr_err("Secure watchdog bite failed\n");

	return 0;
}
module_param_call(gen_sec_wdt_scm, gen_sec_wdt_scm, param_get_bool,
		&dummy_arg, S_IWUSR | S_IRUGO);

static int gen_sec_wdt_bite(const char *val, struct kernel_param *kp)
{
	static void *sec_wdog_virt;
	bring_other_cpus_down();
	sec_wdog_virt = ioremap(REG_MPM2_WDOG_BASE, SZ_4K);
	if (!sec_wdog_virt) {
		pr_info("unable to map sec wdog page\n");
		return -1;
	}
	writel_relaxed(REG_VAL_WDOG_RESET_DO_RESET,
			sec_wdog_virt + REG_OFFSET_MPM2_WDOG_RESET);
	writel_relaxed(REG_VAL_WDOG_BITE_VAL,
			sec_wdog_virt + REG_OFFSET_MPM2_WDOG_BITE_VAL);
	/* wait after set */
	mb();
	mdelay(MDELAY_TIME);

	return 0;
}
module_param_call(gen_sec_wdt_bite, gen_sec_wdt_bite, param_get_bool,
		&dummy_arg, S_IWUSR | S_IRUGO);


static int gen_thermal_bite(const char *val, struct kernel_param *kp)
{
	msm_thermal_bite(0, 6000);

	return 0;
}
module_param_call(gen_thermal_bite, gen_thermal_bite, param_get_bool,
		&dummy_arg, S_IWUSR | S_IRUGO);

static int __init lge_panic_handler_early_init(void)
{
	struct device_node *np;
	uint32_t crash_handler_magic = 0;
	uint32_t mem_addr = 0;
	uint32_t mem_size = 0;

	np = of_find_compatible_node(NULL, NULL, "qcom,msm-imem");
	if (!np) {
		pr_err("unable to find DT imem node\n");
		return -ENODEV;
	}
	msm_imem_base = of_iomap(np, 0);
	if (!msm_imem_base) {
		pr_err("unable to map imem\n");
		return -ENODEV;
	}

	np = of_find_compatible_node(NULL, NULL, "ramoops");
	if (!np) {
		pr_err("unable to find DT ramoops node\n");
		return -ENODEV;
	}

	of_property_read_u32(np, "mem-address", &mem_addr);
	of_property_read_u32(np, "mem-size", &mem_size);
	pr_info("mem-address=%d\n", mem_addr);
	pr_info("mem-size=%d\n", mem_size);
	lge_set_ram_console_addr(mem_addr, mem_size);

	/* check struct boot_shared_imem_cookie_type is matched */
	crash_handler_magic = __raw_readl(CRASH_HANDLER_MAGIC);
	WARN(crash_handler_magic != CRASH_HANDLER_MAGIC_VALUE,
			"Check sbl's struct boot_shared_imem_cookie_type.\n"
			"Need to update lge_handle_panic's imem offset.\n");

	/* Set default restart_reason to hw reset. */
	lge_set_restart_reason(LGE_RB_MAGIC | LGE_ERR_TZ);

	return 0;
}

early_initcall(lge_panic_handler_early_init);

static int __init lge_panic_handler_probe(struct platform_device *pdev)
{
	int ret = 0;

	return ret;
}

static int lge_panic_handler_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver panic_handler_driver __refdata = {
	.probe = lge_panic_handler_probe,
	.remove = lge_panic_handler_remove,
	.driver = {
		.name = PANIC_HANDLER_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init lge_panic_handler_init(void)
{
	return platform_driver_register(&panic_handler_driver);
}

static void __exit lge_panic_handler_exit(void)
{
	platform_driver_unregister(&panic_handler_driver);
}

module_init(lge_panic_handler_init);
module_exit(lge_panic_handler_exit);

MODULE_DESCRIPTION("LGE panic handler driver");
MODULE_AUTHOR("SungEun Kim <cleaneye.kim@lge.com>");
MODULE_LICENSE("GPL");
