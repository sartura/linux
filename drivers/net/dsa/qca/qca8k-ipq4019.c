// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2011-2012, 2020-2021 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (c) 2015, 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016 John Crispin <john@phrozen.org>
 * Copyright (c) 2021 Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/mdio.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/reset.h>
#include <net/dsa.h>

#include "qca8k.h"

int
qca8k_read(struct qca8k_priv *priv, u32 reg, u32 *val)
{
	return regmap_read(priv->regmap, reg, val);
}

int
qca8k_write(struct qca8k_priv *priv, u32 reg, u32 val)
{
	return regmap_write(priv->regmap, reg, val);
}

int
qca8k_rmw(struct qca8k_priv *priv, u32 reg, u32 mask, u32 write_val)
{
	return regmap_update_bits(priv->regmap, reg, mask, write_val);
}

int
qca8k_reg_set(struct qca8k_priv *priv, u32 reg, u32 val)
{
       return regmap_set_bits(priv->regmap, reg, val);
}

int
qca8k_reg_clear(struct qca8k_priv *priv, u32 reg, u32 val)
{
	return regmap_clear_bits(priv->regmap, reg, val);
}

static const struct regmap_range qca8k_readable_ranges[] = {
	regmap_reg_range(0x0000, 0x00e4), /* Global control */
	regmap_reg_range(0x0100, 0x0168), /* EEE control */
	regmap_reg_range(0x0200, 0x0270), /* Parser control */
	regmap_reg_range(0x0400, 0x0454), /* ACL */
	regmap_reg_range(0x0600, 0x0718), /* Lookup */
	regmap_reg_range(0x0800, 0x0b70), /* QM */
	regmap_reg_range(0x0c00, 0x0c80), /* PKT */
	regmap_reg_range(0x0e00, 0x0e98), /* L3 */
	regmap_reg_range(0x1000, 0x10ac), /* MIB - Port0 */
	regmap_reg_range(0x1100, 0x11ac), /* MIB - Port1 */
	regmap_reg_range(0x1200, 0x12ac), /* MIB - Port2 */
	regmap_reg_range(0x1300, 0x13ac), /* MIB - Port3 */
	regmap_reg_range(0x1400, 0x14ac), /* MIB - Port4 */
	regmap_reg_range(0x1500, 0x15ac), /* MIB - Port5 */
	regmap_reg_range(0x1600, 0x16ac), /* MIB - Port6 */

};

static const struct regmap_access_table qca8k_readable_table = {
	.yes_ranges = qca8k_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(qca8k_readable_ranges),
};

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

int
qca8k_busy_wait(struct qca8k_priv *priv, u32 reg, u32 mask)
{
	u32 val;

	return regmap_read_poll_timeout(priv->regmap, reg, val,
					!(val & mask),
					0,
					QCA8K_BUSY_WAIT_TIMEOUT);
}

void
qca8k_port_set_status(struct qca8k_priv *priv, int port, int enable)
{
	u32 mask = QCA8K_PORT_STATUS_TXMAC | QCA8K_PORT_STATUS_RXMAC;

	/* Port 0 is internally connected to the CPU
	 * TODO: Probably check for RGMII as well if it doesnt work
	 * in RGMII mode.
	 */
	if (port > QCA8K_CPU_PORT)
		mask |= QCA8K_PORT_STATUS_LINK_AUTO;

	if (enable)
		qca8k_reg_set(priv, QCA8K_REG_PORT_STATUS(port), mask);
	else
		qca8k_reg_clear(priv, QCA8K_REG_PORT_STATUS(port), mask);
}

