/*
 *  Copyright (C) 2014, YongK Kim <yongk.kim@lge.com>
 *  Driver for cable detect
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *
 */
#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <soc/qcom/lge/power/lge_power_class.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <soc/qcom/lge/power/lge_cable_detect.h>
#include <linux/wakelock.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/workqueue.h>
#include <soc/qcom/smem.h>

#define MODULE_NAME		"cable_detect"
#define DRIVER_DESC		"Cable detect driver"
#define DRIVER_AUTHOR	"yongk.kim@lge.com"
#define DRIVER_VERSION	"1.0"

#define MAX_CABLE_NUM	15
#define DEFAULT_USB_VAL 1200001

#ifdef CONFIG_LGE_PM_FLOATED_CHARGER
#define FLOATED_START_CHARGE	1
#define FLOATED_POPUP_DISPLAY	2
#endif

struct cable_info_table {
	int threshhold_low;
	int threshhold_high;
	cable_adc_type type;
	unsigned ta_ma;
	unsigned usb_ma;
	struct list_head list;
};

struct cable_detect {
	struct device	*dev;
	struct kobject *kobj;
	struct lge_power lge_cd_lpc;
	struct lge_power *lge_adc_lpc;
	struct power_supply *usb_psy;
	int usb_adc_val;
	int cable_type;
	int cable_type_boot;
	int usb_current;
	int ta_current;
	struct list_head cable_data_list;
	uint32_t usb_id_channel;
	int is_updated;
	int modified_usb_ma;
	int chg_present;
	int chg_type;
	int floated_charger;
	int is_factory_cable;
	int is_factory_cable_boot;
	int scope;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	struct wake_lock iusb_wake_lock;
#endif
};

static cable_boot_type boot_cable_type = NONE_INIT_CABLE;
static struct cable_detect *the_cable_detect;
static int lgcd_thermal_input_current;
static int lgcd_thermal_control;

static int is_factory_cable(int cable_type, bool runTime)
{
	if (runTime) {
		if (cable_type == CABLE_ADC_56K ||
			cable_type == CABLE_ADC_130K ||
			cable_type == CABLE_ADC_910K)
			return FACTORY_CABLE;
		else
			return NORMAL_CABLE;
	} else {
		if (cable_type == LT_CABLE_56K ||
			cable_type == LT_CABLE_130K ||
			cable_type == LT_CABLE_910K)
			return FACTORY_CABLE;
		else
			return NORMAL_CABLE;
	}
}

static int __cable_detect_get_usb_id_adc(struct cable_detect *cd)
{
	int rc = 0;
	union lge_power_propval lge_val = {0,};
	rc = cd->lge_adc_lpc->get_property(cd->lge_adc_lpc,
			LGE_POWER_PROP_USB_ID_PHY, &lge_val);
	cd->usb_adc_val = (int)lge_val.int64val;
	return rc;
}

static int cable_detect_get_usb_id_adc(struct cable_detect *cd)
{
	__cable_detect_get_usb_id_adc(cd);

	return cd->usb_adc_val;
}

static int cable_detect_read_cable_info(struct cable_detect *cd)
{
	int adc;
	struct cable_info_table *cable_info_table;
	adc = cable_detect_get_usb_id_adc(cd);

	pr_info("adc=%d\n", adc);
	list_for_each_entry(cable_info_table, &cd->cable_data_list, list) {
	if (adc >= cable_info_table->threshhold_low &&
				adc <= cable_info_table->threshhold_high) {
		pr_info("cable info --> %d\n", cable_info_table->type);
		cd->usb_current = cable_info_table->usb_ma;
		cd->ta_current = cable_info_table->ta_ma;
		cd->is_factory_cable = is_factory_cable(cable_info_table->type, true);
		return cable_info_table->type;
		}
	}
	return -EINVAL;
}

static char *lge_power_cable_detect_supplied_from[] = {
	"ac",
	"usb",
};

static char *lge_power_cable_detect_supplied_to[] = {
	"batt_id",
	"cc",
	"dock",
};

