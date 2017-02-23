/*
 *  max17048_battery.c
 *  fuel-gauge systems for lithium-ion (Li+) batteries
 *
 *  Copyright (C) 2012 Nvidia Cooperation
 *  Chandler Zhang <chazhang@nvidia.com>
 *  Syed Rafiuddin <srafiuddin@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <asm/unaligned.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/power/max17048_battery.h>
#include <soc/qcom/smsm.h>

#ifdef CONFIG_LGE_PM
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/power_supply.h>
#include <linux/wakelock.h>
#define MAX17048_MODE		0x06
#include <soc/qcom/lge/board_lge.h>
#endif

#define MAX17048_VCELL                  0x02
#define MAX17048_SOC                    0x04
#define MAX17048_VER                    0x08
#define MAX17048_HIBRT                  0x0A
#define MAX17048_CONFIG                 0x0C
#define MAX17048_OCV                    0x0E
#define MAX17048_VLRT                   0x14
#define MAX17048_VRESET                 0x18
#define MAX17048_STATUS                 0x1A
#define MAX17048_UNLOCK                 0x3E
#define MAX17048_TABLE                  0x40
#define MAX17048_RCOMPSEG1              0x80
#define MAX17048_RCOMPSEG2              0x90
#define MAX17048_CMD                    0xFF
#define MAX17048_UNLOCK_VALUE           0x4a57
#define MAX17048_RESET_VALUE            0x5400

#if !defined(CONFIG_MAX17048_SOC_ALERT) || defined(CONFIG_MAX17048_POLLING)
#define MAX17048_POLLING_PERIOD         60000
#endif

#ifdef CONFIG_MAX17048_POLLING
#define MAX17048_POLLING_PERIOD_7       10000
#define MAX17048_POLLING_PERIOD_3       5000
#endif

#ifdef CONFIG_MAX17048_LOW_POLLING
#define MAX17048_LOW_POLLING_PERIOD     5000
#endif

#define MAX17048_BATTERY_FULL           100
#define MAX17048_BATTERY_LOW            15
#define MAX17048_VERSION_NO             0x11
#define MAX17048_VERSION_NO2            0x12

struct max17048_chip {
	struct i2c_client		*client;
	struct delayed_work		work;
#ifdef CONFIG_MAX17048_POLLING
	struct delayed_work		polling_work;
#endif
#ifdef CONFIG_MAX17048_LOW_POLLING
	struct delayed_work		low_polling_work;
#endif
	struct max17048_battery_model	*model_data;
	/* battery voltage */
	int vcell;
	/* battery capacity */
	int soc;
	/* battery capacity */
	/* Capacity_level modified for indicating custom model SOC */
	int capacity_level;

	int lasttime_vcell;
	int lasttime_soc;
	int lasttime_status;
#ifdef CONFIG_LGE_PM
	struct work_struct	alert_work;
	struct wake_lock	alert_lock;
	uint16_t config;
	uint16_t status;
	int alert_gpio;
	int voltage;
	int lasttime_voltage;
	int lasttime_capacity_level;
	int state;
	struct power_supply *batt_psy;
#endif
#ifdef CONFIG_BATTERY_EXTERNAL_FUELGAUGE
	struct power_supply battery;
#endif
};

#ifdef CONFIG_LGE_PM
static struct max17048_chip *ref;
int lge_power_test_flag = 1;
#endif

static int max17048_write_word(struct i2c_client *client, int reg, u16 value)
{
	int ret;

	ret = i2c_smbus_write_word_data(client, reg, swab16(value));

	if (ret < 0)
		pr_err("%s(): Failed in writing register 0x%02x err %d\n",
			__func__, reg, ret);

	return ret;
}

static int max17048_read_word(struct i2c_client *client, int reg)
{
	int ret;

	ret = i2c_smbus_read_word_data(client, reg);

	if (ret < 0) {
		pr_err("%s(): Failed in reading register 0x%02x err %d\n",
			__func__, reg, ret);
		return ret;
	} else {
		ret = (int)swab16((uint16_t)(ret & 0x0000ffff));
		return ret;
	}
}

#ifdef CONFIG_LGE_PM
static int max17048_get_config(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int config;

	config = max17048_read_word(client, MAX17048_CONFIG);
	if (config < 0) {
		pr_err(" %s: err %d\n", __func__, config);
		return config;
	} else {
		pr_err("%s : config = 0x%x\n", __func__, config);
		chip->config = config;
		return 0;
	}
}

static int max17048_get_status(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int status;

	status = max17048_read_word(client, MAX17048_STATUS);
	if (status < 0) {
		pr_err("%s: err %d\n", __func__, status);
		return status;
	} else {
		pr_err("%s : status = 0x%x\n", __func__, status);
		chip->status = status;
		return 0;
	}
}

