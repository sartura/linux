// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2022, Maxime Chevallier <maxime.chevallier@bootlin.com> */

#include <linux/bitfield.h>
#include <linux/dsa/oob.h>
#include <linux/skbuff.h>

#include "dsa_priv.h"

#define DSA_OOB_TAG_LEN 4

int dsa_oob_tag_push(struct sk_buff *skb, struct dsa_oob_tag_info *ti)
{
	struct dsa_oob_tag_info *tag_info;

	tag_info = skb_ext_add(skb, SKB_EXT_DSA_OOB);

	tag_info->dp = ti->dp;

	return 0;
}
EXPORT_SYMBOL(dsa_oob_tag_push);

int dsa_oob_tag_pop(struct sk_buff *skb, struct dsa_oob_tag_info *ti)
{
	struct dsa_oob_tag_info *tag_info;

	tag_info = skb_ext_find(skb, SKB_EXT_DSA_OOB);
	if (!tag_info)
		return -EINVAL;

	ti->dp = tag_info->dp;

	return 0;
}
EXPORT_SYMBOL(dsa_oob_tag_pop);

static struct sk_buff *oob_tag_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	struct dsa_oob_tag_info tag_info;

	tag_info.dp = dp->index;

	if (dsa_oob_tag_push(skb, &tag_info))
		return NULL;

	return skb;
}

static struct sk_buff *oob_tag_rcv(struct sk_buff *skb,
				   struct net_device *dev)
{
	struct dsa_oob_tag_info tag_info;

	if (dsa_oob_tag_pop(skb, &tag_info))
		return NULL;

	skb->dev = dsa_master_find_slave(dev, 0, tag_info.dp);
	if (!skb->dev)
		return NULL;

	return skb;
}

const struct dsa_device_ops oob_tag_dsa_ops = {
	.name	= "oob",
	.proto	= DSA_TAG_PROTO_OOB,
	.xmit	= oob_tag_xmit,
	.rcv	= oob_tag_rcv,
	.needed_headroom = DSA_OOB_TAG_LEN,
};

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DSA tag driver for out-of-band tagging");
MODULE_AUTHOR("Maxime Chevallier <maxime.chevallier@bootlin.com>");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_OOB);

module_dsa_tag_driver(oob_tag_dsa_ops);
