/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Renesas CRC-WRAPPER drivers header;
 *
 * Copyright (C) 2024 by Renesas Electronics Corporation
 *
 * Redistribution of this file is permitted under
 * the terms of the GNU Public License (GPL)
 */

#ifndef _RENESAS_CRC_WRAPPER_H_
#define _RENESAS_CRC_WRAPPER_H_

/* WCRC modes */
#define MM_IOC_MAGIC 'o'
#define INDEPENDENT_CRC_MODE _IOWR(MM_IOC_MAGIC, 0, struct wcrc_info)
#define E2E_CRC_MODE _IOWR(MM_IOC_MAGIC, 1, struct wcrc_info)
#define DATA_THROUGH_MODE _IOWR(MM_IOC_MAGIC, 2, struct wcrc_info)
#define E2E_CRC_DATA_THROUGH_MODE _IOWR(MM_IOC_MAGIC, 3, struct wcrc_info)

/* Input, Output modes */
#define MM_IOC_MAGIC_2 'h'
#define SET_INPUT_LEN	_IOWR(MM_IOC_MAGIC_2, 0, uint32_t)
#define SET_INPUT_DATA	_IOWR(MM_IOC_MAGIC_2, 1, uint32_t)
#define GET_OUTPUT		_IOWR(MM_IOC_MAGIC_2, 2, uint32_t)

/* Polynomial modes */
#define POLY_32_ETHERNET _IOWR(MM_IOC_MAGIC, 4, struct wcrc_info)
#define POLY_16_CCITT_FALSE_CRC16 _IOWR(MM_IOC_MAGIC, 5, struct wcrc_info)
#define POLY_8_SAE_J1850 _IOWR(MM_IOC_MAGIC, 6, struct wcrc_info)
#define POLY_8_0x2F _IOWR(MM_IOC_MAGIC, 7, struct wcrc_info)
#define POLY_32_0xF4ACFB13 _IOWR(MM_IOC_MAGIC, 8, struct wcrc_info)
#define POLY_32_0x1EDC6F41 _IOWR(MM_IOC_MAGIC, 9, struct wcrc_info) //Castagnoli
#define POLY_21_0x102899 _IOWR(MM_IOC_MAGIC, 10, struct wcrc_info)
#define POLY_17_0x1685B _IOWR(MM_IOC_MAGIC, 11, struct wcrc_info)
#define POLY_15_0x4599 _IOWR(MM_IOC_MAGIC, 12, struct wcrc_info)

struct wcrc_info {
    //Input data and output crc data
    uint32_t data_input;
    uint32_t crc_data_out;
    uint32_t kcrc_data_out;

    // Size of data input
    unsigned int d_in_sz;

    // Calculation continuous check
    bool conti_cal;
    bool during_conti_cal;
    bool skip_data_in;

    //Selecting WCRC/CRC/KCRC/FIFO channels
    unsigned int wcrc_unit;
    unsigned int crc_unit;
    unsigned int kcrc_unit;

    //CRC addiotional features
    unsigned int poly_mode;
    unsigned int in_exor_on;
    unsigned int out_exor_on;
    unsigned int in_bit_swap;
    unsigned int out_bit_swap;
    unsigned int in_byte_swap;
    unsigned int out_byte_swap;

    //KCRC addiotional features
    unsigned int kcrc_cmd0;
    unsigned int kcrc_cmd1;
    unsigned int kcrc_cmd2;

    //Choosing CRC = 0, or KCRC = 1 mode
    bool crc_opt;	

    //For E2E CRC mode
    //WCRCm_CRCm_EN
	unsigned int conv_size;
	//WCRCm_CRCm_INIT_CRC
	unsigned int init_crc_code;
	//WCRCm_CRCm_EN
	bool out_en;
	bool res_en;
	bool trans_en;
	bool in_en;
	
	//WCRCm_CRCm_CMDEN
	bool cmd_en;

};

/* -----------------------------------------------------------------------------
 * Regrister Definition
 */

#define CRCm        (2)
#define KCRCm       (3)

/* Address assignment of FIFO */
/* Data */
#define PORT_DATA(mod)				            \
		((CRCm)      == (mod))	? 	(0x800) :	\
		((KCRCm)     == (mod))	? 	(0xC00) :	\
		(0x800)

