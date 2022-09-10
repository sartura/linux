// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2011-2012, 2020-2021 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (c) 2015, 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016 John Crispin <john@phrozen.org>
 * Copyright (c) 2022 Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/module.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <net/dsa.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/mdio.h>
#include <linux/phylink.h>

#include "qca8k.h"

static struct regmap_config qca8k_ipq4019_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x16ac, /* end MIB - Port6 range */
	.rd_table = &qca8k_readable_table,
};

static struct regmap_config qca8k_ipq4019_psgmii_phy_regmap_config = {
	.name = "psgmii-phy",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x7fc,
};

static const struct qca8k_match_data ipq4019 = {
	.id = QCA8K_ID_IPQ4019,
	.mib_count = QCA8K_QCA833X_MIB_COUNT,
	//.ops = &qca8xxx_ops,
};

static int
qca8k_ipq4019_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qca8k_priv *priv;
	void __iomem *base, *psgmii;
	struct device_node *np = dev->of_node, *mdio_np, *psgmii_ethphy_np;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->info = &ipq4019;

	/* Start by setting up the register mapping */
	base = devm_platform_ioremap_resource_byname(pdev, "base");
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(dev, base,
					     &qca8k_ipq4019_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(dev, "base regmap initialization failed, %d\n", ret);
		return ret;
	}

	psgmii = devm_platform_ioremap_resource_byname(pdev, "psgmii_phy");
	if (IS_ERR(psgmii))
		return PTR_ERR(psgmii);

	priv->psgmii = devm_regmap_init_mmio(dev, psgmii,
					     &qca8k_ipq4019_psgmii_phy_regmap_config);
	if (IS_ERR(priv->psgmii)) {
		ret = PTR_ERR(priv->psgmii);
		dev_err(dev, "PSGMII regmap initialization failed, %d\n", ret);
		return ret;
	}

	/* Check the detected switch id */
	ret = qca8k_read_switch_id(priv);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id qca8k_ipq4019_of_match[] = {
	{ .compatible = "qca,ipq4019-qca8337n", },
	{ /* sentinel */ },
};

static struct platform_driver qca8k_ipq4019_driver = {
	.probe = qca8k_ipq4019_probe,
	//.remove = qca8k_ipq4019_remove,
	.driver = {
		.name = "qca8k-ipq4019",
		.of_match_table = qca8k_ipq4019_of_match,
	},
};

module_platform_driver(qca8k_ipq4019_driver);

MODULE_AUTHOR("Mathieu Olivari, John Crispin <john@phrozen.org>");
MODULE_AUTHOR("Gabor Juhos <j4g8y7@gmail.com>, Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Qualcomm IPQ4019 built-in switch driver");
MODULE_LICENSE("GPL");
