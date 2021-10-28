// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2011-2012 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (c) 2015, 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016 John Crispin <john@phrozen.org>
 */

#include <linux/module.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <net/dsa.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/if_bridge.h>
#include <linux/mdio.h>
#include <linux/phylink.h>
#include <linux/gpio/consumer.h>
#include <linux/etherdevice.h>

#include "qca8k.h"

/* The 32bit switch registers are accessed indirectly. To achieve this we need
 * to set the page of the register. Track the last page that was set to reduce
 * mdio writes
 */
static u16 qca8k_current_page = 0xffff;

static void
qca8k_split_addr(u32 regaddr, u16 *r1, u16 *r2, u16 *page)
{
	regaddr >>= 1;
	*r1 = regaddr & 0x1e;

	regaddr >>= 5;
	*r2 = regaddr & 0x7;

	regaddr >>= 3;
	*page = regaddr & 0x3ff;
}

static int
qca8k_mii_read32(struct mii_bus *bus, int phy_id, u32 regnum, u32 *val)
{
	int ret;

	ret = bus->read(bus, phy_id, regnum);
	if (ret >= 0) {
		*val = ret;
		ret = bus->read(bus, phy_id, regnum + 1);
		*val |= ret << 16;
	}

	if (ret < 0) {
		dev_err_ratelimited(&bus->dev,
				    "failed to read qca8k 32bit register\n");
		*val = 0;
		return ret;
	}

	return 0;
}

static void
qca8k_mii_write32(struct mii_bus *bus, int phy_id, u32 regnum, u32 val)
{
	u16 lo, hi;
	int ret;

	lo = val & 0xffff;
	hi = (u16)(val >> 16);

	ret = bus->write(bus, phy_id, regnum, lo);
	if (ret >= 0)
		ret = bus->write(bus, phy_id, regnum + 1, hi);
	if (ret < 0)
		dev_err_ratelimited(&bus->dev,
				    "failed to write qca8k 32bit register\n");
}

static int
qca8k_set_page(struct mii_bus *bus, u16 page)
{
	int ret;

	if (page == qca8k_current_page)
		return 0;

	ret = bus->write(bus, 0x18, 0, page);
	if (ret < 0) {
		dev_err_ratelimited(&bus->dev,
				    "failed to set qca8k page\n");
		return ret;
	}

	qca8k_current_page = page;
	usleep_range(1000, 2000);
	return 0;
}