/* Command */
#define PORT_CMD(mod)				            \
		((CRCm)      == (mod))	? 	(0x900) :	\
		((KCRCm)     == (mod))	? 	(0xD00) :	\
		(0x900)

/* Expected data */
#define PORT_EXPT_DATA(mod)				        \
		((CRCm)      == (mod))	? 	(0xA00) :	\
		((KCRCm)     == (mod))	? 	(0xE00) :	\
		(0xA00)

/* Result */
#define PORT_RES(mod)				            \
		((CRCm)      == (mod))	? 	(0xB00) :	\
		((KCRCm)     == (mod))	? 	(0xF00) :	\
		(0xB00)

// Size each port is 256 bytes
#define PORT_SIZE	(256)

/* WCRC for CRC */

/* WCRC[m] CRC[m]
transfer enable register register */
#define WCRCm_CRCm_EN 0x0800

#define CRCm_EN_OUT_EN BIT(16)
#define CRCm_EN_RES_EN BIT(8)
#define CRCm_EN_TRANS_EN BIT(1)
#define CRCm_EN_IN_EN BIT(0)

/* WCRC[m] CRC[m]
transfer stop register register */
#define WCRCm_CRCm_STOP 0x0820

#define CRCm_STOP BIT(0)

/* WCRC[m] CRC[m]
transfer command enable register register */
#define WCRCm_CRCm_CMDEN 0x0830

#define CRCm_CMDEN_CMD_EN BIT(0)

/* WCRC[m] CRC[m]
compare setting register register */
#define WCRCm_CRCm_COMP 0x0840

#define CRCm_COMP_COMP_FREQ_16 0 //default; compare every 16Byte
#define CRCm_COMP_COMP_FREQ_32 BIT(16) //compare every 32Byte
#define CRCm_COMP_COMP_FREQ_64 (3 << 16) //compare every 64Byte
#define CRCm_COMP_EXP_REQSEL BIT(1)
#define CRCm_COMP_COMP_EN BIT(0)

/* WCRC[m] CRC[m]
compare result register regrister */
#define WCRCm_CRCm_COMP_RES 0x0850

/* WCRC[m] CRC[m]
conversion setting register register */
#define WCRCm_CRCm_CONV 0x0870

/* WCRC[m] CRC[m]
wait register register */
#define WCRCm_CRCm_WAIT 0x0880

#define CRCm_WAIT BIT(0)

/* WCRC[m] CRC[m]
initial CRC code register register */
#define WCRCm_CRCm_INIT_CRC 0x0910

#define CRCm_INIT_CRC_INIT_CODE 0xFFFFFFFF

/* WCRC[m] CRC[m]
status register register */
#define WCRCm_CRCm_STS 0x0A00

#define CRCm_STS_STOP_DONE BIT(31)
#define CRCm_STS_CMD_DONE BIT(24)
#define CRCm_STS_RES_DONE BIT(20)
#define CRCm_STS_COMP_ERR BIT(13)
#define CRCm_STS_COMP_DONE BIT(12)
#define CRCm_STS_TRANS_DONE BIT(0)

/* WCRC[m] CRC[m]
interrupt enable register register */
#define WCRCm_CRCm_INTEN 0x0A40

#define CRCm_INTEN_STOP_DONE_IE BIT(31)
#define CRCm_INTEN_CMD_DONE_IE BIT(24)
#define CRCm_INTEN_RES_DONE_IE BIT(20)
#define CRCm_INTEN_COMP_ERR_IE BIT(13)
#define CRCm_INTEN_COMP_DONE_IE BIT(12)
#define CRCm_INTEN_TRANS_DONE_IE BIT(0)

/* WCRC[m] CRC[m]
ECM output enable register register */
#define WCRCm_CRCm_ECMEM 0x0A80

#define CRCm_ECMEM_COMP_ERR_OE BIT(13)

/* WCRC[m] CRC[m]
Buffer state read enable register register */
#define WCRCm_CRCm_BUF_STS_RDEN 0x0AA0

#define CRC_BUF_STS_RDEN_CODE_VALUE 0xA5A5
#define CRC_BUF_STS_RDEN BIT(0)

/* WCRC[m] CRC[m]
Buffer state read register register */
#define WCRCm_CRCm_BUF_STS 0x0AA4

#define CRC_BUF_STS_RES_COMP_ENDFLAG BIT(18)
#define CRC_BUF_EMPTY BIT(8)

/* WCRC for KCRC */

