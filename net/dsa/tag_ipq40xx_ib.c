// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2020, Gabor Juhos <j4g8y7@gmail.com> */

#include <linux/bitfield.h>
#include <linux/dsa/ipq40xx.h>
#include <linux/soc/qcom/ipq40xx-edma.h>

#include "dsa_priv.h"

static struct sk_buff *ipq40xx_ib_tag_xmit(struct sk_buff *skb,
					   struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	u16 tci;
	int err;

	tci = FIELD_PREP(IPQ40XX_DSA_DP_MASK, BIT(dp->index));
	tci |= IPQ40XX_DSA_FROM_CPU;

	err = __vlan_insert_tag(skb, cpu_to_be16(IPQ40XX_DSA_TAG_PROTO), tci);
	if (err)
		return NULL;

	return skb;
}

static struct sk_buff *ipq40xx_ib_tag_rcv(struct sk_buff *skb,
					  struct net_device *dev,
					  struct packet_type *pt)
{
	struct edma_rrd *rrd;
	int offset;
	int port;

	offset = EDMA_RRD_SIZE + ETH_HLEN;
	if (unlikely(skb_headroom(skb) < offset))
		return NULL;

	rrd = (struct edma_rrd *)(skb->data - offset);
	port = FIELD_GET(EDMA_RRD1_PORT_ID_MASK, rrd->rrd1);

	skb->dev = dsa_master_find_slave(dev, 0, port);
	if (!skb->dev)
		return NULL;

	return skb;
}

const struct dsa_device_ops ipq40xx_ib_tag_ops = {
	.name	= "ipq40xx-ib",
	.proto	= DSA_TAG_PROTO_IPQ40XX_IB,
	.xmit	= ipq40xx_ib_tag_xmit,
	.rcv	= ipq40xx_ib_tag_rcv,
	.overhead = VLAN_HLEN,
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DSA tag driver for the IPQ40xx SoCs' built-in ethernet switch");
MODULE_AUTHOR("Gabor Juhos <j4g8y7@gmail.com>");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_IPQ40XX_IB);

module_dsa_tag_driver(ipq40xx_ib_tag_ops);
