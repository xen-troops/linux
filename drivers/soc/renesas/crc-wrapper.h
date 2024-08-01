/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Renesas CRC-WRAPPER drivers header
 *
 * Copyright (C) 2024 Renesas Electronics Inc.
 *
 * Author: huybui2 <huy.bui.wm@renesas.com>
 */

#ifndef _RENESAS_CRC_WRAPPER_H_
#define _RENESAS_CRC_WRAPPER_H_

//Gen4: WCRC_DEVICES = 4
//Gen5: WCRC_DEVICES = 11
#define WCRC_DEVICES 11

/* -----------------------------------------------------------------------------
 * Regrister Definition
 */

#define CRCm (2)
#define KCRCm (3)

/* Address assignment of FIFO */
/* Data */
#define PORT_DATA(mod) (		\
	((CRCm) == (mod))  ? (0x800) :	\
	((KCRCm) == (mod)) ? (0xC00) :	\
	(0x800)				\
)

/* Command */
#define PORT_CMD(mod) (			\
	((CRCm) == (mod))  ? (0x900) :	\
	((KCRCm) == (mod)) ? (0xD00) :	\
	(0x900)				\
)

/* Expected data */
#define PORT_EXPT_DATA(mod) (		\
	((CRCm) == (mod))  ? (0xA00) :	\
	((KCRCm) == (mod)) ? (0xE00) :	\
	(0xA00)				\
)

/* Result */
#define PORT_RES(mod) (			\
	((CRCm) == (mod))  ? (0xB00) :	\
	((KCRCm) == (mod)) ? (0xF00) :	\
	(0xB00)				\
)

/* WCRC register (XXXX: CRCm or KCRCm) */

/* WCRC_XXXX_EN transfer enable register */
#define WCRCm_CRCm_EN 0x0800
#define WCRCm_KCRCm_EN 0x0C00

#define WCRCm_XXXX_EN(mod) (			\
	((CRCm) == (mod))  ? (WCRCm_CRCm_EN)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_EN) : \
	(WCRCm_CRCm_EN)				\
)

#define OUT_EN BIT(16)
#define RES_EN BIT(8)
#define TRANS_EN BIT(1)
#define IN_EN BIT(0)

/* WCRCm_XXXX_STOP transfer stop register */
#define WCRCm_CRCm_STOP 0x0820
#define WCRCm_KCRCm_STOP 0x0C20

#define WCRCm_XXXX_STOP(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_STOP)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_STOP) :	\
	(WCRCm_CRCm_STOP)				\
)

#define STOP BIT(0)

/* WCRCm_XXXX_CMDEN transfer command enable register */
#define WCRCm_CRCm_CMDEN 0x0830
#define WCRCm_KCRCm_CMDEN 0x0C30

#define WCRCm_XXXX_CMDEN(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_CMDEN)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_CMDEN) :	\
	(WCRCm_CRCm_CMDEN)				\
)

#define CMD_EN BIT(0)

/* WCRC_XXXX_COMP compare setting register */
#define WCRCm_CRCm_COMP 0x0840
#define WCRCm_KCRCm_COMP 0x0C40

#define WCRCm_XXXX_COMP(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_COMP)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_COMP) :	\
	(WCRCm_CRCm_COMP)				\
)

#define COMP_FREQ_16 (0 << 16)
#define COMP_FREQ_32 (1 << 16)
#define COMP_FREQ_64 (3 << 16)
#define EXP_REQSEL BIT(1)
#define COMP_EN BIT(0)

/* WCRC_XXXX_COMP_RES compare result register regrister */
#define WCRCm_CRCm_COMP_RES 0x0850
#define WCRCm_KCRCm_COMP_RES 0x0C50

#define WCRCm_XXXX_COMP_RES(mod) (			\
	((CRCm) == (mod))  ? (WCRCm_CRCm_COMP_RES)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_COMP_RES) :	\
	(WCRCm_CRCm_COMP_RES)				\
)

/* WCRC_XXXX_CONV conversion setting register */
#define WCRCm_CRCm_CONV 0x0870
#define WCRCm_KCRCm_CONV 0x0C70

#define WCRCm_XXXX_CONV(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_CONV)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_CONV) :	\
	(WCRCm_CRCm_CONV)				\
)

