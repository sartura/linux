// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2021, Gabor Juhos <j4g8y7@gmail.com> */

#include <linux/bitfield.h>
#include <linux/dsa/ipq4019.h>

#include "dsa_priv.h"

/* Receive Return Descriptor */
struct edma_rrd {
	u16 rrd0;
	u16 rrd1;
	u16 rrd2;
	u16 rrd3;
	u16 rrd4;
	u16 rrd5;
	u16 rrd6;
	u16 rrd7;
} __packed;

#define EDMA_RRD_SIZE			sizeof(struct edma_rrd)

#define EDMA_RRD1_PORT_ID_MASK		GENMASK(14, 12)

static struct sk_buff *ipq4019_sh_tag_xmit(struct sk_buff *skb,
					   struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	struct ipq40xx_dsa_tag_data *tag_data;

	BUILD_BUG_ON(sizeof_field(struct skb_shared_info, dsa_tag_data) <
		     sizeof(struct ipq40xx_dsa_tag_data));

	skb_shinfo(skb)->dsa_tag_proto = DSA_TAG_PROTO_IPQ4019;
	tag_data = (struct ipq40xx_dsa_tag_data *)skb_shinfo(skb)->dsa_tag_data;

	tag_data->from_cpu = 1;
	/* set the destination port information */
	tag_data->dp = BIT(dp->index);

	return skb;
}

static struct sk_buff *ipq4019_sh_tag_rcv(struct sk_buff *skb,
					  struct net_device *dev)
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

const struct dsa_device_ops ipq4019_sh_tag_dsa_ops = {
	.name	= "ipq4019-sh",
	.proto	= DSA_TAG_PROTO_IPQ4019,
	.xmit	= ipq4019_sh_tag_xmit,
	.rcv	= ipq4019_sh_tag_rcv,
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DSA tag driver for the IPQ4019 SoC built-in ethernet switch");
MODULE_AUTHOR("Gabor Juhos <j4g8y7@gmail.com>");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_IPQ4019);

module_dsa_tag_driver(ipq4019_sh_tag_dsa_ops);
