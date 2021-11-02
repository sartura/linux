// SPDX-License-Identifier: GPL-2.0-only
/*
 * Delta TN48M CPLD GPIO driver
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

struct tn48m_gpio_config {
	int ngpio;
	int ngpio_per_reg;
	unsigned int reg_dat_base;
	unsigned int reg_set_base;
};

static const struct tn48m_gpio_config tn48m_gpo_config = {
	.ngpio = 4,
	.ngpio_per_reg = 4,
	.reg_set_base = 0x31,
};

static const struct tn48m_gpio_config tn48m_gpi_config = {
	.ngpio = 8,
	.ngpio_per_reg = 4,
	.reg_dat_base = 0x3a,
};

static int tn48m_gpio_probe(struct platform_device *pdev)
{
	const struct tn48m_gpio_config *gpio_config = NULL;
	struct gpio_regmap_config config = {0};
	struct regmap *regmap;

	if (!pdev->dev.parent)
		return -ENODEV;

	gpio_config = device_get_match_data(&pdev->dev);
	if (!gpio_config)
		return -ENODEV;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap)
		return -ENODEV;

	config.regmap = regmap;
	config.parent = &pdev->dev;
	config.ngpio = gpio_config->ngpio;
	config.ngpio_per_reg = gpio_config->ngpio_per_reg;
	if (gpio_config->reg_dat_base)
		config.reg_dat_base = gpio_config->reg_dat_base;
	if (gpio_config->reg_set_base)
		config.reg_set_base = gpio_config->reg_set_base;

	return PTR_ERR_OR_ZERO(devm_gpio_regmap_register(&pdev->dev, &config));
}

static const struct of_device_id tn48m_gpio_of_match[] = {
	{ .compatible = "delta,tn48m-gpo", .data = &tn48m_gpo_config },
	{ .compatible = "delta,tn48m-gpi", .data = &tn48m_gpi_config },
	{ }
};
MODULE_DEVICE_TABLE(of, tn48m_gpio_of_match);

static struct platform_driver tn48m_gpio_driver = {
	.driver = {
		.name = "delta-tn48m-gpio",
		.of_match_table = tn48m_gpio_of_match,
	},
	.probe = tn48m_gpio_probe,
};
module_platform_driver(tn48m_gpio_driver);

MODULE_AUTHOR("Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Delta TN48M CPLD GPIO driver");
MODULE_LICENSE("GPL");