/* Using Quickstart instead of reset for Power Test
*  DO NOT USE THIS COMMAND ANOTHER SCENE.
*/
static int max17048_set_reset(struct i2c_client *client)
{
	max17048_write_word(client, MAX17048_MODE, 0x4000);

	pr_info("MAX17048 Reset(Quickstart)\n");

	return 0;
}

/* Calculate MAX17048 custom model SOC */
static int max17048_get_capacity_from_soc(void)
{
	u8 buf[2];
	long batt_soc = 0;

	if (ref == NULL)
		return 100;

	buf[0] = (ref->soc & 0x0000FF00) >> 8;
	buf[1] = (ref->soc & 0x000000FF);

	/* The unit of bit[0] is 1/512 %, Change it to 1% */
	batt_soc = ((buf[0]*256)+buf[1])*19531;

	pr_debug("[max17048]%s : MAXIM Raw Capacity : %d , *100 is %d\n"
			, __func__, (int)(batt_soc/10000000)
			, (int)(batt_soc/100000));

	/* SOC scaling for stable max SOC and changed Cut-off */
	/*Adj SOC = (FG SOC-Emply)/(Full-Empty)*100*/
	batt_soc = (batt_soc-((ref->model_data->empty)*100000))
		/(9400-(ref->model_data->empty))*10000;
	batt_soc /= 10000000;

	if (batt_soc > 100)
		batt_soc = 100;
	else if (batt_soc < 0)
		batt_soc = 0;
#ifdef CONFIG_LGE_UPSCALING_LOW_SOC
	/* Report 0.42%(0x300) ~ 1% to 1% */
	/* If Full and Emplty is changed, need to modify the value, 3 */
	else if (batt_soc == 0 && buf[0] >= 3) {
		pr_err("%s : buf[0] is %d, upscale to 1%%\n"
				, __func__, buf[0]);
		batt_soc = 1;
	}
#endif
	return batt_soc;
}
#endif

static void max17048_get_vcell(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int vcell;

	vcell = max17048_read_word(client, MAX17048_VCELL);
	if (vcell < 0)
		pr_err("%s: err %d\n", __func__, vcell);
	else {
		chip->vcell = vcell >> 4;
		chip->voltage = (chip->vcell * 5) >> 2;
	}

}

static void max17048_get_soc(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int soc;

	soc = max17048_read_word(client, MAX17048_SOC);
	if (soc < 0)
		pr_err("%s: err %d\n", __func__, soc);
	else {
		chip->soc = soc;
		chip->capacity_level = max17048_get_capacity_from_soc();
	}
}

static uint16_t max17048_get_version(struct i2c_client *client)
{
	return swab16(i2c_smbus_read_word_data(client, MAX17048_VER));
}

#ifdef CONFIG_LGE_SHOW_OCV
static void max17048_get_ocv(struct i2c_client *client)
{
	u8 values[2];
	int batt_mv;
	int org_ocv;

	/* Unlock Model Access */
	max17048_write_word(client, MAX17048_UNLOCK, MAX17048_UNLOCK_VALUE);

	/* Read OCV */
	org_ocv = max17048_read_word(client, MAX17048_OCV);
	if (org_ocv == 0xFFFF) {
		pr_err("%s: unlock fail %x\n",	__func__, org_ocv);
	} else {
		values[0] = (org_ocv & 0xff00)>>8;
		values[1] = org_ocv & 0x00ff;
		batt_mv = ((values[0] << 4) + (values[1] >> 4));
		batt_mv = (batt_mv*125)/100;
		pr_info("%s : ocv is 0x%02x%02x - %d\n",
			__func__, values[0], values[1], batt_mv);

		/*Lock Model Access */
		max17048_write_word(client, MAX17048_UNLOCK, 0x0000);
	}
}
#endif

#ifdef CONFIG_MAX17048_LOW_POLLING
static void max17048_low_polling_work(struct work_struct *work)
{
	struct max17048_chip *chip;

	chip = container_of(work, struct max17048_chip, low_polling_work.work);
	if (chip == NULL) {
		pr_err("%s : Called before init.\n", __func__);
		return;
	}
#ifdef CONFIG_LGE_SHOW_OCV
	max17048_get_ocv(chip->client);
#endif
	max17048_get_soc(chip->client);

	if (chip->capacity_level != chip->lasttime_capacity_level) {
		chip->lasttime_soc = chip->soc;
		chip->lasttime_capacity_level = chip->capacity_level;

		if (!chip->batt_psy) {
			chip->batt_psy = power_supply_get_by_name("battery");

			if (!chip->batt_psy)
				goto psy_error;
		}

		pr_info("%s : Reported Capacity : %d / voltage : %d\n",
				__func__, chip->capacity_level, chip->voltage);

		power_supply_changed(chip->batt_psy);
	}

	schedule_delayed_work(&chip->low_polling_work,
		round_jiffies_relative(msecs_to_jiffies(
			MAX17048_LOW_POLLING_PERIOD)));
	return;

psy_error:
	pr_info("%s : batt_psy is not init yet!\n", __func__);

	schedule_delayed_work(&chip->low_polling_work,
		round_jiffies_relative(msecs_to_jiffies(
			MAX17048_LOW_POLLING_PERIOD)));
}
#endif


