// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2020, Gabor Juhos <j4g8y7@gmail.com> */

#include <linux/bitfield.h>
#include <linux/dsa/ipq40xx.h>
#include <linux/soc/qcom/ipq40xx-edma.h>

#include "dsa_priv.h"

static struct sk_buff *ipq40xx_tag_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	struct ipq40xx_dsa_tag_data *tag_data;
	struct dsa_skb_ext *ext;

	ext = dsa_skb_ext_add(skb, DSA_TAG_PROTO_IPQ40XX,
			      struct ipq40xx_dsa_tag_data);

	tag_data = (struct ipq40xx_dsa_tag_data *)ext->tag_data;

	tag_data->from_cpu = 1;
	/* set the destination port information */
	tag_data->dp = BIT(dp->index);

	return skb;
}

static struct sk_buff *ipq40xx_tag_rcv(struct sk_buff *skb,
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

const struct dsa_device_ops ipq40xx_tag_dsa_ops = {
	.name	= "ipq40xx-ext",
	.proto	= DSA_TAG_PROTO_IPQ40XX,
	.xmit	= ipq40xx_tag_xmit,
	.rcv	= ipq40xx_tag_rcv,
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DSA tag driver for the IPQ40xx SoCs' built-in ethernet switch");
MODULE_AUTHOR("Gabor Juhos <j4g8y7@gmail.com>");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_IPQ40XX);

module_dsa_tag_driver(ipq40xx_tag_dsa_ops);