static char *cd_supplied_to[] = {
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_PSY
#ifdef CONFIG_LGE_PM_CHARGING_RT5058_CHARGER
	"rt5058-charger",
#else
	/* here to add another charger psy */
#endif
#else
	"battery",
#endif
};

static char *lge_cd_supplied_to[] = {
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_PSY
#ifdef CONFIG_LGE_PM_CHARGING_RT5058_CHARGER
	"rt5058-charger"
#else
	/* here to add another charger psy */
#endif
#else
	"battery",
#endif
};

static enum
lge_power_property lge_power_cable_detect_properties[] = {
	LGE_POWER_PROP_IS_FACTORY_CABLE,
	LGE_POWER_PROP_IS_FACTORY_MODE_BOOT,
	LGE_POWER_PROP_CABLE_TYPE,
	LGE_POWER_PROP_CABLE_TYPE_BOOT,
	LGE_POWER_PROP_USB_CURRENT,
	LGE_POWER_PROP_TA_CURRENT,
	LGE_POWER_PROP_UPDATE_CABLE_INFO,
	LGE_POWER_PROP_CHG_PRESENT,
	LGE_POWER_PROP_CURRENT_MAX,
	LGE_POWER_PROP_TYPE,
	LGE_POWER_PROP_FLOATED_CHARGER,
};

static int
lge_power_cable_detect_property_is_writeable(struct lge_power *lpc,
		enum lge_power_property lpp)
{
	int ret = 0;

	switch (lpp) {
	case LGE_POWER_PROP_UPDATE_CABLE_INFO:
		ret = 1;
		break;
	default:
		break;
	}
	return ret;
}

static int
lge_power_lge_cable_detect_set_property(struct lge_power *lpc,
			enum lge_power_property lpp,
			const union lge_power_propval *val)
{
	int ret_val = 0;
	struct cable_detect *cd
				= container_of(lpc, struct cable_detect,
					lge_cd_lpc);

	switch (lpp) {
	case LGE_POWER_PROP_UPDATE_CABLE_INFO:
		if (val->intval == 1) {
			cd->cable_type = cable_detect_read_cable_info(cd);
			if (ret_val < 0)
				ret_val = -EINVAL;
			else
				cd->is_updated = 1;
		}
		break;

	default:
		pr_info("Invalid cable detect property value(%d)\n", \
											(int)lpp);
		ret_val = -EINVAL;
		break;
	}
	return ret_val;
}

static int
lge_power_lge_cable_detect_get_property(struct lge_power *lpc,
			enum lge_power_property lpp,
			union lge_power_propval *val)
{
	int ret_val = 0;
	struct cable_detect *cd
			= container_of(lpc, struct cable_detect, lge_cd_lpc);
	switch (lpp) {
	case LGE_POWER_PROP_IS_FACTORY_CABLE:
		val->intval = cd->is_factory_cable;
		break;

	case LGE_POWER_PROP_IS_FACTORY_MODE_BOOT:
		val->intval = cd->is_factory_cable_boot;
		break;

	case LGE_POWER_PROP_CABLE_TYPE:
		val->intval = cd->cable_type;
		break;

	case LGE_POWER_PROP_CABLE_TYPE_BOOT:
		val->intval = cd->cable_type_boot;
		break;

	case LGE_POWER_PROP_USB_CURRENT:
		val->intval = cd->usb_current;
		break;

	case LGE_POWER_PROP_TA_CURRENT:
		val->intval = cd->ta_current * 1000;
		break;

	case LGE_POWER_PROP_UPDATE_CABLE_INFO:
		val->strval = "W_ONLY";
		break;
	case LGE_POWER_PROP_CURRENT_MAX:
		val->intval = cd->modified_usb_ma;
		break;
	case LGE_POWER_PROP_CHG_PRESENT:
		val->intval = cd->chg_present;
		break;
	case LGE_POWER_PROP_TYPE:
		val->intval = cd->chg_type;
		break;
	case LGE_POWER_PROP_FLOATED_CHARGER:
		val->intval = cd->floated_charger;
		break;
	default:
		pr_err("Invalid cable detect property value(%d)\n",
			(int)lpp);
		ret_val = -EINVAL;
		break;
	}
	return ret_val;
}

