// SPDX-License-Identifier: GPL-2.0-only
/*
 * Edgecore AS5114-48X CPLD hwmon driver
 *
 * Copyright (C) 2021 Sartura Ltd.
 *
 * Author: Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>

#define AS5114_TACH_SPEED_SETTING 	0x62
#define AS5114_TACH_SPEED_CLOCK_MASK	GENMASK(7, 6)
#define AS5114_TACH_SPEED_COUNTER_MASK	GENMASK(5, 0)

#define AS5114_FAN1_PWM		0x70
#define AS5114_FAN2_PWM		0x71
#define AS5114_FAN3_PWM		0x72
#define AS5114_FAN4_PWM		0x73
#define AS5114_FAN5_PWM		0x74
#define AS5114_FAN_MIN_DUTY	76	/* 30% */
#define AS5114_FAN_MAX_DUTY	255	/* 100% */

#define AS5114_FAN1_TACH	0x80
#define AS5114_FAN2_TACH	0x81
#define AS5114_FAN3_TACH	0x82
#define AS5114_FAN4_TACH	0x83
#define AS5114_FAN5_TACH	0x84

struct as5114_hwmon_data {
	struct regmap *regmap;
};

static int as5114_fan_pwm_read(struct as5114_hwmon_data *data, int channel,
			       long *val)
{
	unsigned int regval;
	int err;

	err = regmap_read(data->regmap, AS5114_FAN1_PWM + channel, &regval);
	if (err < 0)
		return err;

	*val = regval;

	return 0;
}

static int as5114_fan_pwm_write(struct as5114_hwmon_data *data, int channel,
				long val)
{
	if (val < AS5114_FAN_MIN_DUTY ||
	    val > AS5114_FAN_MAX_DUTY)
		return -EINVAL;

	return regmap_write(data->regmap, AS5114_FAN1_PWM + channel, val);
}

static int as5114_fan_tach_read(struct as5114_hwmon_data *data, int channel,
				long *val)
{
	u32 tach_timer_values[] = { 1048, 2097, 4194, 8389 };
	u8 tach_counter, tach_clock;
	unsigned int regval;
	int err;

	err = regmap_read(data->regmap, AS5114_TACH_SPEED_SETTING, &regval);
	if (err < 0)
		return err;

	tach_counter = FIELD_GET(AS5114_TACH_SPEED_COUNTER_MASK, regval);
	tach_clock = FIELD_GET(AS5114_TACH_SPEED_CLOCK_MASK, regval);

	err = regmap_read(data->regmap, AS5114_FAN1_TACH + channel, &regval);
	if (err < 0)
		return err;

	*val = (regval * 3000000) / (tach_timer_values[tach_clock] * tach_counter);

	return 0;
}

static umode_t as5114_is_visible(const void *data, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			return 0444;
		default:
			return 0;
		}
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return 0644;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static int as5114_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *val)
{
	struct as5114_hwmon_data *data = dev_get_drvdata(dev);
	int err;

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			err = as5114_fan_pwm_read(data, channel, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			err = as5114_fan_tach_read(data, channel, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static int as5114_write(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long val)
{
	struct as5114_hwmon_data *data = dev_get_drvdata(dev);
	int err;

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			err = as5114_fan_pwm_write(data, channel, val);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static const struct hwmon_channel_info *as5114_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_ops as5114_hwmon_ops = {
	.is_visible = as5114_is_visible,
	.write = as5114_write,
	.read = as5114_read,
};

static const struct hwmon_chip_info as5114_chip_info = {
	.ops = &as5114_hwmon_ops,
	.info = as5114_info,
};

static int as5114_hwmon_probe(struct platform_device *pdev)
{
	struct as5114_hwmon_data *data;
	struct device *hwmon_dev;
	struct regmap *regmap;

	if (!pdev->dev.parent)
		return -ENODEV;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap)
		return -ENODEV;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = regmap;

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, pdev->name,
							 data, &as5114_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	return 0;
}

static const struct of_device_id as5114_hwmon_of_match[] = {
	{ .compatible = "edgecore,as5114-hwmon", },
	{ },
};
MODULE_DEVICE_TABLE(of, as5114_hwmon_of_match);

static struct platform_driver as5114_hwmon_driver = {
	.probe = as5114_hwmon_probe,
	.driver = {
		.name = "edgecore-as5114-hwmon",
		.of_match_table = as5114_hwmon_of_match,
	},
};
module_platform_driver(as5114_hwmon_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Edgecore AS5114-48X CPLD hwmon driver");
