/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef _DT_BINDINGS_CLK_LAN969X_H
#define _DT_BINDINGS_CLK_LAN969X_H

#define GCK_ID_QSPI0		0
#define GCK_ID_QSPI2		1
#define GCK_ID_SDMMC0		2
#define GCK_ID_SDMMC1		3
#define GCK_ID_MCAN0		4
#define GCK_ID_MCAN1		5
#define GCK_ID_FLEXCOM0		6
#define GCK_ID_FLEXCOM1		7
#define GCK_ID_FLEXCOM2		8
#define GCK_ID_FLEXCOM3		9
#define GCK_ID_TIMER		10
#define GCK_ID_USB_REFCLK	11

/* Gate clocks */
#define GCK_GATE_USB_DRD	12
#define GCK_GATE_MCRAMC		13
#define GCK_GATE_HMATRIX	14

#endif