static int
qca8k_setup_port(struct dsa_switch *ds, int port)
{
	struct qca8k_priv *priv = (struct qca8k_priv *)ds->priv;
	int ret;

	/* CPU port gets connected to all user ports of the switch */
	if (dsa_is_cpu_port(ds, port)) {
		ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(QCA8K_CPU_PORT),
				QCA8K_PORT_LOOKUP_MEMBER, dsa_user_ports(ds));
		if (ret)
			return ret;

		/* Disable CPU ARP Auto-learning by default */
		ret = qca8k_reg_clear(priv, QCA8K_PORT_LOOKUP_CTRL(QCA8K_CPU_PORT),
				      QCA8K_PORT_LOOKUP_LEARN);
		if (ret)
			return ret;
	}

	/* Individual user ports get connected to CPU port only */
	if (dsa_is_user_port(ds, port)) {
		int shift = 16 * (port % 2);

		ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port),
				QCA8K_PORT_LOOKUP_MEMBER,
				BIT(QCA8K_CPU_PORT));
		if (ret)
			return ret;

		/* Enable ARP Auto-learning by default */
		ret = qca8k_reg_set(priv, QCA8K_PORT_LOOKUP_CTRL(port),
				    QCA8K_PORT_LOOKUP_LEARN);
		if (ret)
			return ret;

		/* For port based vlans to work we need to set the
		 * default egress vid
		 */
		ret = qca8k_rmw(priv, QCA8K_EGRESS_VLAN(port),
				0xfff << shift,
				QCA8K_PORT_VID_DEF << shift);
		if (ret)
			return ret;

		ret = qca8k_write(priv, QCA8K_REG_PORT_VLAN_CTRL0(port),
				  QCA8K_PORT_VLAN_CVID(QCA8K_PORT_VID_DEF) |
				  QCA8K_PORT_VLAN_SVID(QCA8K_PORT_VID_DEF));
		if (ret)
			return ret;
	}

	return 0;
}

static int
qca8k_ipq4019_setup(struct dsa_switch *ds)
{
	struct qca8k_priv *priv = (struct qca8k_priv *)ds->priv;
	int ret, i;

	/* Make sure that port 0 is the cpu port */
	if (!dsa_is_cpu_port(ds, 0)) {
		dev_err(priv->dev, "port 0 is not the CPU port");
		return -EINVAL;
	}

	/* Enable CPU Port */
	ret = qca8k_reg_set(priv, QCA8K_REG_GLOBAL_FW_CTRL0,
			    QCA8K_GLOBAL_FW_CTRL0_CPU_PORT_EN);
	if (ret) {
		dev_err(priv->dev, "failed enabling CPU port");
		return ret;
	}

	/* Enable MIB counters */
	ret = qca8k_mib_init(priv);
	if (ret)
		dev_warn(priv->dev, "MIB init failed");

	/* Enable QCA header mode on the cpu port */
	ret = qca8k_write(priv, QCA8K_REG_PORT_HDR_CTRL(QCA8K_CPU_PORT),
			  QCA8K_PORT_HDR_CTRL_ALL << QCA8K_PORT_HDR_CTRL_TX_S |
			  QCA8K_PORT_HDR_CTRL_ALL << QCA8K_PORT_HDR_CTRL_RX_S);
	if (ret) {
		dev_err(priv->dev, "failed enabling QCA header mode");
		return ret;
	}

	/* Disable forwarding by default on all ports */
	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS; i++) {
		ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(i),
				QCA8K_PORT_LOOKUP_MEMBER, 0);
		if (ret)
			return ret;
	}

	/* Disable MAC by default on all ports */
	for (i = 1; i < QCA8K_IPQ4019_NUM_PORTS; i++)
		qca8k_port_set_status(priv, i, 0);

	/* Forward all unknown frames to CPU port for Linux processing */
	ret = qca8k_write(priv, QCA8K_REG_GLOBAL_FW_CTRL1,
			  BIT(QCA8K_CPU_PORT) << QCA8K_GLOBAL_FW_CTRL1_IGMP_DP_S |
			  BIT(QCA8K_CPU_PORT) << QCA8K_GLOBAL_FW_CTRL1_BC_DP_S |
			  BIT(QCA8K_CPU_PORT) << QCA8K_GLOBAL_FW_CTRL1_MC_DP_S |
			  BIT(QCA8K_CPU_PORT) << QCA8K_GLOBAL_FW_CTRL1_UC_DP_S);
	if (ret)
		return ret;

	/* Setup connection between CPU port & user ports */
	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS; i++) {
		ret = qca8k_setup_port(ds, i);
		if (ret)
			return ret;
	}

	/* Setup our port MTUs to match power on defaults */
	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS; i++)
		/* Set per port MTU to 1500 as the MTU change function
		 * will add the overhead and if its set to 1518 then it
		 * will apply the overhead again and we will end up with
		 * MTU of 1536 instead of 1518
		 */
		priv->port_mtu[i] = ETH_DATA_LEN;
	ret = qca8k_write(priv, QCA8K_MAX_FRAME_SIZE, ETH_FRAME_LEN + ETH_FCS_LEN);
	if (ret)
		dev_warn(priv->dev, "failed setting MTU settings");

	/* Flush the FDB table */
	qca8k_fdb_flush(priv);

	/* We don't have interrupts for link changes, so we need to poll */
	ds->pcs_poll = true;

	/* CPU port HW learning doesnt work correctly, so let DSA handle it */
	ds->assisted_learning_on_cpu_port = true;

	return 0;
}