static void
lge_cable_detect_external_power_changed(struct lge_power *lpc)
{
	union power_supply_propval ret = {0,};
	struct cable_detect *chip =
		container_of(lpc, struct cable_detect, lge_cd_lpc);
	int pre_modified_usb_ma = chip->modified_usb_ma;
	int pre_floated_charger = chip->floated_charger;
	int pre_scope = chip->scope;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	bool need_to_wake_lock = false;
#endif

	pr_err("[LGE_CHG] update cable info!!!\n");

	/* whenever power_supply_changed is called, adc should be read.*/
	chip->cable_type = cable_detect_read_cable_info(chip);

	if(!chip->usb_psy){
		chip->usb_psy = power_supply_get_by_name("usb");
		if(!chip->usb_psy){
			pr_err("usb power_supply is not probed yet!!!\n");
			return;
		}
	}

	chip->floated_charger = 0;
	/* for otg control */
	chip->usb_psy->get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_SCOPE, &ret);
	chip->scope = ret.intval;

	if (chip->scope == POWER_SUPPLY_SCOPE_SYSTEM) {
		chip->chg_type = POWER_SUPPLY_TYPE_OTG;
		chip->is_factory_cable = 0;
	} else {
		chip->usb_psy->get_property(chip->usb_psy,
				POWER_SUPPLY_PROP_TYPE, &ret);
		chip->chg_type = ret.intval;
	}

	if (chip->is_factory_cable) {
		chip->modified_usb_ma = chip->usb_current * 1000;
	} else {
		if (chip->chg_type == POWER_SUPPLY_TYPE_USB) {
			chip->usb_psy->get_property(chip->usb_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &ret);
			chip->modified_usb_ma = ret.intval;
		} else if (chip->chg_type == POWER_SUPPLY_TYPE_USB_DCP) {
#ifdef CONFIG_LGE_PM_FLOATED_CHARGER
			chip->usb_psy->get_property(chip->usb_psy,
					POWER_SUPPLY_PROP_FLOATED_CHARGER, &ret);

			if (ret.intval == FLOATED_START_CHARGE) {
				chip->modified_usb_ma = chip->usb_current * 1000;
			} else if (ret.intval == FLOATED_POPUP_DISPLAY) {
				chip->modified_usb_ma = chip->usb_current * 1000;
				chip->floated_charger = 1;
			} else {
				if (lgcd_thermal_control > 0) {
					chip->modified_usb_ma =
						lgcd_thermal_input_current < lgcd_thermal_control ?
						lgcd_thermal_input_current : lgcd_thermal_control;
				} else {
					chip->modified_usb_ma = chip->ta_current;
				}
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
				if (chip->modified_usb_ma != chip->ta_current)
					need_to_wake_lock = true;
#endif
				chip->modified_usb_ma *= 1000;
			}
#else
			chip->modified_usb_ma = chip->ta_current * 1000;
#endif
		} else if (chip->chg_type == POWER_SUPPLY_TYPE_OTG) {
			chip->modified_usb_ma = 0;
		} else {
			chip->usb_psy->get_property(chip->usb_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &ret);
			chip->modified_usb_ma = ret.intval;
		}
	}

	chip->usb_psy->get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &ret);
	chip->chg_present = ret.intval;

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	if (need_to_wake_lock == true) {
		if (!wake_lock_active(&chip->iusb_wake_lock)) {
			wake_lock(&chip->iusb_wake_lock);
			pr_err("iusb_wake_lock\n");
		}
	} else {
		if (wake_lock_active(&chip->iusb_wake_lock)) {
			wake_unlock(&chip->iusb_wake_lock);
			pr_err("iusb_wake_unlock\n");
		}
	}
#endif

	if (pre_modified_usb_ma != chip->modified_usb_ma ||
			pre_floated_charger != chip->floated_charger ||
			pre_scope != chip->scope)
		lge_power_changed(&chip->lge_cd_lpc);
}