#ifdef CONFIG_MAX17048_POLLING
/*  func : max17048_polling_work
 *  This work func  just print MAX17048 reg. value at kernel log
 *  and does not update it at battery power supply
 *  It means that battery data from this func. is not delivered to user
 */
static void max17048_polling_work(struct work_struct *work)
{
	struct max17048_chip *chip;
	int vcell = 0, soc = 0, capacity = 0, voltage = 0;
	u8 buf[2];

	chip = container_of(work, struct max17048_chip, polling_work.work);

	if (chip == NULL) {
		pr_info("%s : Called before init.\n", __func__);
		return;
	}

	/* Read VCELL */
	vcell = max17048_read_word(chip->client, MAX17048_VCELL);
	if (vcell < 0) {
		pr_err("%s: error get vcell register %d\n",
			__func__, vcell);
	} else {
		vcell = vcell >> 4;
		voltage  = (vcell * 5) >> 2;
	}

	/* Read SOC */
	soc = max17048_read_word(chip->client, MAX17048_SOC);
	if (soc < 0) {
		pr_err("%s: error get soc register %d\n",
			__func__, soc);
	} else {
		buf[0] = (soc & 0x0000FF00) >> 8;
		buf[1] = (soc & 0x000000FF);

		/* The unit of bit[0] is 1/512 %, Change it to 1% */
		capacity = ((buf[0]*256)+buf[1])*19531;

		pr_debug("%s : Raw Capacity : %d , *100 is %d\n"
			, __func__, (int)(capacity/10000000)
			, (int)(capacity/100000));

		/* SOC scaling for stable max SOC and changed Cut-off */
		/* Adj SOC = (FG SOC-Emply) / (Full-Empty) * 100 */
		capacity = (capacity-((ref->model_data->empty)*100000))
			/(9400-(ref->model_data->empty))*10000;

		capacity /= 10000000;

		/* Report 0.42%(0x300) ~ 1% to 1% */
		/* If Full and Emplty is changed, need to modify the value, 3 */
		if (capacity == 0 && buf[0] >= 3) {
			pr_err("%s : buf[0] is %d, upscale to 1%%\n"
				, __func__, buf[0]);
			capacity = 1;
		}
	}

	pr_err("%s : soc : 0x%x / vcell : 0x%x\n",
			__func__, soc, vcell);
	pr_err("%s : capacity : %d  / voltage : %d\n",
			__func__, capacity, voltage);

	if (8 < capacity) {
		schedule_delayed_work(&chip->polling_work,
				round_jiffies_relative(msecs_to_jiffies(
					MAX17048_POLLING_PERIOD)));
	}

	if (3 < capacity && capacity < 8) {
		/* 4%~7% 10sec polling */
		schedule_delayed_work(&chip->polling_work,
			round_jiffies_relative(msecs_to_jiffies(
				MAX17048_POLLING_PERIOD_7)));
	} else {
		/* 0%~3% 5sec polling */
		schedule_delayed_work(&chip->polling_work,
			round_jiffies_relative(msecs_to_jiffies(
				MAX17048_POLLING_PERIOD_3)));
	}
	return;
}
#endif

