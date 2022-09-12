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

static enum dsa_tag_protocol
qca8k_ipq4019_get_tag_protocol(struct dsa_switch *ds, int port,
			       enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_OOB;
}

static struct phylink_pcs *
qca8k_ipq4019_phylink_mac_select_pcs(struct dsa_switch *ds, int port,
				     phy_interface_t interface)
{
	struct qca8k_priv *priv = ds->priv;
	struct phylink_pcs *pcs = NULL;

	switch (interface) {
	case PHY_INTERFACE_MODE_PSGMII:
		switch (port) {
		case 0:
			pcs = &priv->pcs_port_0.pcs;
			break;
		}
		break;
	default:
		break;
	}

	return pcs;
}

static int qca8k_ipq4019_pcs_config(struct phylink_pcs *pcs, unsigned int mode,
				    phy_interface_t interface,
				    const unsigned long *advertising,
				    bool permit_pause_to_mac)
{
	return 0;
}

static void qca8k_ipq4019_pcs_an_restart(struct phylink_pcs *pcs)
{
}

static struct qca8k_pcs *pcs_to_qca8k_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct qca8k_pcs, pcs);
}

static void qca8k_ipq4019_pcs_get_state(struct phylink_pcs *pcs,
					struct phylink_link_state *state)
{
	struct qca8k_priv *priv = pcs_to_qca8k_pcs(pcs)->priv;
	int port = pcs_to_qca8k_pcs(pcs)->port;
	u32 reg;
	int ret;

	ret = qca8k_read(priv, QCA8K_REG_PORT_STATUS(port), &reg);
	if (ret < 0) {
		state->link = false;
		return;
	}

	state->link = !!(reg & QCA8K_PORT_STATUS_LINK_UP);
	state->an_complete = state->link;
	state->an_enabled = !!(reg & QCA8K_PORT_STATUS_LINK_AUTO);
	state->duplex = (reg & QCA8K_PORT_STATUS_DUPLEX) ? DUPLEX_FULL :
							   DUPLEX_HALF;

	switch (reg & QCA8K_PORT_STATUS_SPEED) {
	case QCA8K_PORT_STATUS_SPEED_10:
		state->speed = SPEED_10;
		break;
	case QCA8K_PORT_STATUS_SPEED_100:
		state->speed = SPEED_100;
		break;
	case QCA8K_PORT_STATUS_SPEED_1000:
		state->speed = SPEED_1000;
		break;
	default:
		state->speed = SPEED_UNKNOWN;
		break;
	}

	if (reg & QCA8K_PORT_STATUS_RXFLOW)
		state->pause |= MLO_PAUSE_RX;
	if (reg & QCA8K_PORT_STATUS_TXFLOW)
		state->pause |= MLO_PAUSE_TX;
}

static const struct phylink_pcs_ops qca8k_pcs_ops = {
	.pcs_get_state = qca8k_ipq4019_pcs_get_state,
	.pcs_config = qca8k_ipq4019_pcs_config,
	.pcs_an_restart = qca8k_ipq4019_pcs_an_restart,
};

static void qca8k_ipq4019_setup_pcs(struct qca8k_priv *priv,
				    struct qca8k_pcs *qpcs,
				    int port)
{
	qpcs->pcs.ops = &qca8k_pcs_ops;

	/* We don't have interrupts for link changes, so we need to poll */
	qpcs->pcs.poll = true;
	qpcs->priv = priv;
	qpcs->port = port;
}

static void qca8k_ipq4019_phylink_get_caps(struct dsa_switch *ds, int port,
					   struct phylink_config *config)
{
	switch (port) {
	case 0: /* CPU port */
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		break;

	case 1:
	case 2:
	case 3:
		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  config->supported_interfaces);
		break;
	case 4:
	case 5:
		phy_interface_set_rgmii(config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  config->supported_interfaces);
		break;
	}

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
		MAC_10 | MAC_100 | MAC_1000FD;

	config->legacy_pre_march2020 = false;
}