static void get_cable_data_from_dt(struct cable_detect *cd)
{
	int i;
	u32 cable_value[4];
	struct device_node *node_temp = cd->dev->of_node;
	const char *propname[MAX_CABLE_NUM] = {
		"lge,no-init-cable",
		"lge,cable-mhl-1k",
		"lge,cable-u-28p7k",
		"lge,cable-28p7k",
		"lge,cable-56k",
		"lge,cable-100k",
		"lge,cable-130k",
		"lge,cable-180k",
		"lge,cable-200k",
		"lge,cable-220k",
		"lge,cable-270k",
		"lge,cable-330k",
		"lge,cable-620k",
		"lge,cable-910k",
		"lge,cable-none"
	};
	for (i = 0 ; i < MAX_CABLE_NUM ; i++) {
		struct cable_info_table *cable_info_table;
		cable_info_table = kzalloc(sizeof(struct cable_info_table),
							GFP_KERNEL);
		of_property_read_u32_array(node_temp, propname[i],
							cable_value, 4);
		cable_info_table->threshhold_low = cable_value[0];
		cable_info_table->threshhold_high = cable_value[1];
		cable_info_table->type = i;
		cable_info_table->ta_ma = cable_value[2];
		cable_info_table->usb_ma = cable_value[3];
		list_add_tail(&cable_info_table->list, &cd->cable_data_list);
	}
	of_property_read_u32(node_temp, "lge,usb_id_chan",
						&cd->usb_id_channel);
}

static void
cable_detect_kfree_cable_info_table(struct cable_detect *cd)
{
	struct cable_info_table *cable_info_table, *n;

	list_for_each_entry_safe(cable_info_table, n, &cd->cable_data_list, list) {
		list_del(&cable_info_table->list);
		kfree(cable_info_table);
	}
}

ssize_t lgcd_set_thermal_control(const char *val,
		struct kernel_param *kp){

	int ret;

	ret = param_set_int(val, kp);

	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	if (!the_cable_detect) {
		pr_err("lgcd is not ready\n");
		return 0;
	}

	pr_info("lgcd_thermal_control ==> %d\n", lgcd_thermal_control);
	lge_cable_detect_external_power_changed(&the_cable_detect->lge_cd_lpc);

	return 0;
}
module_param_call(lgcd_thermal_control,
		lgcd_set_thermal_control,
		NULL, &lgcd_thermal_control, 0644);

ssize_t lgcd_set_thermal_input_current(const char *val,
		struct kernel_param *kp){

	int ret;

	ret = param_set_int(val, kp);

	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	if (!the_cable_detect) {
		pr_err("lgcd is not ready\n");
		return 0;
	}

	pr_info("%dmA\n", lgcd_thermal_input_current);
	lge_cable_detect_external_power_changed(&the_cable_detect->lge_cd_lpc);

	return 0;
}
module_param_call(lgcd_thermal_input_current,
		lgcd_set_thermal_input_current,
		NULL, &lgcd_thermal_input_current, 0644);