static void max17048_work(struct work_struct *work)
{
	struct max17048_chip *chip;
#ifdef CONFIG_LGE_PM
	int ret = 0;
#endif

#ifdef MAX17048_DEBUG
	pr_info("%s.\n", __func__);
#endif

	chip = container_of(work, struct max17048_chip, work.work);
#ifdef CONFIG_LGE_PM
	if (chip == NULL) {
		pr_err("%s : Called before init.\n", __func__);
		return;
	}
#ifdef CONFIG_MAX17048_SOC_ALERT
	disable_irq(gpio_to_irq(chip->model_data->alert_gpio));
#endif

	ret = max17048_get_config(chip->client);
	if (ret < 0)
		pr_err("%s : error get config register.\n",
			__func__);

	ret = max17048_get_status(chip->client);
	if (ret < 0)
		pr_err("%s : error get status register.\n",
			__func__);
#endif

#ifdef CONFIG_LGE_SHOW_OCV
	max17048_get_ocv(chip->client);
#endif
	/* Update recently VCELL, SOC and CAPACITY */
	max17048_get_vcell(chip->client);
	max17048_get_soc(chip->client);

	pr_err("%s : Raw SOC : 0x%x / vcell : 0x%x\n",
		__func__, chip->soc, chip->vcell);

#ifdef CONFIG_LGE_PM
	if (chip->capacity_level != chip->lasttime_capacity_level) {
		chip->lasttime_voltage = chip->voltage;
		chip->lasttime_soc = chip->soc;
		chip->lasttime_capacity_level = chip->capacity_level;

		pr_err("%s : Reported Capacity : %d / voltage : %d\n",
				__func__, chip->capacity_level, chip->voltage);

		if (!chip->batt_psy) {
			chip->batt_psy = power_supply_get_by_name("battery");

			if (!chip->batt_psy)
				goto psy_error;
		}

		power_supply_changed(chip->batt_psy);
	}
#else
	if (chip->vcell != chip->lasttime_vcell ||
		chip->soc != chip->lasttime_soc ||
		chip->status !=	chip->lasttime_status) {
		chip->lasttime_vcell = chip->vcell;
		chip->lasttime_soc = chip->soc;

		power_supply_changed(&chip->battery);
	}
#endif

#ifdef CONFIG_MAX17048_LOW_POLLING
	if (chip->capacity_level < 3) {
		schedule_delayed_work(&chip->low_polling_work,
			round_jiffies_relative(msecs_to_jiffies(
				MAX17048_LOW_POLLING_PERIOD)));
	} else {
		cancel_delayed_work(&chip->low_polling_work);
	}
#endif

#ifdef CONFIG_MAX17048_SOC_ALERT
	enable_irq(gpio_to_irq(chip->model_data->alert_gpio));
#else
	schedule_delayed_work(&chip->work,
		round_jiffies_relative(msecs_to_jiffies(
			MAX17048_POLLING_PERIOD)));
#endif
	return;

psy_error:
	pr_err("%s : batt_psy is not init yet!\n", __func__);
#ifdef CONFIG_MAX17048_SOC_ALERT
	enable_irq(gpio_to_irq(chip->model_data->alert_gpio));
#else
	schedule_delayed_work(&chip->work,
		round_jiffies_relative(msecs_to_jiffies(
			MAX17048_POLLING_PERIOD)));
#endif
}

#ifdef CONFIG_LGE_PM
static irqreturn_t max17048_interrupt_handler(int irq, void *data)
{
	struct max17048_chip *chip = data;
	pr_err("%s : MAX17048 interupt occured\n", __func__);

	if (chip == NULL) {
		pr_err("%s : called before init.\n", __func__);
		return IRQ_HANDLED;
	}

	if (chip->state)
		schedule_work(&chip->alert_work);

	return IRQ_HANDLED;
}