static int
qca8k_ipq4019_setup_port(struct dsa_switch *ds, int port)
{
	struct qca8k_priv *priv = (struct qca8k_priv *)ds->priv;
	int ret;

	/* CPU port gets connected to all user ports of the switch */
	if (dsa_is_cpu_port(ds, port)) {
		ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port),
				QCA8K_PORT_LOOKUP_MEMBER, dsa_user_ports(ds));
		if (ret)
			return ret;

		/* Disable CPU ARP Auto-learning by default */
		ret = regmap_clear_bits(priv->regmap,
					QCA8K_PORT_LOOKUP_CTRL(port),
					QCA8K_PORT_LOOKUP_LEARN);
		if (ret)
			return ret;
	}

	/* Individual user ports get connected to CPU port only */
	if (dsa_is_user_port(ds, port)) {
		ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port),
				QCA8K_PORT_LOOKUP_MEMBER,
				BIT(QCA8K_IPQ4019_CPU_PORT));
		if (ret)
			return ret;

		/* Enable ARP Auto-learning by default */
		ret = regmap_set_bits(priv->regmap, QCA8K_PORT_LOOKUP_CTRL(port),
				      QCA8K_PORT_LOOKUP_LEARN);
		if (ret)
			return ret;

		/* For port based vlans to work we need to set the
		 * default egress vid
		 */
		ret = qca8k_rmw(priv, QCA8K_EGRESS_VLAN(port),
				QCA8K_EGREES_VLAN_PORT_MASK(port),
				QCA8K_EGREES_VLAN_PORT(port, QCA8K_PORT_VID_DEF));
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
	if (!dsa_is_cpu_port(ds, QCA8K_IPQ4019_CPU_PORT)) {
		dev_err(priv->dev, "port %d is not the CPU port",
			QCA8K_IPQ4019_CPU_PORT);
		return -EINVAL;
	}

	qca8k_ipq4019_setup_pcs(priv, &priv->pcs_port_0, 0);

	/* Enable CPU Port */
	ret = regmap_set_bits(priv->regmap, QCA8K_REG_GLOBAL_FW_CTRL0,
			      QCA8K_GLOBAL_FW_CTRL0_CPU_PORT_EN);
	if (ret) {
		dev_err(priv->dev, "failed enabling CPU port");
		return ret;
	}

	/* Enable MIB counters */
	ret = qca8k_mib_init(priv);
	if (ret)
		dev_warn(priv->dev, "MIB init failed");

	/* Disable forwarding by default on all ports */
	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS; i++) {
		ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(i),
				QCA8K_PORT_LOOKUP_MEMBER, 0);
		if (ret)
			return ret;
	}

	/* Enable QCA header mode on the CPU port */
	ret = qca8k_write(priv, QCA8K_REG_PORT_HDR_CTRL(QCA8K_IPQ4019_CPU_PORT),
			  FIELD_PREP(QCA8K_PORT_HDR_CTRL_TX_MASK, QCA8K_PORT_HDR_CTRL_ALL) |
			  FIELD_PREP(QCA8K_PORT_HDR_CTRL_RX_MASK, QCA8K_PORT_HDR_CTRL_ALL));
	if (ret) {
		dev_err(priv->dev, "failed enabling QCA header mode");
		return ret;
	}

	/* Disable MAC by default on all ports */
	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS; i++) {
		if (dsa_is_user_port(ds, i))
			qca8k_port_set_status(priv, i, 0);
	}

	/* Forward all unknown frames to CPU port for Linux processing */
	ret = qca8k_write(priv, QCA8K_REG_GLOBAL_FW_CTRL1,
			  FIELD_PREP(QCA8K_GLOBAL_FW_CTRL1_IGMP_DP_MASK, BIT(QCA8K_IPQ4019_CPU_PORT)) |
			  FIELD_PREP(QCA8K_GLOBAL_FW_CTRL1_BC_DP_MASK, BIT(QCA8K_IPQ4019_CPU_PORT)) |
			  FIELD_PREP(QCA8K_GLOBAL_FW_CTRL1_MC_DP_MASK, BIT(QCA8K_IPQ4019_CPU_PORT)) |
			  FIELD_PREP(QCA8K_GLOBAL_FW_CTRL1_UC_DP_MASK, BIT(QCA8K_IPQ4019_CPU_PORT)));
	if (ret)
		return ret;

	/* Setup connection between CPU port & user ports */
	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS; i++) {
		ret = qca8k_ipq4019_setup_port(ds, i);
		if (ret)
			return ret;
	}

	/* Setup our port MTUs to match power on defaults */
	ret = qca8k_write(priv, QCA8K_MAX_FRAME_SIZE, ETH_FRAME_LEN + ETH_FCS_LEN);
	if (ret)
		dev_warn(priv->dev, "failed setting MTU settings");

	/* Flush the FDB table */
	qca8k_fdb_flush(priv);

	/* Set min a max ageing value supported */
	ds->ageing_time_min = 7000;
	ds->ageing_time_max = 458745000;

	/* Set max number of LAGs supported */
	ds->num_lag_ids = QCA8K_NUM_LAGS;

	/* CPU port HW learning doesnt work correctly, so let DSA handle it */
	ds->assisted_learning_on_cpu_port = true;

	return 0;
}

