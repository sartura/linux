// SPDX-License-Identifier: GPL-2.0-only
/*
 * Edgecore AS5114-48X CPLD GPIO driver
 *
 * Copyright (C) 2021 Sartura Ltd.
 *
 * Author: Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define AS5114_SFP_TX_FAULT_1_MASK_REG	0xB0
#define AS5114_SFP_LOS_1_MASK_REG	0xB6
#define AS5114_SFP_MASK_REG_NUM		6

enum as5114_gpio_type {
	AS5114_SFP_TX_DISABLE = 1,
	AS5114_SFP_TX_FAULT,
	AS5114_SFP_PRESENT,
	AS5114_SFP_LOS,
};

static int as5114_gpio_enable(struct regmap *regmap, u8 reg)
{
	u8 buf[AS5114_SFP_MASK_REG_NUM] = {0};
	int ret;

	/*
	 * By default SFP LOS and TX fault pins are disabled.
	 * So, enable both of them by setting their respective
	 * mask registers to 0.
	 * There are 6 registers for LOS and 6 for TX fault.
	 * Each bit inside of them corresponds to a certain pin.
	 */
	ret = regmap_bulk_write(regmap,
				reg,
				buf,
				AS5114_SFP_MASK_REG_NUM);

	return ret;
}

static int as5114_gpio_probe(struct platform_device *pdev)
{
	struct gpio_regmap_config config = {0};
	enum as5114_gpio_type type;
	struct regmap *regmap;
	u32 base;
	int ret;

	if (!pdev->dev.parent)
		return -ENODEV;

	type = (uintptr_t)device_get_match_data(&pdev->dev);
	if (!type)
		return -ENODEV;

	ret = device_property_read_u32(&pdev->dev, "reg", &base);
	if (ret)
		return ret;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap)
		return -ENODEV;

	config.regmap = regmap;
	config.parent = &pdev->dev;
	config.ngpio = 48;
	config.ngpio_per_reg = 8;

	switch (type) {
	case AS5114_SFP_TX_DISABLE:
		config.reg_set_base = base;
		break;
	case AS5114_SFP_TX_FAULT:
		config.reg_dat_base = base;

		ret = as5114_gpio_enable(config.regmap,
					 AS5114_SFP_TX_FAULT_1_MASK_REG);
		if (ret)
			return ret;
		break;
	case AS5114_SFP_PRESENT:
		config.reg_dat_base = base;
		break;
	case AS5114_SFP_LOS:
		config.reg_dat_base = base;

		ret = as5114_gpio_enable(config.regmap,
					 AS5114_SFP_LOS_1_MASK_REG);
		if (ret)
			return ret;
		break;
	default:
		dev_err(&pdev->dev, "unknown type %d\n", type);
		return -ENODEV;
	}

	return PTR_ERR_OR_ZERO(devm_gpio_regmap_register(&pdev->dev, &config));
}

static const struct of_device_id as5114_gpio_of_match[] = {
	{ .compatible = "edgecore,as5114-gpio-sfp-tx-disable", .data = (void *)AS5114_SFP_TX_DISABLE },
	{ .compatible = "edgecore,as5114-gpio-sfp-tx-fault", .data = (void *)AS5114_SFP_TX_FAULT },
	{ .compatible = "edgecore,as5114-gpio-sfp-present", .data = (void *)AS5114_SFP_PRESENT },
	{ .compatible = "edgecore,as5114-gpio-sfp-los", .data = (void *)AS5114_SFP_LOS },
	{ }
};
MODULE_DEVICE_TABLE(of, as5114_gpio_of_match);

static struct platform_driver as5114_gpio_driver = {
	.driver = {
		.name = "edgecore-as5114-gpio",
		.of_match_table = as5114_gpio_of_match,
	},
	.probe = as5114_gpio_probe,
};
module_platform_driver(as5114_gpio_driver);

MODULE_AUTHOR("Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Edgecore AS5114-48X CPLD GPIO driver");
MODULE_LICENSE("GPL");