static int psgmii_vco_calibrate(struct dsa_switch *ds)
{
	struct qca8k_priv *priv = ds->priv;
	int val, ret;

	/* Fix PSGMII RX 20bit */
	ret = phy_write(priv->psgmii_ethphy, MII_BMCR, 0x5b);
	/* Reset PSGMII PHY */
	ret = phy_write(priv->psgmii_ethphy, MII_BMCR, 0x1b);
	/* Release reset */
	ret = phy_write(priv->psgmii_ethphy, MII_BMCR, 0x5b);

	/* Poll for VCO PLL calibration finish */
	ret = phy_read_mmd_poll_timeout(priv->psgmii_ethphy,
					MDIO_MMD_PMAPMD,
					0x28, val,
					(val & BIT(0)),
					10000, 1000000,
					false);
	if (ret) {
		dev_err(ds->dev, "QCA807x PSGMII VCO calibration PLL not ready\n");
		return ret;
	}

	/* Freeze PSGMII RX CDR */
	ret = phy_write(priv->psgmii_ethphy, MII_RESV2, 0x2230);

	/* Start PSGMIIPHY VCO PLL calibration */
	ret = regmap_set_bits(priv->psgmii,
			PSGMIIPHY_VCO_CALIBRATION_CONTROL_REGISTER_1,
			PSGMIIPHY_REG_PLL_VCO_CALIB_RESTART);

	/* Poll for PSGMIIPHY PLL calibration finish */
	ret = regmap_read_poll_timeout(priv->psgmii,
				       PSGMIIPHY_VCO_CALIBRATION_CONTROL_REGISTER_2,
				       val, val & PSGMIIPHY_REG_PLL_VCO_CALIB_READY,
				       10000, 1000000);
	if (ret) {
		dev_err(ds->dev, "PSGMIIPHY VCO calibration PLL not ready\n");
		return ret;
	}

	/* Release PSGMII RX CDR */
	ret = phy_write(priv->psgmii_ethphy, MII_RESV2, 0x3230);

	/* Release PSGMII RX 20bit */
	ret = phy_write(priv->psgmii_ethphy, MII_BMCR, 0x5f);

	return ret;
}

static int ipq4019_psgmii_configure(struct dsa_switch *ds)
{
	struct qca8k_priv *priv = ds->priv;
	int ret;

	if (!priv->psgmii_calibrated) {
		ret = psgmii_vco_calibrate(ds);

		ret = regmap_clear_bits(priv->psgmii, PSGMIIPHY_MODE_CONTROL,
					PSGMIIPHY_MODE_ATHR_CSCO_MODE_25M);
		ret = regmap_write(priv->psgmii, PSGMIIPHY_TX_CONTROL,
				   PSGMIIPHY_TX_CONTROL_MAGIC_VALUE);

		priv->psgmii_calibrated = true;

		return ret;
	}

	return 0;
}