static const struct dsa_switch_ops qca8k_ipq4019_switch_ops = {
	.get_tag_protocol	= qca8k_ipq4019_get_tag_protocol,
	.setup			= qca8k_ipq4019_setup,
	.get_strings		= qca8k_get_strings,
	.get_ethtool_stats	= qca8k_get_ethtool_stats,
	.get_sset_count		= qca8k_get_sset_count,
	.set_ageing_time	= qca8k_set_ageing_time,
	.get_mac_eee		= qca8k_get_mac_eee,
	.set_mac_eee		= qca8k_set_mac_eee,
	.port_enable		= qca8k_port_enable,
	.port_disable		= qca8k_port_disable,
	.port_change_mtu	= qca8k_port_change_mtu,
	.port_max_mtu		= qca8k_port_max_mtu,
	.port_stp_state_set	= qca8k_port_stp_state_set,
	.port_bridge_join	= qca8k_port_bridge_join,
	.port_bridge_leave	= qca8k_port_bridge_leave,
	.port_fast_age		= qca8k_port_fast_age,
	.port_fdb_add		= qca8k_port_fdb_add,
	.port_fdb_del		= qca8k_port_fdb_del,
	.port_fdb_dump		= qca8k_port_fdb_dump,
	.port_mdb_add		= qca8k_port_mdb_add,
	.port_mdb_del		= qca8k_port_mdb_del,
	.port_mirror_add	= qca8k_port_mirror_add,
	.port_mirror_del	= qca8k_port_mirror_del,
	.port_vlan_filtering	= qca8k_port_vlan_filtering,
	.port_vlan_add		= qca8k_port_vlan_add,
	.port_vlan_del		= qca8k_port_vlan_del,
	.phylink_mac_select_pcs	= qca8k_ipq4019_phylink_mac_select_pcs,
	.phylink_get_caps	= qca8k_ipq4019_phylink_get_caps,
	/*.phylink_mac_config	= qca8k_phylink_mac_config,
	.phylink_mac_link_down	= qca8k_phylink_mac_link_down,
	.phylink_mac_link_up	= qca8k_phylink_mac_link_up,*/
	.port_lag_join		= qca8k_port_lag_join,
	.port_lag_leave		= qca8k_port_lag_leave,
};

static const struct qca8k_match_data ipq4019 = {
	.id = QCA8K_ID_IPQ4019,
	.mib_count = QCA8K_QCA833X_MIB_COUNT,
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

	mdio_np = of_parse_phandle(np, "mdio", 0);
	if (!mdio_np) {
		dev_err(dev, "unable to get MDIO bus phandle\n");
		of_node_put(mdio_np);
		return -EINVAL;
	}

	priv->bus = of_mdio_find_bus(mdio_np);
	of_node_put(mdio_np);
	if (!priv->bus) {
		dev_err(dev, "unable to find MDIO bus\n");
		return -EPROBE_DEFER;
	}

	psgmii_ethphy_np = of_parse_phandle(np, "psgmii-ethphy", 0);
	if (!psgmii_ethphy_np) {
		dev_dbg(dev, "unable to get PSGMII eth PHY phandle\n");
		of_node_put(psgmii_ethphy_np);
	}

	if (psgmii_ethphy_np) {
		priv->psgmii_ethphy = of_phy_find_device(psgmii_ethphy_np);
		of_node_put(psgmii_ethphy_np);
		if (!priv->psgmii_ethphy) {
			dev_err(dev, "unable to get PSGMII eth PHY\n");
			return -ENODEV;
		}
	}

	/* Check the detected switch id */
	ret = qca8k_read_switch_id(priv);
	if (ret)
		return ret;

	priv->ds = devm_kzalloc(dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = dev;
	priv->ds->num_ports = QCA8K_IPQ4019_NUM_PORTS;
	priv->ds->priv = priv;
	priv->ds->ops = &qca8k_ipq4019_switch_ops;
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

	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS; i++)
		qca8k_port_set_status(priv, i, 0);

	dsa_unregister_switch(priv->ds);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id qca8k_ipq4019_of_match[] = {
	{ .compatible = "qca,ipq4019-qca8337n", },
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
MODULE_LICENSE("GPL");