static int max17048_clear_interrupt(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;
	pr_info("%s.\n", __func__);
	if (chip == NULL)
		return -ENODEV;

	/* Clear low battery alert config register */
	if (chip->config & 0x0020) {
		chip->config &= 0xffdf;
		ret = max17048_write_word(chip->client,
				MAX17048_CONFIG, chip->config);
		if (ret < 0)
			return ret;
	}

	/* Clear All setted status register */
	if (chip->status & 0xFF00) {
		chip->status &= 0x00FF;
		ret = max17048_write_word(chip->client,
				MAX17048_STATUS, chip->status);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int max17048_set_athd_alert(struct i2c_client *client, int level)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;
	pr_info("%s.\n", __func__);
	if (chip == NULL)
		return -ENODEV;

	if (level > 32)
		level = 32;
	else if (level < 1)
		level = 1;

	level = 32 - level;

	/* If same LEVEL set previous value */
	if (level == (chip->config & 0x1F))
		return level;

	/* New LEVEL set and Set alert threshold flag */

	chip->config = ((chip->config & 0xffe0) | level);

	ret = max17048_write_word(chip->client,
			MAX17048_CONFIG, chip->config);
	if (ret < 0)
		return ret;

	return level;
}

static int max17048_set_alsc_alert(struct i2c_client *client,
	int enable)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;
	pr_info("%s. with %d\n", __func__, enable);
	if (chip == NULL)
		return -ENODEV;

	if (enable) {
		/* Enable SOC change alert */
		if (!(chip->config & 0x0040)) {
			chip->config |= 0x0040;
			ret = max17048_write_word(chip->client,
					MAX17048_CONFIG, chip->config);
			if (ret < 0)
				return ret;
		}
	} else {
		/* Disable SOC change alert */
		if (chip->config & 0x0040) {
			chip->config &= 0xFFBF;
			ret = max17048_write_word(chip->client,
					MAX17048_CONFIG, chip->config);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

static int max17048_set_rcomp(struct i2c_client *client, int rcomp)
{

	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret = 0;

	if (chip == NULL)
		return -ENODEV;

	rcomp &= 0xff;
	chip->config = ((chip->config & 0x00ff) | (rcomp << 8));

	ret = max17048_write_word(chip->client,
			MAX17048_CONFIG, chip->config);

	return ret;
}

#define IDLE_BATT_TEMP 250
int max17048_set_rcomp_by_temperature(struct i2c_client *client)
{
	int init_rcomp;
	int temp_hot;
	int temp_cold;
	int pre_rcomp = 0, new_rcomp = 0;
	int temp;
	int rc;
	union power_supply_propval ret = {0,};

	struct max17048_chip *chip = i2c_get_clientdata(client);

	/* if fuel gauge is not initialized, */
	if (ref == NULL || chip == NULL)
		return -ENODEV;
	else {
		init_rcomp = ref->model_data->rcomp;
		temp_hot =  ref->model_data->temp_co_hot;
		temp_cold = ref->model_data->temp_co_cold;
	}
	if (!chip->batt_psy) {
		chip->batt_psy = power_supply_get_by_name("battery");
		if (!chip->batt_psy) {
			pr_err("%s : batt_psy is not init yet!\n", __func__);
			return -ENODEV;
		}
	}

	chip->batt_psy->get_property(chip->batt_psy,
			POWER_SUPPLY_PROP_TEMP, &ret);

	temp = ret.intval;

	temp /= 10;

	/* Read RCOMP applied*/
	pre_rcomp = max17048_read_word(client, MAX17048_CONFIG);
	pre_rcomp = (pre_rcomp & 0xFF00) >> 8;

	if (pre_rcomp < 0)
		return pre_rcomp;

	/* Calculate RCOMP by temperature*/
	if (temp > 20)
		new_rcomp = init_rcomp + (int)((temp - 20)*temp_hot/(-1000));
	else if (temp < 20)
		new_rcomp = init_rcomp + (int)((temp - 20)*temp_cold/(-1000));
	else
		new_rcomp = init_rcomp;

	if (new_rcomp > 0xFF)
		new_rcomp = 0xFF;
	else if (new_rcomp < 0)
		new_rcomp = 0;

	pr_err("%s : temp = %d, pre_rcomp = 0x%02X -> new_rcomp = 0x%02X\n"
		, __func__ , temp, pre_rcomp, new_rcomp);

	/* Write RCOMP */
	if (new_rcomp != pre_rcomp) {
		rc = max17048_set_rcomp(chip->client, new_rcomp);
		if (rc < 0)
			pr_info("failed to write RCOMP\n");
	}
	return 0;
}


static void max17048_alert_work(struct work_struct *work)
{
	struct max17048_chip *chip = container_of(work,
			struct max17048_chip, alert_work);
	int ret;

	wake_lock(&chip->alert_lock);

#ifdef MAX17048_DEBUG
	pr_info("%s.\n", __func__);
#endif

	if (chip == NULL) {
		pr_info("%s : Called before init.\n", __func__);
		goto error;
	}

	/* MAX17048 update register */
	max17048_work(&(chip->work.work));

	/* Clear Interrupt status */
	ret = max17048_clear_interrupt(chip->client);
	if (ret < 0)
		pr_info("%s : error clear alert interrupt register.\n",
				__func__);

	max17048_set_rcomp_by_temperature(chip->client);

error:
	wake_unlock(&chip->alert_lock);
	return;
}

/* Sysfs & Export symbol for other device */
ssize_t max17048_show_voltage(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	int level;

	if (ref == NULL)
		return snprintf(buf, PAGE_SIZE, "ERROR\n");

	if (lge_power_test_flag == 1) {
#ifdef CONFIG_MAX17048_SOC_ALERT
		disable_irq(gpio_to_irq(ref->model_data->alert_gpio));
#else
		cancel_delayed_work(&ref->work);
#endif
		/* Wait for Fuel-gauge stability */
		msleep(1000);
		max17048_work(&(ref->work.work));

#ifdef CONFIG_MAX17048_SOC_ALERT
		enable_irq(gpio_to_irq(ref->model_data->alert_gpio));
#else
		schedule_delayed_work(&ref->work, HZ);
#endif
	}

	level = ref->voltage;
	return snprintf(buf, PAGE_SIZE, "%d\n", level);
}
DEVICE_ATTR(voltage, 0444, max17048_show_voltage, NULL);

ssize_t max17048_show_capacity(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	int level = 0;

	if (ref == NULL)
		return snprintf(buf, PAGE_SIZE, "ERROR\n");

	if (lge_power_test_flag == 1) {
#ifdef CONFIG_MAX17048_SOC_ALERT
		disable_irq(gpio_to_irq(ref->model_data->alert_gpio));
#else
		cancel_delayed_work(&ref->work);
#endif
		/* Wait for Fuel-gauge stability */
		msleep(1000);
		max17048_set_reset(ref->client);

		/* Wait for re-calculate values by Fuel-gauge */
		msleep(300);

		max17048_work(&(ref->work.work));

#ifdef CONFIG_MAX17048_SOC_ALERT
		enable_irq(gpio_to_irq(ref->model_data->alert_gpio));
#else
		schedule_delayed_work(&ref->work, HZ);
#endif
	}

	level = ref->capacity_level;
	return snprintf(buf, PAGE_SIZE, "%d\n", level);
}
DEVICE_ATTR(capacity, 0444, max17048_show_capacity, NULL);

ssize_t max17048_store_status(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf,
			  size_t count)
{
	if (ref == NULL)
		return -ENODEV;

	if (strncmp(buf, "reset", 5) == 0) {
#ifndef CONFIG_MAX17048_SOC_ALERT
		cancel_delayed_work(&ref->work);
		max17048_set_reset(ref->client);
		schedule_delayed_work(&ref->work, HZ);
#else
		disable_irq(gpio_to_irq(ref->model_data->alert_gpio));
		max17048_set_reset(ref->client);
		enable_irq(gpio_to_irq(ref->model_data->alert_gpio));
#endif
	} else {
		return -EINVAL;
	}
	return count;
}
DEVICE_ATTR(fuelrst, 0220, NULL, max17048_store_status);

int max17048_get_voltage(void)
{
	/* if fuel gauge is not initialized, */
	if (ref == NULL)
		return 4350;

	return ref->voltage;
}
EXPORT_SYMBOL(max17048_get_voltage);

int max17048_get_capacity(void)
{
	/* if fuel gauge is not initialized, */
	if (ref == NULL)
		return 100;

	return ref->capacity_level;
}
EXPORT_SYMBOL(max17048_get_capacity);

int max17048_get_fulldesign(void)
{
	/* if fuel gauge is not initialized, */
	if (ref == NULL)
		return 2000;

	return ref->model_data->full_design;
}
EXPORT_SYMBOL(max17048_get_fulldesign);
#endif

#ifdef CONFIG_BATTERY_EXTERNAL_FUELGAUGE
static int max17048_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = max17048_get_voltage();
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = max17048_get_capacity();
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = max17048_get_fulldesign();
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property max17048_battery_props[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

static char *pm_power_supplied_to[] = {
	"battery",
};

static struct power_supply max17048_ps = {
	.name = "fuelgauge",
	.type = POWER_SUPPLY_TYPE_FUELGAUGE,
	.supplied_to = pm_power_supplied_to,
	.num_supplicants = ARRAY_SIZE(pm_power_supplied_to),
	.properties = max17048_battery_props,
	.num_properties = ARRAY_SIZE(max17048_battery_props),
	.get_property = max17048_get_property,
};
#endif

#ifdef CONFIG_OF
static int max17048_parse_dt(struct device *dev,
		struct max17048_battery_model *mdata)
{
	struct device_node *dev_node = dev->of_node;

	int rc = 0;

#ifdef CONFIG_LGE_PM
	mdata->alert_gpio = of_get_named_gpio(dev_node,
			"max17048,alert_gpio", 0);
	pr_err("%s : Get alert_gpio %d\n", __func__,
			mdata->alert_gpio);
#endif

	rc = of_property_read_u32(dev_node, "max17048,alert_threshold",
			&mdata->alert_threshold);
	rc = of_property_read_u32(dev_node, "max17048,full_design",
			&mdata->full_design);
	rc = of_property_read_u32(dev_node, "max17048,rcomp",
			&mdata->rcomp);
	rc = of_property_read_u32(dev_node, "max17048,temp_co_hot",
			&mdata->temp_co_hot);
	rc = of_property_read_u32(dev_node, "max17048,temp_co_cold",
			&mdata->temp_co_cold);
	rc = of_property_read_u32(dev_node, "max17048,empty",
			&mdata->empty);

	return rc;
}
#endif

#define BATT_NOT_PRESENT 200 /*in case of battery not presence */
static int max17048_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct max17048_chip *chip;
#ifdef CONFIG_LGE_PM
	struct max17048_battery_model *mdata;
#endif
	int ret = 0;
	uint16_t version;

	pr_info("[MAX17048] probe : START\n");

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

#ifdef CONFIG_LGE_PM
	if (ret)
		goto error;
#endif

	if (!i2c_check_functionality(client->adapter,
				I2C_FUNC_SMBUS_BYTE_DATA)) {
		pr_err(" %s : i2c_check_functionality fail\n", __func__);
		return -EIO;
	}

	chip->client = client;

/* Parsing Device Tress or Using Device Platform data */
#ifdef CONFIG_OF
	if (&client->dev.of_node) {
		mdata = devm_kzalloc(&client->dev,
				sizeof(struct max17048_battery_model),
				GFP_KERNEL);

		if (mdata == NULL)
			return -ENOMEM;

		chip->model_data = mdata;

		ret = max17048_parse_dt(&client->dev, mdata);
		if (ret != 0)
			return ret;
	} else {
		chip->model_data = client->dev.platform_data;
	}
#else
	chip->model_data = client->dev.platform_data;
#endif

	i2c_set_clientdata(client, chip);

	version = max17048_get_version(client);
	pr_info("MAX17048 Fuel-Gauge Ver 0x%x\n", version);

	if ((version != MAX17048_VERSION_NO)
		&& (version != MAX17048_VERSION_NO2)) {
		ret = -ENODEV;
		goto error;
	}

	ref = chip;

#ifdef CONFIG_LGE_PM
	/* MAX17048 alert gpio setting */
	if (chip->model_data->alert_gpio) {
		ret = gpio_request(chip->model_data->alert_gpio,
				"max17048_alert");
		if (ret < 0) {
			pr_err("%s : GPIO Request Failed : return %d\n",
					__func__, ret);
			goto err_gpio_request_failed;
		}

		gpio_direction_input(chip->model_data->alert_gpio);

		ret = request_irq(gpio_to_irq(chip->model_data->alert_gpio),
				max17048_interrupt_handler,
				IRQF_TRIGGER_FALLING,
				"MAX17048_Alert", chip);
		if (ret < 0) {
			pr_err("%s : IRQ Request Failed : return %d\n",
					__func__, ret);
			goto err_request_irq_failed;
		}

		ret = enable_irq_wake(gpio_to_irq(
			chip->model_data->alert_gpio));
		if (ret < 0) {
			pr_err("[MAX17043] set irq to wakeup source failed.\n");
			goto err_request_wakeup_irq_failed;
		}
#ifdef CONFIG_MAX17048_SOC_ALERT
		disable_irq(gpio_to_irq(chip->model_data->alert_gpio));
#endif
	}

	/* sysfs path : /sys/bus/i2c/devices/84-0036/voltage */
	ret = device_create_file(&client->dev, &dev_attr_voltage);
	if (ret < 0) {
		pr_err("%s:File device creation failed: %d\n", __func__, ret);
		ret = -ENODEV;
		goto err_create_file_voltage_failed;
	}
	/* sysfs path : /sys/bus/i2c/devices/84-0036/capacity */
	ret = device_create_file(&client->dev, &dev_attr_capacity);
	if (ret < 0) {
		pr_err("%s:File device creation failed: %d\n", __func__, ret);
		ret = -ENODEV;
		goto err_create_file_capacity_failed;
	}
	/* sysfs path : /sys/bus/i2c/devices/84-0036/fuelrst */
	ret = device_create_file(&client->dev, &dev_attr_fuelrst);
	if (ret < 0) {
		pr_err("%s:File device creation failed: %d\n", __func__, ret);
		ret = -ENODEV;
		goto err_create_file_fuelrst_failed;
	}
#endif

	INIT_DELAYED_WORK(&chip->work, max17048_work);
#ifdef CONFIG_MAX17048_POLLING
	INIT_DELAYED_WORK(&chip->polling_work, max17048_polling_work);
#endif
#ifdef CONFIG_MAX17048_LOW_POLLING
	INIT_DELAYED_WORK(&chip->low_polling_work, max17048_low_polling_work);
#endif
#ifndef CONFIG_LGE_PM
	schedule_delayed_work(&chip->work,
		round_jiffies_relative(
		msecs_to_jiffies(MAX17048_POLLING_PERIOD)));
#else
	INIT_WORK(&chip->alert_work, max17048_alert_work);
	wake_lock_init(&chip->alert_lock, WAKE_LOCK_SUSPEND, "max17048_alert");


	/* Update recently status register & clears the reset indicator */
	max17048_get_status(client);
	ret = max17048_write_word(client,
			MAX17048_STATUS, chip->status & 0xFEFF);
	if (ret < 0)
		goto err_i2c_write_failed;

	/* Update recently config register */
	max17048_get_config(client);
	/* Set RCOMP*/
	max17048_set_rcomp_by_temperature(chip->client);
	/* Set low battery alert threshold */
	max17048_set_athd_alert(client, chip->model_data->alert_threshold);

	/* Set register following each mode define */
#ifdef CONFIG_MAX17048_SOC_ALERT
	max17048_set_alsc_alert(client, 1);
	chip->state = 1;
#else
	max17048_set_alsc_alert(client, 0);
#endif
	max17048_work(&(chip->work.work));

#ifdef CONFIG_MAX17048_POLLING
	schedule_delayed_work(&chip->polling_work,
		round_jiffies_relative(
			msecs_to_jiffies(MAX17048_POLLING_PERIOD)));
#endif

	/* Clear CONFIG.ATHD and Any setted STATUS register flag */
	max17048_clear_interrupt(client);
#ifdef CONFIG_MAX17048_SOC_ALERT
	enable_irq(gpio_to_irq(chip->model_data->alert_gpio));
#endif
#endif

#ifdef CONFIG_BATTERY_EXTERNAL_FUELGAUGE
	chip->battery = max17048_ps;

	ret = power_supply_register(&chip->client->dev, &chip->battery);
	if (ret < 0)
		pr_err("%s : failed resiter fuelgauge power supply\n", __func__);
#endif

	pr_info("[MAX17048] probe : DONE\n");
	return 0;

err_i2c_write_failed:
	wake_lock_destroy(&chip->alert_lock);
err_create_file_fuelrst_failed:
	device_remove_file(&client->dev, &dev_attr_capacity);
err_create_file_capacity_failed:
	device_remove_file(&client->dev, &dev_attr_voltage);
err_create_file_voltage_failed:
	disable_irq_wake(gpio_to_irq(chip->model_data->alert_gpio));
err_request_wakeup_irq_failed:
	free_irq(gpio_to_irq(chip->model_data->alert_gpio), NULL);
err_request_irq_failed:
	gpio_free(chip->model_data->alert_gpio);
err_gpio_request_failed:
error:
	kfree(chip);
	ref = NULL;

	return ret;
}

static int max17048_remove(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);

	device_remove_file(&client->dev, &dev_attr_voltage);
	device_remove_file(&client->dev, &dev_attr_capacity);
	device_remove_file(&client->dev, &dev_attr_fuelrst);

	wake_lock_destroy(&chip->alert_lock);

#ifdef CONFIG_MAX17048_POLLING
	cancel_delayed_work(&chip->polling_work);
#endif

#ifdef CONFIG_MAX17048_LOW_POLLING
	cancel_delayed_work(&chip->low_polling_work);
#endif

#ifndef CONFIG_MAX17048_SOC_ALERT
	cancel_delayed_work(&chip->work);
#endif

#ifdef CONFIG_BATTERY_EXTERNAL_FUELGAUGE
	power_supply_unregister(&chip->battery);
#endif

	kfree(chip);
	return 0;
}

#ifdef CONFIG_PM

static int max17048_suspend(struct i2c_client *client,
		pm_message_t state)
{
	if (ref == NULL)
		return 0;

#ifdef CONFIG_MAX17048_POLLING
	cancel_delayed_work(&ref->polling_work);
#endif

#ifdef CONFIG_MAX17048_LOW_POLLING
	cancel_delayed_work(&ref->low_polling_work);
#endif

#ifndef CONFIG_MAX17048_SOC_ALERT
	cancel_delayed_work(&ref->work);
#else
	/* Disable ALSC(1% alert) */
	max17048_set_alsc_alert(client, 0);
	ref->state = 0;
#endif
	return 0;
}

static int max17048_resume(struct i2c_client *client)
{
	if (ref == NULL)
		return 0;

#ifdef CONFIG_MAX17048_POLLING
	schedule_delayed_work(&ref->polling_work, MAX17048_POLLING_PERIOD);
#endif

#ifndef CONFIG_MAX17048_SOC_ALERT
	schedule_delayed_work(&ref->work, HZ);
#else
	max17048_work(&(ref->work.work));
	ref->state = 1;
	/* Enable ALSC(1% alert) */
	max17048_set_alsc_alert(client, 1);
#endif
	return 0;
}

#else

#define max17048_suspend NULL
#define max17048_resume NULL

#endif /* CONFIG_PM */

#ifdef CONFIG_OF
static struct of_device_id max17048_match_table[] = {
	{ .compatible = "maxim,max17048", },
	{ },
};
#endif

static const struct i2c_device_id max17048_id[] = {
	{ "max17048", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, max17048_id);

static struct i2c_driver max17048_i2c_driver = {
	.driver	= {
		.name	= "max17048",
#ifdef CONFIG_LGE_PM
		.owner	= THIS_MODULE,
#endif
#ifdef CONFIG_OF
		.of_match_table = max17048_match_table,
#endif
	},
	.probe		= max17048_probe,
	.remove		= max17048_remove,
	.suspend	= max17048_suspend,
	.resume		= max17048_resume,
	.id_table	= max17048_id,
};

static int __init max17048_init(void)
{
	return i2c_add_driver(&max17048_i2c_driver);
}
module_init(max17048_init);

static void __exit max17048_exit(void)
{
	i2c_del_driver(&max17048_i2c_driver);
}
module_exit(max17048_exit);

MODULE_AUTHOR("Chandler Zhang <chazhang@nvidia.com>");
MODULE_DESCRIPTION("MAX17048 Fuel Gauge");
MODULE_LICENSE("GPL");