static void
qca8k_ipq4019_phylink_mac_config(struct dsa_switch *ds, int port, unsigned int mode,
				 const struct phylink_link_state *state)
{
	struct qca8k_priv *priv = ds->priv;

	switch (port) {
	case 0:
		/* CPU port, no configuration needed */
		return;
	case 1:
	case 2:
	case 3:
		if (state->interface == PHY_INTERFACE_MODE_PSGMII)
			if (ipq4019_psgmii_configure(ds))
				dev_err(ds->dev, "PSGMII configuration failed!\n");
		return;
	case 4:
	case 5:
		if (state->interface == PHY_INTERFACE_MODE_RGMII ||
		    state->interface == PHY_INTERFACE_MODE_RGMII_ID ||
		    state->interface == PHY_INTERFACE_MODE_RGMII_RXID ||
		    state->interface == PHY_INTERFACE_MODE_RGMII_TXID) {
			qca8k_reg_set(priv, QCA8K_IPQ4019_REG_RGMII_CTRL,
				      QCA8K_IPQ4019_RGMII_CTRL_CLK);
		}

		if (state->interface == PHY_INTERFACE_MODE_PSGMII)
			if (ipq4019_psgmii_configure(ds))
				dev_err(ds->dev, "PSGMII configuration failed!\n");
		return;
	default:
		dev_err(ds->dev, "%s: unsupported port: %i\n", __func__, port);
		return;
	}
}

static void
qca8k_ipq4019_phylink_validate(struct dsa_switch *ds, int port,
			       unsigned long *supported,
			       struct phylink_link_state *state)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	switch (port) {
	case 0: /* CPU port */
		if (state->interface != PHY_INTERFACE_MODE_INTERNAL)
			goto unsupported;
		break;
	case 1:
	case 2:
	case 3:
		/* Only PSGMII mode is supported */
		if (state->interface != PHY_INTERFACE_MODE_PSGMII)
			goto unsupported;
		break;
	case 4:
	case 5:
		/* PSGMII and RGMII modes are supported */
		if (state->interface != PHY_INTERFACE_MODE_PSGMII &&
		    state->interface != PHY_INTERFACE_MODE_RGMII &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_ID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_RXID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_TXID)
			goto unsupported;
		break;
	default:
unsupported:
		dev_warn(ds->dev, "interface '%s' (%d) on port %d is not supported\n",
			 phy_modes(state->interface), state->interface, port);
		linkmode_zero(supported);
		return;
	}

	if (port == 0) {
		phylink_set_port_modes(mask);

		phylink_set(mask, 1000baseT_Full);

		phylink_set(mask, Pause);
		phylink_set(mask, Asym_Pause);

		linkmode_and(supported, supported, mask);
		linkmode_and(state->advertising, state->advertising, mask);
	} else {
		/* Simply copy what PHYs tell us */
		linkmode_copy(state->advertising, supported);
	}
}

static enum dsa_tag_protocol
qca8k_get_tag_protocol(struct dsa_switch *ds, int port,
		       enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_IPQ4019;
}

static const struct dsa_switch_ops qca8k_ipq4019_switch_ops = {
	.get_tag_protocol	= qca8k_get_tag_protocol,
	.setup			= qca8k_ipq4019_setup,
	.get_strings		= qca8k_get_strings,
	.get_ethtool_stats	= qca8k_get_ethtool_stats,
	.get_sset_count		= qca8k_get_sset_count,
	.get_mac_eee		= qca8k_get_mac_eee,
	.set_mac_eee		= qca8k_set_mac_eee,
	.port_enable		= qca8k_port_enable,
	.port_disable		= qca8k_port_disable,
	.port_change_mtu	= qca8k_port_change_mtu,
	.port_max_mtu		= qca8k_port_max_mtu,
	.port_stp_state_set	= qca8k_port_stp_state_set,
	.port_bridge_join	= qca8k_port_bridge_join,
	.port_bridge_leave	= qca8k_port_bridge_leave,
	.port_fdb_add		= qca8k_port_fdb_add,
	.port_fdb_del		= qca8k_port_fdb_del,
	.port_fdb_dump		= qca8k_port_fdb_dump,
	.port_vlan_filtering	= qca8k_port_vlan_filtering,
	.port_vlan_add		= qca8k_port_vlan_add,
	.port_vlan_del		= qca8k_port_vlan_del,
	.phylink_validate	= qca8k_ipq4019_phylink_validate,
	.phylink_mac_link_state	= qca8k_phylink_mac_link_state,
	.phylink_mac_config	= qca8k_ipq4019_phylink_mac_config,
	.phylink_mac_link_down	= qca8k_phylink_mac_link_down,
	.phylink_mac_link_up	= qca8k_phylink_mac_link_up,
};