/* WCRC[m] KCRC[m]
transfer enable register register */
#define WCRCm_KCRCm_EN 0x0C00

#define KCRCm_EN_OUT_EN BIT(16)
#define KCRCm_EN_RES_EN BIT(8)
#define KCRCm_EN_TRANS_EN BIT(1)
#define KCRCm_EN_IN_EN BIT(0)

/* WCRC[m] KCRC[m]
transfer stop register register */
#define WCRCm_KCRCm_STOP 0x0C20

#define KCRCm_STOP BIT(0)

/* WCRC[m] KCRC[m]
transfer command enable register register */
#define WCRCm_KCRCm_CMDEN 0x0C30

#define KCRCm_CMDEN_CMD_EN BIT(0)

/* WCRC[m] KCRC[m]
compare setting register register */
#define WCRCm_KCRCm_COMP 0x0C40

#define KCRCm_COMP_COMP_FREQ_16 0 //default; compare every 16Byte
#define KCRCm_COMP_COMP_FREQ_32 BIT(16) //compare every 32Byte
#define KCRCm_COMP_COMP_FREQ_64 (3 << 16) //compare every 64Byte
#define KCRCm_COMP_EXP_REQSEL BIT(1)
#define KCRCm_COMP_COMP_EN BIT(0)

/* WCRC[m] KCRC[m]
compare result register register */
#define WCRCm_KCRCm_COMP_RES 0x0C50

/* WCRC[m] KCRC[m]
conversion setting register register */
#define WCRCm_KCRCm_CONV 0x0C70

/* WCRC[m] KCRC[m]
wait register register */
#define WCRCm_KCRCm_WAIT 0x0C80

#define KCRCm_WAIT BIT(0)

/* WCRC[m] KCRC[m]
initial CRC code register register */
#define WCRCm_KCRCm_INIT_CRC 0x0D10

#define KCRCm_INIT_CRC_INIT_CODE 0xFFFFFFFF

/* WCRC[m] KCRC[m]
status register register */
#define WCRCm_KCRCm_STS 0x0E00

#define KCRCm_STS_STOP_DONE BIT(31)
#define KCRCm_STS_CMD_DONE BIT(24)
#define KCRCm_STS_RES_DONE BIT(20)
#define KCRCm_STS_COMP_ERR BIT(13)
#define KCRCm_STS_COMP_DONE BIT(12)
#define KCRCm_STS_TRANS_DONE BIT(0)

/* WCRC[m] KCRC[m]
interrupt enable register register */
#define WCRCm_KCRCm_INT 0x0E40

#define KCRCm_INTEN_STOP_DONE_IE BIT(31)
#define KCRCm_INTEN_CMD_DONE_IE BIT(24)
#define KCRCm_INTEN_RES_DONE_IE BIT(20)
#define KCRCm_INTEN_COMP_ERR_IE BIT(13)
#define KCRCm_INTEN_COMP_DONE_IE BIT(12)
#define KCRCm_INTEN_TRANS_DONE_IE BIT(0)

/* WCRC[m] KCRC[m]
ECM output enable register register */
#define WCRCm_KCRCm_ECMEN 0x0E80

#define KCRCm_ECMEM_COMP_ERR_OE BIT(13)

/* WCRC[m] KCRC[m]
Buffer state read enable register register */
#define WCRCm_KCRCm_BUF_STS_RDEN 0x0EA0

#define KCRC_BUF_STS_RDEN_CODE_VALUE 0xA5A5
#define KCRC_BUF_STS_RDEN BIT(0)

/* WCRC[m] KCRC[m]
Buffer state read register register */
#define WCRCm_KCRCm_BUF_STS 0x0EA4

#define KCRC_BUF_STS_RES_COMP_ENDFLAG BIT(18)
#define KCRC_BUF_EMPTY BIT(8)

/* WCRC common */

/* WCRC[m] common
status register register */
#define WCRCm_COMMON_STS 0x0F00

/* WCRC[m] common
interrupt enable register register */
#define WCRCm_COMMON_STS 0x0F00

/* WCRC[m] common
ECM output enable register register */
#define WCRCm_COMMON_ECMEN 0x0F80

#define WCRC_EDC_ERR_OE BIT(16)

/* WCRC[m]
error injection register register */
#define WCRCm_ERRINJ 0x0FC0

#endif /* _RENESAS_CRC_WRAPPER_H_ */
