// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2011-2012, 2020-2021 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (c) 2015, 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016 John Crispin <john@phrozen.org>
 * Copyright (c) 2021 Robert Marko <robert.marko@sartura.hr>
 */

#include <linux/clk.h>
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
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x16ac, /* end MIB - Port6 range */
	.rd_table = &qca8k_readable_table,
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
			  BIT(0) << QCA8K_GLOBAL_FW_CTRL1_IGMP_DP_S |
			  GENMASK(5, 0) << QCA8K_GLOBAL_FW_CTRL1_BC_DP_S |
			  GENMASK(5, 0) << QCA8K_GLOBAL_FW_CTRL1_MC_DP_S |
			  GENMASK(5, 0) << QCA8K_GLOBAL_FW_CTRL1_UC_DP_S);
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
		priv->port_mtu[i] = ETH_FRAME_LEN + ETH_FCS_LEN;
	ret = qca8k_write(priv, QCA8K_MAX_FRAME_SIZE, ETH_FRAME_LEN + ETH_FCS_LEN);
	if (ret)
		dev_warn(priv->dev, "failed setting MTU settings");

	/* Flush the FDB table */
	qca8k_fdb_flush(priv);

	/* We don't have interrupts for link changes, so we need to poll */
	ds->pcs_poll = true;

	return 0;
}