int
qca8k_read(struct qca8k_priv *priv, u32 reg, u32 *val)
{
	struct mii_bus *bus = priv->bus;
	u16 r1, r2, page;
	int ret;

	qca8k_split_addr(reg, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8k_set_page(bus, page);
	if (ret < 0)
		goto exit;

	ret = qca8k_mii_read32(bus, 0x10 | r2, r1, val);

exit:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

int
qca8k_write(struct qca8k_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;
	u16 r1, r2, page;
	int ret;

	qca8k_split_addr(reg, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8k_set_page(bus, page);
	if (ret < 0)
		goto exit;

	qca8k_mii_write32(bus, 0x10 | r2, r1, val);

exit:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

int
qca8k_rmw(struct qca8k_priv *priv, u32 reg, u32 mask, u32 write_val)
{
	struct mii_bus *bus = priv->bus;
	u16 r1, r2, page;
	u32 val;
	int ret;

	qca8k_split_addr(reg, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8k_set_page(bus, page);
	if (ret < 0)
		goto exit;

	ret = qca8k_mii_read32(bus, 0x10 | r2, r1, &val);
	if (ret < 0)
		goto exit;

	val &= ~mask;
	val |= write_val;
	qca8k_mii_write32(bus, 0x10 | r2, r1, val);

exit:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

int
qca8k_reg_set(struct qca8k_priv *priv, u32 reg, u32 val)
{
	return qca8k_rmw(priv, reg, 0, val);
}

int
qca8k_reg_clear(struct qca8k_priv *priv, u32 reg, u32 val)
{
	return qca8k_rmw(priv, reg, val, 0);
}

static int
qca8k_regmap_read(void *ctx, uint32_t reg, uint32_t *val)
{
	struct qca8k_priv *priv = (struct qca8k_priv *)ctx;

	return qca8k_read(priv, reg, val);
}

static int
qca8k_regmap_write(void *ctx, uint32_t reg, uint32_t val)
{
	struct qca8k_priv *priv = (struct qca8k_priv *)ctx;

	return qca8k_write(priv, reg, val);
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

static struct regmap_config qca8k_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x16ac, /* end MIB - Port6 range */
	.reg_read = qca8k_regmap_read,
	.reg_write = qca8k_regmap_write,
	.rd_table = &qca8k_readable_table,
};

int
qca8k_busy_wait(struct qca8k_priv *priv, u32 reg, u32 mask)
{
	int ret, ret1;
	u32 val;

	ret = read_poll_timeout(qca8k_read, ret1, !(val & mask),
				0, QCA8K_BUSY_WAIT_TIMEOUT * USEC_PER_MSEC, false,
				priv, reg, &val);

	/* Check if qca8k_read has failed for a different reason
	 * before returning -ETIMEDOUT
	 */
	if (ret < 0 && ret1 < 0)
		return ret1;

	return ret;
}

static u32
qca8k_port_to_phy(int port)
{
	/* From Andrew Lunn:
	 * Port 0 has no internal phy.
	 * Port 1 has an internal PHY at MDIO address 0.
	 * Port 2 has an internal PHY at MDIO address 1.
	 * ...
	 * Port 5 has an internal PHY at MDIO address 4.
	 * Port 6 has no internal PHY.
	 */

	return port - 1;
}

static int
qca8k_mdio_busy_wait(struct mii_bus *bus, u32 reg, u32 mask)
{
	u16 r1, r2, page;
	u32 val;
	int ret, ret1;

	qca8k_split_addr(reg, &r1, &r2, &page);

	ret = read_poll_timeout(qca8k_mii_read32, ret1, !(val & mask), 0,
				QCA8K_BUSY_WAIT_TIMEOUT * USEC_PER_MSEC, false,
				bus, 0x10 | r2, r1, &val);

	/* Check if qca8k_read has failed for a different reason
	 * before returnting -ETIMEDOUT
	 */
	if (ret < 0 && ret1 < 0)
		return ret1;

	return ret;
}

static int
qca8k_mdio_write(struct mii_bus *bus, int phy, int regnum, u16 data)
{
	u16 r1, r2, page;
	u32 val;
	int ret;

	if (regnum >= QCA8K_MDIO_MASTER_MAX_REG)
		return -EINVAL;

	val = QCA8K_MDIO_MASTER_BUSY | QCA8K_MDIO_MASTER_EN |
	      QCA8K_MDIO_MASTER_WRITE | QCA8K_MDIO_MASTER_PHY_ADDR(phy) |
	      QCA8K_MDIO_MASTER_REG_ADDR(regnum) |
	      QCA8K_MDIO_MASTER_DATA(data);

	qca8k_split_addr(QCA8K_MDIO_MASTER_CTRL, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8k_set_page(bus, page);
	if (ret)
		goto exit;

	qca8k_mii_write32(bus, 0x10 | r2, r1, val);

	ret = qca8k_mdio_busy_wait(bus, QCA8K_MDIO_MASTER_CTRL,
				   QCA8K_MDIO_MASTER_BUSY);

exit:
	/* even if the busy_wait timeouts try to clear the MASTER_EN */
	qca8k_mii_write32(bus, 0x10 | r2, r1, 0);

	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static int
qca8k_mdio_read(struct mii_bus *bus, int phy, int regnum)
{
	u16 r1, r2, page;
	u32 val;
	int ret;

	if (regnum >= QCA8K_MDIO_MASTER_MAX_REG)
		return -EINVAL;

	val = QCA8K_MDIO_MASTER_BUSY | QCA8K_MDIO_MASTER_EN |
	      QCA8K_MDIO_MASTER_READ | QCA8K_MDIO_MASTER_PHY_ADDR(phy) |
	      QCA8K_MDIO_MASTER_REG_ADDR(regnum);

	qca8k_split_addr(QCA8K_MDIO_MASTER_CTRL, &r1, &r2, &page);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = qca8k_set_page(bus, page);
	if (ret)
		goto exit;

	qca8k_mii_write32(bus, 0x10 | r2, r1, val);

	ret = qca8k_mdio_busy_wait(bus, QCA8K_MDIO_MASTER_CTRL,
				   QCA8K_MDIO_MASTER_BUSY);
	if (ret)
		goto exit;

	ret = qca8k_mii_read32(bus, 0x10 | r2, r1, &val);

exit:
	/* even if the busy_wait timeouts try to clear the MASTER_EN */
	qca8k_mii_write32(bus, 0x10 | r2, r1, 0);

	mutex_unlock(&bus->mdio_lock);

	if (ret >= 0)
		ret = val & QCA8K_MDIO_MASTER_DATA_MASK;

	return ret;
}

static int
qca8k_internal_mdio_write(struct mii_bus *slave_bus, int phy, int regnum, u16 data)
{
	struct qca8k_priv *priv = slave_bus->priv;
	struct mii_bus *bus = priv->bus;

	return qca8k_mdio_write(bus, phy, regnum, data);
}

static int
qca8k_internal_mdio_read(struct mii_bus *slave_bus, int phy, int regnum)
{
	struct qca8k_priv *priv = slave_bus->priv;
	struct mii_bus *bus = priv->bus;

	return qca8k_mdio_read(bus, phy, regnum);
}

static int
qca8k_phy_write(struct dsa_switch *ds, int port, int regnum, u16 data)
{
	struct qca8k_priv *priv = ds->priv;

	/* Check if the legacy mapping should be used and the
	 * port is not correctly mapped to the right PHY in the
	 * devicetree
	 */
	if (priv->legacy_phy_port_mapping)
		port = qca8k_port_to_phy(port) % PHY_MAX_ADDR;

	return qca8k_mdio_write(priv->bus, port, regnum, data);
}

static int
qca8k_phy_read(struct dsa_switch *ds, int port, int regnum)
{
	struct qca8k_priv *priv = ds->priv;
	int ret;

	/* Check if the legacy mapping should be used and the
	 * port is not correctly mapped to the right PHY in the
	 * devicetree
	 */
	if (priv->legacy_phy_port_mapping)
		port = qca8k_port_to_phy(port) % PHY_MAX_ADDR;

	ret = qca8k_mdio_read(priv->bus, port, regnum);

	if (ret < 0)
		return 0xffff;

	return ret;
}

static int
qca8k_mdio_register(struct qca8k_priv *priv, struct device_node *mdio)
{
	struct dsa_switch *ds = priv->ds;
	struct mii_bus *bus;

	bus = devm_mdiobus_alloc(ds->dev);

	if (!bus)
		return -ENOMEM;

	bus->priv = (void *)priv;
	bus->name = "qca8k slave mii";
	bus->read = qca8k_internal_mdio_read;
	bus->write = qca8k_internal_mdio_write;
	snprintf(bus->id, MII_BUS_ID_SIZE, "qca8k-%d",
		 ds->index);

	bus->parent = ds->dev;
	bus->phy_mask = ~ds->phys_mii_mask;

	ds->slave_mii_bus = bus;

	return devm_of_mdiobus_register(priv->dev, bus, mdio);
}

static int
qca8k_setup_mdio_bus(struct qca8k_priv *priv)
{
	u32 internal_mdio_mask = 0, external_mdio_mask = 0, reg;
	struct device_node *ports, *port, *mdio;
	phy_interface_t mode;
	int err;

	ports = of_get_child_by_name(priv->dev->of_node, "ports");
	if (!ports)
		ports = of_get_child_by_name(priv->dev->of_node, "ethernet-ports");

	if (!ports)
		return -EINVAL;

	for_each_available_child_of_node(ports, port) {
		err = of_property_read_u32(port, "reg", &reg);
		if (err) {
			of_node_put(port);
			of_node_put(ports);
			return err;
		}

		if (!dsa_is_user_port(priv->ds, reg))
			continue;

		of_get_phy_mode(port, &mode);

		if (of_property_read_bool(port, "phy-handle") &&
		    mode != PHY_INTERFACE_MODE_INTERNAL)
			external_mdio_mask |= BIT(reg);
		else
			internal_mdio_mask |= BIT(reg);
	}

	of_node_put(ports);
	if (!external_mdio_mask && !internal_mdio_mask) {
		dev_err(priv->dev, "no PHYs are defined.\n");
		return -EINVAL;
	}

	/* The QCA8K_MDIO_MASTER_EN Bit, which grants access to PHYs through
	 * the MDIO_MASTER register also _disconnects_ the external MDC
	 * passthrough to the internal PHYs. It's not possible to use both
	 * configurations at the same time!
	 *
	 * Because this came up during the review process:
	 * If the external mdio-bus driver is capable magically disabling
	 * the QCA8K_MDIO_MASTER_EN and mutex/spin-locking out the qca8k's
	 * accessors for the time being, it would be possible to pull this
	 * off.
	 */
	if (!!external_mdio_mask && !!internal_mdio_mask) {
		dev_err(priv->dev, "either internal or external mdio bus configuration is supported.\n");
		return -EINVAL;
	}

	if (external_mdio_mask) {
		/* Make sure to disable the internal mdio bus in cases
		 * a dt-overlay and driver reload changed the configuration
		 */

		return qca8k_reg_clear(priv, QCA8K_MDIO_MASTER_CTRL,
				       QCA8K_MDIO_MASTER_EN);
	}

	/* Check if the devicetree declare the port:phy mapping */
	mdio = of_get_child_by_name(priv->dev->of_node, "mdio");
	if (of_device_is_available(mdio)) {
		err = qca8k_mdio_register(priv, mdio);
		if (err)
			of_node_put(mdio);

		return err;
	}

	/* If a mapping can't be found the legacy mapping is used,
	 * using the qca8k_port_to_phy function
	 */
	priv->legacy_phy_port_mapping = true;
	priv->ops.phy_read = qca8k_phy_read;
	priv->ops.phy_write = qca8k_phy_write;

	return 0;
}

static int
qca8k_setup_of_rgmii_delay(struct qca8k_priv *priv)
{
	struct device_node *port_dn;
	phy_interface_t mode;
	struct dsa_port *dp;
	u32 val;

	/* CPU port is already checked */
	dp = dsa_to_port(priv->ds, 0);

	port_dn = dp->dn;

	/* Check if port 0 is set to the correct type */
	of_get_phy_mode(port_dn, &mode);
	if (mode != PHY_INTERFACE_MODE_RGMII_ID &&
	    mode != PHY_INTERFACE_MODE_RGMII_RXID &&
	    mode != PHY_INTERFACE_MODE_RGMII_TXID) {
		return 0;
	}

	switch (mode) {
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
		if (of_property_read_u32(port_dn, "rx-internal-delay-ps", &val))
			val = 2;
		else
			/* Switch regs accept value in ns, convert ps to ns */
			val = val / 1000;

		if (val > QCA8K_MAX_DELAY) {
			dev_err(priv->dev, "rgmii rx delay is limited to a max value of 3ns, setting to the max value");
			val = 3;
		}

		priv->rgmii_rx_delay = val;
		/* Stop here if we need to check only for rx delay */
		if (mode != PHY_INTERFACE_MODE_RGMII_ID)
			break;

		fallthrough;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		if (of_property_read_u32(port_dn, "tx-internal-delay-ps", &val))
			val = 1;
		else
			/* Switch regs accept value in ns, convert ps to ns */
			val = val / 1000;

		if (val > QCA8K_MAX_DELAY) {
			dev_err(priv->dev, "rgmii tx delay is limited to a max value of 3ns, setting to the max value");
			val = 3;
		}

		priv->rgmii_tx_delay = val;
		break;
	default:
		return 0;
	}

	return 0;
}

void
qca8k_port_set_status(struct qca8k_priv *priv, int port, int enable)
{
	u32 mask = QCA8K_PORT_STATUS_TXMAC | QCA8K_PORT_STATUS_RXMAC;

	/* Port 0 and 6 have no internal PHY */
	if (port > 0 && port < 6)
		mask |= QCA8K_PORT_STATUS_LINK_AUTO;

	if (enable)
		qca8k_reg_set(priv, QCA8K_REG_PORT_STATUS(port), mask);
	else
		qca8k_reg_clear(priv, QCA8K_REG_PORT_STATUS(port), mask);
}

static int
qca8k_setup(struct dsa_switch *ds)
{
	struct qca8k_priv *priv = (struct qca8k_priv *)ds->priv;
	int ret, i;
	u32 mask;

	/* Make sure that port 0 is the cpu port */
	if (!dsa_is_cpu_port(ds, 0)) {
		dev_err(priv->dev, "port 0 is not the CPU port");
		return -EINVAL;
	}

	mutex_init(&priv->reg_mutex);

	/* Start by setting up the register mapping */
	priv->regmap = devm_regmap_init(ds->dev, NULL, priv,
					&qca8k_regmap_config);
	if (IS_ERR(priv->regmap))
		dev_warn(priv->dev, "regmap initialization failed");

	ret = qca8k_setup_mdio_bus(priv);
	if (ret)
		return ret;

	ret = qca8k_setup_of_rgmii_delay(priv);
	if (ret)
		return ret;

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
		dev_warn(priv->dev, "mib init failed");

	/* Enable QCA header mode on the cpu port */
	ret = qca8k_write(priv, QCA8K_REG_PORT_HDR_CTRL(QCA8K_CPU_PORT),
			  QCA8K_PORT_HDR_CTRL_ALL << QCA8K_PORT_HDR_CTRL_TX_S |
			  QCA8K_PORT_HDR_CTRL_ALL << QCA8K_PORT_HDR_CTRL_RX_S);
	if (ret) {
		dev_err(priv->dev, "failed enabling QCA header mode");
		return ret;
	}

	/* Disable forwarding by default on all ports */
	for (i = 0; i < QCA8K_NUM_PORTS; i++) {
		ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(i),
				QCA8K_PORT_LOOKUP_MEMBER, 0);
		if (ret)
			return ret;
	}

	/* Disable MAC by default on all ports */
	for (i = 1; i < QCA8K_NUM_PORTS; i++)
		qca8k_port_set_status(priv, i, 0);

	/* Forward all unknown frames to CPU port for Linux processing */
	ret = qca8k_write(priv, QCA8K_REG_GLOBAL_FW_CTRL1,
			  BIT(0) << QCA8K_GLOBAL_FW_CTRL1_IGMP_DP_S |
			  BIT(0) << QCA8K_GLOBAL_FW_CTRL1_BC_DP_S |
			  BIT(0) << QCA8K_GLOBAL_FW_CTRL1_MC_DP_S |
			  BIT(0) << QCA8K_GLOBAL_FW_CTRL1_UC_DP_S);
	if (ret)
		return ret;

	/* Setup connection between CPU port & user ports */
	for (i = 0; i < QCA8K_NUM_PORTS; i++) {
		/* CPU port gets connected to all user ports of the switch */
		if (dsa_is_cpu_port(ds, i)) {
			ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(QCA8K_CPU_PORT),
					QCA8K_PORT_LOOKUP_MEMBER, dsa_user_ports(ds));
			if (ret)
				return ret;
		}

		/* Individual user ports get connected to CPU port only */
		if (dsa_is_user_port(ds, i)) {
			int shift = 16 * (i % 2);

			ret = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(i),
					QCA8K_PORT_LOOKUP_MEMBER,
					BIT(QCA8K_CPU_PORT));
			if (ret)
				return ret;

			/* Enable ARP Auto-learning by default */
			ret = qca8k_reg_set(priv, QCA8K_PORT_LOOKUP_CTRL(i),
					    QCA8K_PORT_LOOKUP_LEARN);
			if (ret)
				return ret;

			/* For port based vlans to work we need to set the
			 * default egress vid
			 */
			ret = qca8k_rmw(priv, QCA8K_EGRESS_VLAN(i),
					0xfff << shift,
					QCA8K_PORT_VID_DEF << shift);
			if (ret)
				return ret;

			ret = qca8k_write(priv, QCA8K_REG_PORT_VLAN_CTRL0(i),
					  QCA8K_PORT_VLAN_CVID(QCA8K_PORT_VID_DEF) |
					  QCA8K_PORT_VLAN_SVID(QCA8K_PORT_VID_DEF));
			if (ret)
				return ret;
		}
	}

	/* The port 5 of the qca8337 have some problem in flood condition. The
	 * original legacy driver had some specific buffer and priority settings
	 * for the different port suggested by the QCA switch team. Add this
	 * missing settings to improve switch stability under load condition.
	 * This problem is limited to qca8337 and other qca8k switch are not affected.
	 */
	if (priv->switch_id == QCA8K_ID_QCA8337) {
		for (i = 0; i < QCA8K_NUM_PORTS; i++) {
			switch (i) {
			/* The 2 CPU port and port 5 requires some different
			 * priority than any other ports.
			 */
			case 0:
			case 5:
			case 6:
				mask = QCA8K_PORT_HOL_CTRL0_EG_PRI0(0x3) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI1(0x4) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI2(0x4) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI3(0x4) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI4(0x6) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI5(0x8) |
					QCA8K_PORT_HOL_CTRL0_EG_PORT(0x1e);
				break;
			default:
				mask = QCA8K_PORT_HOL_CTRL0_EG_PRI0(0x3) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI1(0x4) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI2(0x6) |
					QCA8K_PORT_HOL_CTRL0_EG_PRI3(0x8) |
					QCA8K_PORT_HOL_CTRL0_EG_PORT(0x19);
			}
			qca8k_write(priv, QCA8K_REG_PORT_HOL_CTRL0(i), mask);

			mask = QCA8K_PORT_HOL_CTRL1_ING(0x6) |
			QCA8K_PORT_HOL_CTRL1_EG_PRI_BUF_EN |
			QCA8K_PORT_HOL_CTRL1_EG_PORT_BUF_EN |
			QCA8K_PORT_HOL_CTRL1_WRED_EN;
			qca8k_rmw(priv, QCA8K_REG_PORT_HOL_CTRL1(i),
				  QCA8K_PORT_HOL_CTRL1_ING_BUF |
				  QCA8K_PORT_HOL_CTRL1_EG_PRI_BUF_EN |
				  QCA8K_PORT_HOL_CTRL1_EG_PORT_BUF_EN |
				  QCA8K_PORT_HOL_CTRL1_WRED_EN,
				  mask);
		}
	}

	/* Special GLOBAL_FC_THRESH value are needed for ar8327 switch */
	if (priv->switch_id == QCA8K_ID_QCA8327) {
		mask = QCA8K_GLOBAL_FC_GOL_XON_THRES(288) |
		       QCA8K_GLOBAL_FC_GOL_XOFF_THRES(496);
		qca8k_rmw(priv, QCA8K_REG_GLOBAL_FC_THRESH,
			  QCA8K_GLOBAL_FC_GOL_XON_THRES_S |
			  QCA8K_GLOBAL_FC_GOL_XOFF_THRES_S,
			  mask);
	}

	/* Setup our port MTUs to match power on defaults */
	for (i = 0; i < QCA8K_NUM_PORTS; i++)
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
qca8k_phylink_mac_config(struct dsa_switch *ds, int port, unsigned int mode,
			 const struct phylink_link_state *state)
{
	struct qca8k_priv *priv = ds->priv;
	u32 reg, val;
	int ret;

	switch (port) {
	case 0: /* 1st CPU port */
		if (state->interface != PHY_INTERFACE_MODE_RGMII &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_ID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_TXID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_RXID &&
		    state->interface != PHY_INTERFACE_MODE_SGMII)
			return;

		reg = QCA8K_REG_PORT0_PAD_CTRL;
		break;
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
		/* Internal PHY, nothing to do */
		return;
	case 6: /* 2nd CPU port / external PHY */
		if (state->interface != PHY_INTERFACE_MODE_RGMII &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_ID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_TXID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_RXID &&
		    state->interface != PHY_INTERFACE_MODE_SGMII &&
		    state->interface != PHY_INTERFACE_MODE_1000BASEX)
			return;

		reg = QCA8K_REG_PORT6_PAD_CTRL;
		break;
	default:
		dev_err(ds->dev, "%s: unsupported port: %i\n", __func__, port);
		return;
	}

	if (port != 6 && phylink_autoneg_inband(mode)) {
		dev_err(ds->dev, "%s: in-band negotiation unsupported\n",
			__func__);
		return;
	}

	switch (state->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		/* RGMII mode means no delay so don't enable the delay */
		qca8k_write(priv, reg, QCA8K_PORT_PAD_RGMII_EN);
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
		/* RGMII_ID needs internal delay. This is enabled through
		 * PORT5_PAD_CTRL for all ports, rather than individual port
		 * registers
		 */
		qca8k_write(priv, reg,
			    QCA8K_PORT_PAD_RGMII_EN |
			    QCA8K_PORT_PAD_RGMII_TX_DELAY(priv->rgmii_tx_delay) |
			    QCA8K_PORT_PAD_RGMII_RX_DELAY(priv->rgmii_rx_delay) |
			    QCA8K_PORT_PAD_RGMII_TX_DELAY_EN |
			    QCA8K_PORT_PAD_RGMII_RX_DELAY_EN);
		/* QCA8337 requires to set rgmii rx delay */
		if (priv->switch_id == QCA8K_ID_QCA8337)
			qca8k_write(priv, QCA8K_REG_PORT5_PAD_CTRL,
				    QCA8K_PORT_PAD_RGMII_RX_DELAY_EN);
		break;
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		/* Enable SGMII on the port */
		qca8k_write(priv, reg, QCA8K_PORT_PAD_SGMII_EN);

		/* Enable/disable SerDes auto-negotiation as necessary */
		ret = qca8k_read(priv, QCA8K_REG_PWS, &val);
		if (ret)
			return;
		if (phylink_autoneg_inband(mode))
			val &= ~QCA8K_PWS_SERDES_AEN_DIS;
		else
			val |= QCA8K_PWS_SERDES_AEN_DIS;
		qca8k_write(priv, QCA8K_REG_PWS, val);

		/* Configure the SGMII parameters */
		ret = qca8k_read(priv, QCA8K_REG_SGMII_CTRL, &val);
		if (ret)
			return;

		val |= QCA8K_SGMII_EN_PLL | QCA8K_SGMII_EN_RX |
			QCA8K_SGMII_EN_TX | QCA8K_SGMII_EN_SD;

		if (dsa_is_cpu_port(ds, port)) {
			/* CPU port, we're talking to the CPU MAC, be a PHY */
			val &= ~QCA8K_SGMII_MODE_CTRL_MASK;
			val |= QCA8K_SGMII_MODE_CTRL_PHY;
		} else if (state->interface == PHY_INTERFACE_MODE_SGMII) {
			val &= ~QCA8K_SGMII_MODE_CTRL_MASK;
			val |= QCA8K_SGMII_MODE_CTRL_MAC;
		} else if (state->interface == PHY_INTERFACE_MODE_1000BASEX) {
			val &= ~QCA8K_SGMII_MODE_CTRL_MASK;
			val |= QCA8K_SGMII_MODE_CTRL_BASEX;
		}

		qca8k_write(priv, QCA8K_REG_SGMII_CTRL, val);
		break;
	default:
		dev_err(ds->dev, "xMII mode %s not supported for port %d\n",
			phy_modes(state->interface), port);
		return;
	}
}

static void
qca8k_phylink_validate(struct dsa_switch *ds, int port,
		       unsigned long *supported,
		       struct phylink_link_state *state)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	switch (port) {
	case 0: /* 1st CPU port */
		if (state->interface != PHY_INTERFACE_MODE_NA &&
		    state->interface != PHY_INTERFACE_MODE_RGMII &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_ID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_TXID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_RXID &&
		    state->interface != PHY_INTERFACE_MODE_SGMII)
			goto unsupported;
		break;
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
		/* Internal PHY */
		if (state->interface != PHY_INTERFACE_MODE_NA &&
		    state->interface != PHY_INTERFACE_MODE_GMII &&
		    state->interface != PHY_INTERFACE_MODE_INTERNAL)
			goto unsupported;
		break;
	case 6: /* 2nd CPU port / external PHY */
		if (state->interface != PHY_INTERFACE_MODE_NA &&
		    state->interface != PHY_INTERFACE_MODE_RGMII &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_ID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_TXID &&
		    state->interface != PHY_INTERFACE_MODE_RGMII_RXID &&
		    state->interface != PHY_INTERFACE_MODE_SGMII &&
		    state->interface != PHY_INTERFACE_MODE_1000BASEX)
			goto unsupported;
		break;
	default:
unsupported:
		linkmode_zero(supported);
		return;
	}

	phylink_set_port_modes(mask);
	phylink_set(mask, Autoneg);

	phylink_set(mask, 1000baseT_Full);
	phylink_set(mask, 10baseT_Half);
	phylink_set(mask, 10baseT_Full);
	phylink_set(mask, 100baseT_Half);
	phylink_set(mask, 100baseT_Full);

	if (state->interface == PHY_INTERFACE_MODE_1000BASEX)
		phylink_set(mask, 1000baseX_Full);

	phylink_set(mask, Pause);
	phylink_set(mask, Asym_Pause);

	linkmode_and(supported, supported, mask);
	linkmode_and(state->advertising, state->advertising, mask);
}

static u32 qca8k_get_phy_flags(struct dsa_switch *ds, int port)
{
	struct qca8k_priv *priv = ds->priv;

	/* Communicate to the phy internal driver the switch revision.
	 * Based on the switch revision different values needs to be
	 * set to the dbg and mmd reg on the phy.
	 * The first 2 bit are used to communicate the switch revision
	 * to the phy driver.
	 */
	if (port > 0 && port < 6)
		return priv->switch_revision;

	return 0;
}

static enum dsa_tag_protocol
qca8k_get_tag_protocol(struct dsa_switch *ds, int port,
		       enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_QCA;
}

static const struct dsa_switch_ops qca8k_switch_ops = {
	.get_tag_protocol	= qca8k_get_tag_protocol,
	.setup			= qca8k_setup,
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
	.phylink_validate	= qca8k_phylink_validate,
	.phylink_mac_link_state	= qca8k_phylink_mac_link_state,
	.phylink_mac_config	= qca8k_phylink_mac_config,
	.phylink_mac_link_down	= qca8k_phylink_mac_link_down,
	.phylink_mac_link_up	= qca8k_phylink_mac_link_up,
	.get_phy_flags		= qca8k_get_phy_flags,
};

static int qca8k_read_switch_id(struct qca8k_priv *priv)
{
	const struct qca8k_match_data *data;
	u32 val;
	u8 id;
	int ret;

	/* get the switches ID from the compatible */
	data = of_device_get_match_data(priv->dev);
	if (!data)
		return -ENODEV;

	ret = qca8k_read(priv, QCA8K_REG_MASK_CTRL, &val);
	if (ret < 0)
		return -ENODEV;

	id = QCA8K_MASK_CTRL_DEVICE_ID(val & QCA8K_MASK_CTRL_DEVICE_ID_MASK);
	if (id != data->id) {
		dev_err(priv->dev, "Switch id detected %x but expected %x", id, data->id);
		return -ENODEV;
	}

	priv->switch_id = id;

	/* Save revision to communicate to the internal PHY driver */
	priv->switch_revision = (val & QCA8K_MASK_CTRL_REV_ID_MASK);

	return 0;
}

static int
qca8k_sw_probe(struct mdio_device *mdiodev)
{
	struct qca8k_priv *priv;
	int ret;

	/* allocate the private data struct so that we can probe the switches
	 * ID register
	 */
	priv = devm_kzalloc(&mdiodev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->bus = mdiodev->bus;
	priv->dev = &mdiodev->dev;

	priv->reset_gpio = devm_gpiod_get_optional(priv->dev, "reset",
						   GPIOD_ASIS);
	if (IS_ERR(priv->reset_gpio))
		return PTR_ERR(priv->reset_gpio);

	if (priv->reset_gpio) {
		gpiod_set_value_cansleep(priv->reset_gpio, 1);
		/* The active low duration must be greater than 10 ms
		 * and checkpatch.pl wants 20 ms.
		 */
		msleep(20);
		gpiod_set_value_cansleep(priv->reset_gpio, 0);
	}

	/* Check the detected switch id */
	ret = qca8k_read_switch_id(priv);
	if (ret)
		return ret;

	priv->ds = devm_kzalloc(&mdiodev->dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = &mdiodev->dev;
	priv->ds->num_ports = QCA8K_NUM_PORTS;
	priv->ds->priv = priv;
	priv->ops = qca8k_switch_ops;
	priv->ds->ops = &priv->ops;
	mutex_init(&priv->reg_mutex);
	dev_set_drvdata(&mdiodev->dev, priv);

	return dsa_register_switch(priv->ds);
}

static void
qca8k_sw_remove(struct mdio_device *mdiodev)
{
	struct qca8k_priv *priv = dev_get_drvdata(&mdiodev->dev);
	int i;

	if (!priv)
		return;

	for (i = 0; i < QCA8K_NUM_PORTS; i++)
		qca8k_port_set_status(priv, i, 0);

	dsa_unregister_switch(priv->ds);

	dev_set_drvdata(&mdiodev->dev, NULL);
}

static void qca8k_sw_shutdown(struct mdio_device *mdiodev)
{
	struct qca8k_priv *priv = dev_get_drvdata(&mdiodev->dev);

	if (!priv)
		return;

	dsa_switch_shutdown(priv->ds);

	dev_set_drvdata(&mdiodev->dev, NULL);
}

#ifdef CONFIG_PM_SLEEP
static void
qca8k_set_pm(struct qca8k_priv *priv, int enable)
{
	int i;

	for (i = 0; i < QCA8K_NUM_PORTS; i++) {
		if (!priv->port_sts[i].enabled)
			continue;

		qca8k_port_set_status(priv, i, enable);
	}
}

static int qca8k_suspend(struct device *dev)
{
	struct qca8k_priv *priv = dev_get_drvdata(dev);

	qca8k_set_pm(priv, 0);

	return dsa_switch_suspend(priv->ds);
}

static int qca8k_resume(struct device *dev)
{
	struct qca8k_priv *priv = dev_get_drvdata(dev);

	qca8k_set_pm(priv, 1);

	return dsa_switch_resume(priv->ds);
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(qca8k_pm_ops,
			 qca8k_suspend, qca8k_resume);

static const struct qca8k_match_data qca832x = {
	.id = QCA8K_ID_QCA8327,
};

static const struct qca8k_match_data qca833x = {
	.id = QCA8K_ID_QCA8337,
};

static const struct of_device_id qca8k_of_match[] = {
	{ .compatible = "qca,qca8327", .data = &qca832x },
	{ .compatible = "qca,qca8334", .data = &qca833x },
	{ .compatible = "qca,qca8337", .data = &qca833x },
	{ /* sentinel */ },
};

static struct mdio_driver qca8kmdio_driver = {
	.probe  = qca8k_sw_probe,
	.remove = qca8k_sw_remove,
	.shutdown = qca8k_sw_shutdown,
	.mdiodrv.driver = {
		.name = "qca8k",
		.of_match_table = qca8k_of_match,
		.pm = &qca8k_pm_ops,
	},
};

mdio_module_driver(qca8kmdio_driver);

MODULE_AUTHOR("Mathieu Olivari, John Crispin <john@phrozen.org>");
MODULE_DESCRIPTION("Driver for QCA8K ethernet switch family");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:qca8k");