/* WCRC_XXXX_WAIT wait register */
#define WCRCm_CRCm_WAIT 0x0880
#define WCRCm_KCRCm_WAIT 0x0C80

#define WCRCm_XXXX_WAIT(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_WAIT)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_WAIT) :	\
	(WCRCm_CRCm_WAIT)				\
)

#define WAIT BIT(0)

/* WCRC_XXXX_INIT_CRC initial CRC code register */
#define WCRCm_CRCm_INIT_CRC 0x0910
#define WCRCm_KCRCm_INIT_CRC 0x0D10

#define WCRCm_XXXX_INIT_CRC(mod) (			\
	((CRCm) == (mod))  ? (WCRCm_CRCm_INIT_CRC)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_INIT_CRC) :	\
	(WCRCm_CRCm_INIT_CRC)				\
)

#define INIT_CODE 0xFFFFFFFF

/* WCRC_XXXX_STS status register */
#define WCRCm_CRCm_STS 0x0A00
#define WCRCm_KCRCm_STS 0x0E00

#define WCRCm_XXXX_STS(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_STS)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_STS) :	\
	(WCRCm_CRCm_STS)				\
)

#define STOP_DONE BIT(31)
#define CMD_DONE BIT(24)
#define RES_DONE BIT(20)
#define COMP_ERR BIT(13)
#define COMP_DONE BIT(12)
#define TRANS_DONE BIT(0)

/* WCRC_XXXX_INTEN interrupt enable register */
#define WCRCm_CRCm_INTEN 0x0A40
#define WCRCm_KCRCm_INTEN 0x0E40

#define WCRCm_XXXX_INTEN(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_INTEN)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_INTEN) :	\
	(WCRCm_CRCm_INTEN)				\
)

#define STOP_DONE_IE BIT(31)
#define CMD_DONE_IE BIT(24)
#define RES_DONE_IE BIT(20)
#define COMP_ERR_IE BIT(13)
#define COMP_DONE_IE BIT(12)
#define TRANS_DONE_IE BIT(0)

/* WCRC_XXXX_ECMEN ECM output enable register */
#define WCRCm_CRCm_ECMEN 0x0A80
#define WCRCm_KCRCm_ECMEN 0x0E80

#define WCRCm_XXXX_ECMEN(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_ECMEN)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_ECMEN) :	\
	(WCRCm_CRCm_ECMEN)				\
)

#define COMP_ERR_OE BIT(13)

/* WCRC_XXXX_BUF_STS_RDEN Buffer state read enable register */
#define WCRCm_CRCm_BUF_STS_RDEN 0x0AA0
#define WCRCm_KCRCm_BUF_STS_RDEN 0x0EA0

#define WCRCm_XXXX_BUF_STS_RDEN(mod) (				\
	((CRCm) == (mod))  ? (WCRCm_CRCm_BUF_STS_RDEN)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_BUF_STS_RDEN) :	\
	(WCRCm_CRCm_BUF_STS_RDEN)				\
)

#define CODE_VALUE (0xA5A5 << 16)
#define BUF_STS_RDEN BIT(0)

/* WCRC_XXXX_BUF_STS Buffer state read register */
#define WCRCm_CRCm_BUF_STS 0x0AA4
#define WCRCm_KCRCm_BUF_STS 0x0EA4

#define WCRCm_XXXX_BUF_STS(mod) (			\
	((CRCm) == (mod))  ? (WCRCm_CRCm_BUF_STS)  :	\
	((KCRCm) == (mod)) ? (WCRCm_KCRCm_BUF_STS) :	\
	(WCRCm_CRCm_BUF_STS)				\
)

#define RES_COMP_ENDFLAG BIT(18)
#define BUF_EMPTY BIT(8)

/* WCRC common */

/* WCRCm common status register */
#define WCRCm_COMMON_STS 0x0F00
#define EDC_ERR BIT(16)

/* WCRCm common interrupt enable register */
#define WCRCm_INTEN 0x0F00
#define EDC_ERR_IE BIT(16)

/* WCRCm common ECM output enable register */
#define WCRCm_COMMON_ECMEN 0x0F80
#define EDC_ERR_OE BIT(16)

/* WCRCm error injection register */
#define WCRCm_ERRINJ 0x0FC0
#define CODE (0xA5A5 << 16)

#endif /* _RENESAS_CRC_WRAPPER_H_ */