static void
qca8k_ipq4019_phylink_mac_config(struct dsa_switch *ds, int port, unsigned int mode,
				 const struct phylink_link_state *state)
{
	/* Nothing to configure
	 * TODO: Look into moving PHY calibration here
	 */
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
	case 4:
	case 5:
		/* User ports, only PSGMII mode is supported for now */
		if (state->interface != PHY_INTERFACE_MODE_PSGMII)
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

enum ar40xx_port_wrapper_cfg {
	PORT_WRAPPER_PSGMII = 0,
	PORT_WRAPPER_RGMII = 3,
};

#define AR40XX_PSGMII_MODE_CONTROL			0x1b4
#define   AR40XX_PSGMII_ATHR_CSCO_MODE_25M		BIT(0)

#define AR40XX_PSGMIIPHY_TX_CONTROL			0x288

#define AR40XX_REG_RGMII_CTRL				0x0004
#define AR40XX_REG_PORT_LOOKUP(_i)			(0x660 + (_i) * 0xc)
#define   AR40XX_PORT_LOOKUP_LOOPBACK			BIT(21)

#define AR40XX_PHY_SPEC_STATUS				0x11
#define   AR40XX_PHY_SPEC_STATUS_LINK			BIT(10)
#define   AR40XX_PHY_SPEC_STATUS_DUPLEX			BIT(13)
#define   AR40XX_PHY_SPEC_STATUS_SPEED			GENMASK(16, 14)

#define AR40XX_PSGMII_ID				5
#define AR40XX_PSGMII_CALB_NUM				100
#define AR40XX_MALIBU_PSGMII_MODE_CTRL			0x6d
#define AR40XX_MALIBU_PHY_PSGMII_MODE_CTRL_ADJUST_VAL	0x220c
#define AR40XX_MALIBU_PHY_MMD7_DAC_CTRL			0x801a
#define AR40XX_MALIBU_DAC_CTRL_MASK			0x380
#define AR40XX_MALIBU_DAC_CTRL_VALUE			0x280
#define AR40XX_MALIBU_PHY_RLP_CTRL			0x805a
#define AR40XX_PSGMII_TX_DRIVER_1_CTRL			0xb
#define AR40XX_MALIBU_PHY_PSGMII_REDUCE_SERDES_TX_AMP	0x8a
#define AR40XX_MALIBU_PHY_LAST_ADDR			4

static u32
psgmii_read(struct qca8k_priv *priv, int reg)
{
	u32 val;

	regmap_read(priv->psgmii, reg, &val);
	return val;
}

static void
psgmii_write(struct qca8k_priv *priv, int reg, u32 val)
{
	regmap_write(priv->psgmii, reg, val);
}

static void
qca8k_phy_mmd_write(struct qca8k_priv *priv, u32 phy_id,
		     u16 mmd_num, u16 reg_id, u16 reg_val)
{
	struct mii_bus *bus = priv->bus;

	mutex_lock(&bus->mdio_lock);
	__mdiobus_write(bus, phy_id, MII_MMD_CTRL, mmd_num);
	__mdiobus_write(bus, phy_id, MII_MMD_DATA, reg_id);
	__mdiobus_write(bus, phy_id, MII_MMD_CTRL, MII_MMD_CTRL_NOINCR | mmd_num);
	__mdiobus_write(bus, phy_id, MII_MMD_DATA, reg_val);
	mutex_unlock(&bus->mdio_lock);
}

static u16
qca8k_phy_mmd_read(struct qca8k_priv *priv, u32 phy_id,
		    u16 mmd_num, u16 reg_id)
{
	struct mii_bus *bus = priv->bus;
	u16 value;

	mutex_lock(&bus->mdio_lock);
	__mdiobus_write(bus, phy_id, MII_MMD_CTRL, mmd_num);
	__mdiobus_write(bus, phy_id, MII_MMD_DATA, reg_id);
	__mdiobus_write(bus, phy_id, MII_MMD_CTRL, MII_MMD_CTRL_NOINCR | mmd_num);
	value = __mdiobus_read(bus, phy_id, MII_MMD_DATA);
	mutex_unlock(&bus->mdio_lock);

	return value;
}

static void
ess_reset(struct qca8k_priv *priv)
{
	reset_control_assert(priv->ess_rst);

	mdelay(10);

	reset_control_deassert(priv->ess_rst);

	/* Waiting for all inner tables to be flushed and reinitialized.
	 * This takes between 5 and 10ms.
	 */
	mdelay(10);
}

static void
ar40xx_malibu_psgmii_ess_reset(struct qca8k_priv *priv)
{
	struct mii_bus *bus = priv->bus;
	u32 n;

	/* Reset phy psgmii */
	/* fix phy psgmii RX 20bit */
	mdiobus_write(bus, AR40XX_PSGMII_ID, 0x0, 0x005b);
	/* reset phy psgmii */
	mdiobus_write(bus, AR40XX_PSGMII_ID, 0x0, 0x001b);
	/* release reset phy psgmii */
	mdiobus_write(bus, AR40XX_PSGMII_ID, 0x0, 0x005b);

	for (n = 0; n < AR40XX_PSGMII_CALB_NUM; n++) {
		u16 status;

		status = qca8k_phy_mmd_read(priv, AR40XX_PSGMII_ID,
					     MDIO_MMD_PMAPMD, 0x28);
		if (status & BIT(0))
			break;

		/* Polling interval to check PSGMII PLL in malibu is ready
		 * the worst time is 8.67ms
		 * for 25MHz reference clock
		 * [512+(128+2048)*49]*80ns+100us
		 */
		mdelay(2);
	}

	/* check malibu psgmii calibration done end... */

	/* freeze phy psgmii RX CDR */
	mdiobus_write(bus, AR40XX_PSGMII_ID, 0x1a, 0x2230);

	ess_reset(priv);

	/* wait for the psgmii calibration to complete */
	for (n = 0; n < AR40XX_PSGMII_CALB_NUM; n++) {
		u32 status;

		status = psgmii_read(priv, 0xa0);
		if (status & BIT(0))
			break;

		/* Polling interval to check PSGMII PLL in ESS is ready */
		mdelay(2);
	}

	/* release phy psgmii RX CDR */
	mdiobus_write(bus, AR40XX_PSGMII_ID, 0x1a, 0x3230);
	/* release phy psgmii RX 20bit */
	mdiobus_write(bus, AR40XX_PSGMII_ID, 0x0, 0x005f);
}

static void
ar40xx_phytest_run(struct qca8k_priv *priv, int phy)
{
	/* enable check */
	qca8k_phy_mmd_write(priv, phy, 7, 0x8029, 0x0000);
	qca8k_phy_mmd_write(priv, phy, 7, 0x8029, 0x0003);

	/* start traffic */
	qca8k_phy_mmd_write(priv, phy, 7, 0x8020, 0xa000);

	/* wait precisely for all traffic end
	 * 4096(pkt num) * 1524(size) * 8ns (125MHz) = 49.9ms
	 */
	mdelay(50);
}

static bool
ar40xx_phytest_check_counters(struct qca8k_priv *priv, int phy, u32 count)
{
	u32 tx_ok, tx_error;
	u32 rx_ok, rx_error;
	u32 tx_ok_high16;
	u32 rx_ok_high16;
	u32 tx_all_ok, rx_all_ok;

	/* read counters */
	tx_ok = qca8k_phy_mmd_read(priv, phy, 7, 0x802e);
	tx_ok_high16 = qca8k_phy_mmd_read(priv, phy, 7, 0x802d);
	tx_error = qca8k_phy_mmd_read(priv, phy, 7, 0x802f);
	rx_ok = qca8k_phy_mmd_read(priv, phy, 7, 0x802b);
	rx_ok_high16 = qca8k_phy_mmd_read(priv, phy, 7, 0x802a);
	rx_error = qca8k_phy_mmd_read(priv, phy, 7, 0x802c);
	tx_all_ok = tx_ok + (tx_ok_high16 << 16);
	rx_all_ok = rx_ok + (rx_ok_high16 << 16);

	if (tx_all_ok != count || tx_error != 0) {
		dev_dbg(priv->dev,
			"PHY%d tx_ok:%08x tx_err:%08x rx_ok:%08x rx_err:%08x\n",
			phy, tx_all_ok, tx_error, rx_all_ok, rx_error);
		return false;
	}

	return true;
}

static void
ar40xx_check_phy_reset_status(struct qca8k_priv *priv, int phy)
{
	u16 bmcr;

	bmcr = mdiobus_read(priv->bus, phy, MII_BMCR);
	if (bmcr & BMCR_RESET)
		dev_warn_once(priv->dev, "PHY %d reset is pending\n", phy);
}

static void
ar40xx_psgmii_single_phy_testing(struct qca8k_priv *priv, int phy)
{
	struct mii_bus *bus = priv->bus;
	int j;

	mdiobus_write(bus, phy, MII_BMCR, BMCR_RESET | BMCR_ANENABLE);
	ar40xx_check_phy_reset_status(priv, phy);

	mdiobus_write(bus, phy, MII_BMCR, BMCR_LOOPBACK | BMCR_FULLDPLX |
					  BMCR_SPEED1000);

	for (j = 0; j < AR40XX_PSGMII_CALB_NUM; j++) {
		u16 status;

		status = mdiobus_read(bus, phy, AR40XX_PHY_SPEC_STATUS);
		if (status & AR40XX_PHY_SPEC_STATUS_LINK)
			break;

		/* the polling interval to check if the PHY link up or not
		  * maxwait_timer: 750 ms +/-10 ms
		  * minwait_timer : 1 us +/- 0.1us
		  * time resides in minwait_timer ~ maxwait_timer
		  * see IEEE 802.3 section 40.4.5.2
		  */
		mdelay(8);
	}

	ar40xx_phytest_run(priv, phy);

	/* check counter */
	if (ar40xx_phytest_check_counters(priv, phy, 0x1000)) {
		priv->phy_t_status &= (~BIT(phy));
	} else {
		dev_info(priv->dev, "PHY %d single test PSGMII issue happen!\n", phy);
		priv->phy_t_status |= BIT(phy);
	}

	mdiobus_write(bus, phy, MII_BMCR, BMCR_ANENABLE | BMCR_PDOWN |
					  BMCR_SPEED1000);
}

static void
ar40xx_psgmii_all_phy_testing(struct qca8k_priv *priv)
{
	struct mii_bus *bus = priv->bus;
	int phy, j;

	mdiobus_write(bus, 0x1f, MII_BMCR, BMCR_RESET | BMCR_ANENABLE);
	for (phy = 0; phy < QCA8K_IPQ4019_NUM_PORTS - 1; phy++)
		ar40xx_check_phy_reset_status(priv, phy);

	mdiobus_write(bus, 0x1f, MII_BMCR, BMCR_LOOPBACK | BMCR_FULLDPLX |
					   BMCR_SPEED1000);

	for (j = 0; j < AR40XX_PSGMII_CALB_NUM; j++) {
		for (phy = 0; phy < QCA8K_IPQ4019_NUM_PORTS - 1; phy++) {
			u16 status;

			status = mdiobus_read(bus, phy, AR40XX_PHY_SPEC_STATUS);
			if (!(status & AR40XX_PHY_SPEC_STATUS_LINK))
				break;
		}

		if (phy >= (QCA8K_IPQ4019_NUM_PORTS - 1))
			break;
		/* The polling interva to check if the PHY link up or not */
		mdelay(8);
	}

	ar40xx_phytest_run(priv, 0x1f);

	for (phy = 0; phy < QCA8K_IPQ4019_NUM_PORTS - 1; phy++) {
		if (ar40xx_phytest_check_counters(priv, phy, 4096)) {
			/* success */
			priv->phy_t_status &= ~BIT(phy + 8);
		} else {
			dev_info(priv->dev, "PHY%d test see issue!\n", phy);
			priv->phy_t_status |= BIT(phy + 8);
		}
	}

	dev_dbg(priv->dev, "PHY all test 0x%x \r\n", priv->phy_t_status);
}

static void
ar40xx_psgmii_self_test(struct qca8k_priv *priv)
{
	struct mii_bus *bus = priv->bus;
	u32 i, phy;

	ar40xx_malibu_psgmii_ess_reset(priv);

	/* switch to access MII reg for copper */
	mdiobus_write(bus, 4, 0x1f, 0x8500);

	for (phy = 0; phy < QCA8K_IPQ4019_NUM_PORTS - 1; phy++) {
		/*enable phy mdio broadcast write*/
		qca8k_phy_mmd_write(priv, phy, 7, 0x8028, 0x801f);
	}

	/* force no link by power down */
	mdiobus_write(bus, 0x1f, MII_BMCR, BMCR_ANENABLE | BMCR_PDOWN |
					   BMCR_SPEED1000);

	/* Setup packet generator for loopback calibration */
	qca8k_phy_mmd_write(priv, 0x1f, 7, 0x8021, 0x1000); /* 4096 Packets */
	qca8k_phy_mmd_write(priv, 0x1f, 7, 0x8062, 0x05e0); /* 1524 Bytes */

	/* fix mdi status */
	mdiobus_write(bus, 0x1f, 0x10, 0x6800);
	for (i = 0; i < AR40XX_PSGMII_CALB_NUM; i++) {
		priv->phy_t_status = 0;

		for (phy = 0; phy < QCA8K_IPQ4019_NUM_PORTS - 1; phy++) {
			qca8k_rmw(priv, AR40XX_REG_PORT_LOOKUP(phy + 1),
				AR40XX_PORT_LOOKUP_LOOPBACK,
				AR40XX_PORT_LOOKUP_LOOPBACK);
		}

		for (phy = 0; phy < QCA8K_IPQ4019_NUM_PORTS - 1; phy++)
			ar40xx_psgmii_single_phy_testing(priv, phy);

		ar40xx_psgmii_all_phy_testing(priv);

		if (priv->phy_t_status)
			ar40xx_malibu_psgmii_ess_reset(priv);
		else
			break;
	}

	if (i >= AR40XX_PSGMII_CALB_NUM)
		dev_info(priv->dev, "PSGMII cannot recover\n");
	else
		dev_dbg(priv->dev, "PSGMII recovered after %d times reset\n", i);

	/* configuration recover */
	/* packet number */
	qca8k_phy_mmd_write(priv, 0x1f, 7, 0x8021, 0x0);
	/* disable check */
	qca8k_phy_mmd_write(priv, 0x1f, 7, 0x8029, 0x0);
	/* disable traffic */
	qca8k_phy_mmd_write(priv, 0x1f, 7, 0x8020, 0x0);
}

static void
ar40xx_psgmii_self_test_clean(struct qca8k_priv *priv)
{
	struct mii_bus *bus = priv->bus;
	int phy;

	/* disable phy internal loopback */
	mdiobus_write(bus, 0x1f, 0x10, 0x6860);
	mdiobus_write(bus, 0x1f, MII_BMCR, BMCR_ANENABLE | BMCR_RESET |
					   BMCR_SPEED1000);

	for (phy = 0; phy < QCA8K_IPQ4019_NUM_PORTS - 1; phy++) {
		/* disable mac loop back */
		qca8k_rmw(priv, AR40XX_REG_PORT_LOOKUP(phy + 1),
				AR40XX_PORT_LOOKUP_LOOPBACK, 0);

		/* disable phy mdio broadcast write */
		qca8k_phy_mmd_write(priv, phy, 7, 0x8028, 0x001f);
	}
}

static void
ar40xx_malibu_init(struct qca8k_priv *priv)
{
	int i;
	u16 val;

	/* war to enable AZ transmitting ability */
	qca8k_phy_mmd_write(priv, AR40XX_PSGMII_ID, 1,
		      AR40XX_MALIBU_PSGMII_MODE_CTRL,
		      AR40XX_MALIBU_PHY_PSGMII_MODE_CTRL_ADJUST_VAL);

	for (i = 0; i < QCA8K_IPQ4019_NUM_PORTS - 1; i++) {

		/* change malibu control_dac */
		val = qca8k_phy_mmd_read(priv, i, 7, AR40XX_MALIBU_PHY_MMD7_DAC_CTRL);
		val &= ~AR40XX_MALIBU_DAC_CTRL_MASK;
		val |= AR40XX_MALIBU_DAC_CTRL_VALUE;
		qca8k_phy_mmd_write(priv, i, 7, AR40XX_MALIBU_PHY_MMD7_DAC_CTRL, val);

		if (i == AR40XX_MALIBU_PHY_LAST_ADDR) {
			/* avoid PHY to get into hibernation */
			val = qca8k_phy_mmd_read(priv, i, 3,
						  AR40XX_MALIBU_PHY_RLP_CTRL);
			val &= (~(1<<1));
			qca8k_phy_mmd_write(priv, i, 3,
					     AR40XX_MALIBU_PHY_RLP_CTRL, val);
		}
	}

	/* adjust psgmii serdes tx amp */
	mdiobus_write(priv->bus, AR40XX_PSGMII_ID,
		      AR40XX_PSGMII_TX_DRIVER_1_CTRL,
		      AR40XX_MALIBU_PHY_PSGMII_REDUCE_SERDES_TX_AMP);
}

static void
ar40xx_mac_mode_init(struct qca8k_priv *priv)
{
	switch (priv->mac_mode) {
	case PORT_WRAPPER_PSGMII:
		ar40xx_malibu_init(priv);
		ar40xx_psgmii_self_test(priv);
		ar40xx_psgmii_self_test_clean(priv);

		psgmii_write(priv, AR40XX_PSGMII_MODE_CONTROL, 0x2200);
		psgmii_write(priv, AR40XX_PSGMIIPHY_TX_CONTROL, 0x8380);
		break;
	case PORT_WRAPPER_RGMII:
		qca8k_write(priv, AR40XX_REG_RGMII_CTRL, BIT(10));
		break;
	}
}

static void
qca8k_dsa_init_work(struct work_struct *work)
{
	struct qca8k_priv *priv = container_of(work, struct qca8k_priv, dsa_init.work);
	struct device *parent = priv->dev->parent;
	int ret;

	ret = dsa_register_switch(priv->ds);

	switch (ret) {
	case 0:
		return;

	case -EPROBE_DEFER:
		dev_dbg(priv->dev, "dsa_register_switch defered.\n");
		schedule_delayed_work(&priv->dsa_init, msecs_to_jiffies(200));
		return;

	default:
		dev_err(priv->dev, "dsa_register_switch failed with (%d).\n", ret);
		/* unbind anything failed */
		if (parent)
			device_lock(parent);

		device_release_driver(priv->dev);
		if (parent)
			device_unlock(parent);
		return;
	}
}

static int
qca8k_ipq4019_probe(struct platform_device *pdev)
{
	struct qca8k_priv *priv;
	void __iomem *base;
	struct device_node *np = pdev->dev.of_node, *mdio_np;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(priv->dev, base,
					     &qca8k_ipq4019_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(priv->dev, "regmap initialization failed, %d\n", ret);
		return ret;
	}

	priv->ess_clk = of_clk_get_by_name(np, "ess_clk");
	if (IS_ERR(priv->ess_clk)) {
		dev_err(&pdev->dev, "Failed to get ess_clk\n");
		return PTR_ERR(priv->ess_clk);
	}

	priv->ess_rst = devm_reset_control_get(&pdev->dev, "ess_rst");
	if (IS_ERR(priv->ess_rst)) {
		dev_err(&pdev->dev, "Failed to get ess_rst control!\n");
		return PTR_ERR(priv->ess_rst);
	}

	ret = of_property_read_u32(np, "mac-mode", &priv->mac_mode);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to get 'mac-mode' property\n");
		return -EINVAL;
	}

	priv->psgmii = syscon_regmap_lookup_by_phandle(np, "psgmii-phy");
	if (IS_ERR_OR_NULL(priv->psgmii)) {
		dev_err(&pdev->dev, "unable to get 'psgmii-phy' base\n");
		return -EINVAL;
	}

	mdio_np = of_parse_phandle(np, "mdio", 0);
	if (!mdio_np) {
		dev_err(&pdev->dev, "unable to get MDIO bus phandle\n");
		return -EINVAL;
	}

	priv->bus = of_mdio_find_bus(mdio_np);
	of_node_put(mdio_np);
	if (!priv->bus) {
		dev_err(&pdev->dev, "unable to find MDIO bus\n");
		return -EPROBE_DEFER;
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

	clk_prepare_enable(priv->ess_clk);

	ess_reset(priv);

	ar40xx_mac_mode_init(priv);

	reset_control_put(priv->ess_rst);

	/* Ok. What's going on with the delayed dsa_switch_register?!
	 *
	 * On Bootup, this switch driver loads before the ethernet
	 * driver. This causes a problem in dsa_register_switch when
	 * it parses the tree and encounters the not-yet-ready
	 * 	"ethernet = <&gmac>;" property.
	 *
	 * Which will err with -EPROBE_DEFER. Normally this should be
	 * OK and the driver will just get loaded at a later time.
	 * However, the EthernetSubSystem (ESS for short) really doesn't
	 * like being resetted more than once in this fashion and will
	 * "lock it up for good"... like "real good".
	 *
	 * So far, only a reboot can "unwedge" it, which is not what
	 * we want.
	 *
	 * So this workaround (running dsa_register_switch in a
	 * workqueue task) is employed to fix this unknown issue within
	 * the SoC for now.
	 */

	INIT_DELAYED_WORK(&priv->dsa_init, qca8k_dsa_init_work);
	schedule_delayed_work(&priv->dsa_init, msecs_to_jiffies(1000));

	return 0;
}

static const struct of_device_id qca8k_ipq4019_of_match[] = {
	{ .compatible = "qca,ipq4019-qca8337n" },
	{ /* sentinel */ },
};

static struct platform_driver qca8k_ipq4019_driver = {
	.driver = {
		.name = "qca8k-ipq4019",
		.of_match_table = qca8k_ipq4019_of_match,
	},
};

module_platform_driver_probe(qca8k_ipq4019_driver, qca8k_ipq4019_probe);

MODULE_AUTHOR("Mathieu Olivari, John Crispin <john@phrozen.org>");
MODULE_AUTHOR("Gabor Juhos <j4g8y7@gmail.com>, Robert Marko <robert.marko@sartura.hr>");
MODULE_DESCRIPTION("Qualcomm IPQ4019 built-in switch driver");
MODULE_LICENSE("GPL v2");