static int cable_detect_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct lge_power *lge_power_cd;
	struct cable_detect *cd =
		kzalloc(sizeof(struct cable_detect), GFP_KERNEL);

	pr_info("%s: cable_detect probe start!\n", __func__);
	cd->dev = &pdev->dev;
	cd->cable_type = 0;
	cd->cable_type_boot = boot_cable_type;
	cd->is_factory_cable_boot = is_factory_cable(cd->cable_type_boot, false);
	cd->modified_usb_ma = 0;

	platform_set_drvdata(pdev, cd);

	INIT_LIST_HEAD(&cd->cable_data_list);

	get_cable_data_from_dt(cd);
	cd->lge_adc_lpc = lge_power_get_by_name("adc");
	if (!cd->lge_adc_lpc) {
		pr_err("%s : lge_adc_lpc is not yet ready\n", __func__);
		ret = -EPROBE_DEFER;
		goto error;
	}
	lge_power_cd = &cd->lge_cd_lpc;
	lge_power_cd->name = "cable_detect";
	lge_power_cd->properties = lge_power_cable_detect_properties;
	lge_power_cd->num_properties
			= ARRAY_SIZE(lge_power_cable_detect_properties);
	lge_power_cd->set_property
			= lge_power_lge_cable_detect_set_property;
	lge_power_cd->property_is_writeable
			= lge_power_cable_detect_property_is_writeable;
	lge_power_cd->get_property
			= lge_power_lge_cable_detect_get_property;
	lge_power_cd->supplied_from
			= lge_power_cable_detect_supplied_from;
	lge_power_cd->num_supplies
			= ARRAY_SIZE(lge_power_cable_detect_supplied_from);
	lge_power_cd->external_power_changed
			= lge_cable_detect_external_power_changed;
	lge_power_cd->lge_psy_supplied_to = cd_supplied_to;
	lge_power_cd->num_lge_psy_supplicants
			= ARRAY_SIZE(cd_supplied_to);
	lge_power_cd->supplied_to = lge_cd_supplied_to;
	lge_power_cd->num_supplicants = ARRAY_SIZE(lge_cd_supplied_to);
	lge_power_cd->lge_supplied_to
			= lge_power_cable_detect_supplied_to;
	lge_power_cd->num_lge_supplicants
			= ARRAY_SIZE(lge_power_cable_detect_supplied_to);
	ret = lge_power_register(cd->dev, lge_power_cd);
	if (ret < 0) {
		pr_err("[LGE_CHG] Failed to register lge power class: %d\n",
				ret);
		goto error;
	}
	cd->cable_type = cable_detect_read_cable_info(cd);
	cd->chg_present = 0;
	cd->floated_charger = 0;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	wake_lock_init(&cd->iusb_wake_lock, WAKE_LOCK_SUSPEND,
			"lpc_cd_iusb_wake_lock");
#endif
	the_cable_detect = cd;
	pr_info("cable_detect probe end!\n");
	return ret;

error:
	cable_detect_kfree_cable_info_table(cd);
	kfree(cd);
	return ret;
}

static int cable_detect_remove(struct platform_device *pdev)
{
	struct cable_detect *cd = platform_get_drvdata(pdev);
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	wake_lock_destroy(&cd->iusb_wake_lock);
#endif
	cable_detect_kfree_cable_info_table(cd);
	kfree(cd);
	return 0;
}

static struct of_device_id cable_detect_match_table[] = {
	{ .compatible = "lge,cable-detect" },
	{ },
};

static struct platform_driver cable_detect_device_driver = {
	.probe = cable_detect_probe,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = cable_detect_match_table,
	},
	.remove = cable_detect_remove,
};

static int __init cable_detect_init(void)
{
	return platform_driver_register(&cable_detect_device_driver);
}

static void __exit cable_detect_exit(void)
{
	platform_driver_unregister(&cable_detect_device_driver);
}

fs_initcall(cable_detect_init);
module_exit(cable_detect_exit);

static int __init boot_cable_setup(char *boot_cable)
{
	if (!strcmp(boot_cable, "LT_56K"))
		boot_cable_type = LT_CABLE_56K;
	else if (!strcmp(boot_cable, "LT_130K"))
		boot_cable_type = LT_CABLE_130K;
	else if (!strcmp(boot_cable, "400MA"))
		boot_cable_type = USB_CABLE_400MA;
	else if (!strcmp(boot_cable, "DTC_500MA"))
		boot_cable_type = USB_CABLE_DTC_500MA;
	else if (!strcmp(boot_cable, "Abnormal_400MA"))
		boot_cable_type = ABNORMAL_USB_CABLE_400MA;
	else if (!strcmp(boot_cable, "LT_910K"))
		boot_cable_type = LT_CABLE_910K;
	else if (!strcmp(boot_cable, "NO_INIT"))
		boot_cable_type = NONE_INIT_CABLE;
	else
		boot_cable_type = NONE_INIT_CABLE;

	pr_info("Boot cable : %s %d\n", boot_cable, boot_cable_type);

	return 1;
}

__setup("bootcable.type=", boot_cable_setup);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_VERSION(DRIVER_VERSION);
