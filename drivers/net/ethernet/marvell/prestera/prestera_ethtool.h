/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/* Copyright (c) 2019-2021 Marvell International Ltd. All rights reserved. */

#ifndef __PRESTERA_ETHTOOL_H_
#define __PRESTERA_ETHTOOL_H_

#include <linux/ethtool.h>

#include "prestera.h"

extern const struct ethtool_ops prestera_ethtool_ops;

void prestera_ethtool_port_state_changed(struct prestera_port *port,
					 struct prestera_port_event *evt);

#endif /* _PRESTERA_ETHTOOL_H_ */