static int
qca8k_ipq4019_probe(struct platform_device *pdev)
{
	struct qca8k_priv *priv;
	void __iomem *base, *psgmii;
	struct device_node *np = pdev->dev.of_node, *mdio_np, *psgmii_ethphy_np;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;

	base = devm_platform_ioremap_resource_byname(pdev, "base");
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(priv->dev, base,
					     &qca8k_ipq4019_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(priv->dev, "base regmap initialization failed, %d\n", ret);
		return ret;
	}

	psgmii = devm_platform_ioremap_resource_byname(pdev, "psgmii_phy");
	if (IS_ERR(psgmii))
		return PTR_ERR(psgmii);

	priv->psgmii = devm_regmap_init_mmio(priv->dev, psgmii,
					     &qca8k_ipq4019_psgmii_phy_regmap_config);
	if (IS_ERR(priv->psgmii)) {
		ret = PTR_ERR(priv->psgmii);
		dev_err(priv->dev, "PSGMII regmap initialization failed, %d\n", ret);
		return ret;
	}

	mdio_np = of_parse_phandle(np, "mdio", 0);
	if (!mdio_np) {
		dev_err(&pdev->dev, "unable to get MDIO bus phandle\n");
		of_node_put(mdio_np);
		return -EINVAL;
	}

	priv->bus = of_mdio_find_bus(mdio_np);
	of_node_put(mdio_np);
	if (!priv->bus) {
		dev_err(&pdev->dev, "unable to find MDIO bus\n");
		return -EPROBE_DEFER;
	}

	psgmii_ethphy_np = of_parse_phandle(np, "psgmii-ethphy", 0);
	if (!psgmii_ethphy_np) {
		dev_err(&pdev->dev, "unable to get PSGMII eth PHY phandle\n");
		of_node_put(psgmii_ethphy_np);
		return -ENODEV;
	}

	priv->psgmii_ethphy = of_phy_find_device(psgmii_ethphy_np);
	of_node_put(psgmii_ethphy_np);
	if (!priv->psgmii_ethphy) {
		return -ENODEV;
	}

	priv->ds = devm_kzalloc(priv->dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = priv->dev;
	priv->ds->num_ports = QCA8K_IPQ4019_NUM_PORTS;
	priv->ds->priv = priv;
	priv->ops = qca8k_ipq4019_switch_ops;
	priv->ds->ops = &priv->ops;

	mutex_init(&priv->reg_mutex);
	platform_set_drvdata(pdev, priv);

	return dsa_register_switch(priv->ds);
}

static int
qca8k_ipq4019_remove(struct platform_device *pdev)
{
	struct qca8k_priv *priv = dev_get_drvdata(&pdev->dev);
	int i;

	if (!priv)
		return 0;

	for (i = 0; i < QCA8K_NUM_PORTS; i++)
		qca8k_port_set_status(priv, i, 0);

	dsa_unregister_switch(priv->ds);

	dev_set_drvdata(&pdev->dev, NULL);

	return 0;
}

static const struct of_device_id qca8k_ipq4019_of_match[] = {
	{ .compatible = "qca,ipq4019-qca8337n" },
	{ /* sentinel */ },
};

static struct platform_driver qca8k_ipq4019_driver = {
	.probe = qca8k_ipq4019_probe,
	.remove = qca8k_ipq4019_remove,
	.driver = {
		.name = "qca8k-ipq4019",
		.of_match_table = qca8k_ipq4019_of_match,
	},
};

module_platform_driver(qca8k_ipq4019_driver);

MODULE_AUTHOR("Mathieu Olivari, John Crispin <john@phrozen.org>");
MODULE_AUTHOR("Gabor Juhos <j4g8y7@gmail.com>, Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Qualcomm IPQ4019 built-in switch driver");
MODULE_LICENSE("GPL v2");
