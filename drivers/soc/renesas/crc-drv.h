/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Renesas CRC drivers header;
 *
 * Copyright (C) 2024 by Renesas Electronics Corporation
 *
 * Redistribution of this file is permitted under
 * the terms of the GNU Public License (GPL)
 */

#ifndef _RENESAS_CRC_H_
#define _RENESAS_CRC_H_

int crc_calculate(struct wcrc_info *info);

/* -----------------------------------------------------------------------------
 * Regrister Definition
 */

/* CRC[m] Input register */
#define DCRAmCIN 0x0000

/* CRC[m] Data register */
#define DCRAmCOUT 0x0004

#define DCRAmCOUT_DEFAULT 0xFFFFFFFF //default; value for CRC calculation
//initial value of each polynimial: CRC calulation method; polynomial
#define DCRAmCOUT_32_ETHERNET 0xFFFFFFFF //default; CRC-32-IEEE 802.3; 04C11DB7
#define DCRAmCOUT_16_CCITT_FALSE_CRC16 0xFFFF //CCITT_FALSE_CRC16; 1021
#define DCRAmCOUT_8_SAE_J1850 0xFF //SAE_J1850; 1D
#define DCRAmCOUT_8_0x2F 0xFF // 0x2F polynomial
#define DCRAmCOUT_32_0xF4ACFB13 0xFFFFFFFF //0xF4ACFB13 polynomial
#define DCRAmCOUT_32_0x1EDC6F41 0xFFFFFFFF //0x1EDC6F41 polynomial CRC-32 (Castagnoli)
#define DCRAmCOUT_21_0x102899 0x1FFFFF //0x102899 polynomial CRC-21
#define DCRAmCOUT_17_0x1685B 0x1FFFF //0x1685B polynomial CRC-17
#define DCRAmCOUT_15_0x4599 0x7FFF //0x4599 polynomial CRC-15

/* CRC[m] Control register */
#define DCRAmCTL 0x0020

#define DCRAmCTL_ISZ_32 0 //default 32bit width DCRAmCIN_31:0
#define DCRAmCTL_ISZ_16 BIT(4) //16bit width DCRAmCIN_15:0
#define DCRAmCTL_ISZ_8 BIT(5) //8bit width DCRAmCIN_7:0

#define DCRAmCTL_POL_32_ETHERNET 0 //default CRC-32-IEEE 802.3
#define DCRAmCTL_POL_16_CCITT_FALSE_CRC16 BIT(0) //CCITT_FALSE_CRC16
#define DCRAmCTL_POL_8_SAE_J1850 BIT(1) //SAE_J1850
#define DCRAmCTL_POL_8_0x2F (3 << 0) // 0x2F polynomial
#define DCRAmCTL_POL_32_0xF4ACFB13 BIT(2) //0xF4ACFB13 polynomial
#define DCRAmCTL_POL_32_0x1EDC6F41 (5 << 0) //0x1EDC6F41 polynomial CRC-32 (Castagnoli)
#define DCRAmCTL_POL_21_0x102899 (6 << 0) //0x102899 polynomial CRC-21
#define DCRAmCTL_POL_17_0x1685B (7 << 0) //0x1685B polynomial CRC-17
#define DCRAmCTL_POL_15_0x4599 BIT(3) //0x4599 polynomial CRC-15

/* CRC[m] Control register 2 */
#define DCRAmCTL2 0x0040

#define DCRAmCTL2_xorvalmode BIT(7) //EXOR ON of output data

#define DCRAmCTL2_bitswapmode BIT(6) //bit swap of output data

#define DCRAmCTL2_byteswapmode_00 0 //default no byte swap of output data
#define DCRAmCTL2_byteswapmode_01 BIT(4)
#define DCRAmCTL2_byteswapmode_10 BIT(5)
#define DCRAmCTL2_byteswapmode_11 (3 << 3)

#define DCRAmCTL2_xorvalinmode BIT(3) //EXOR ON of input data

#define DCRAmCTL2_bitswapinmode BIT(2) //bit swap of input data

#define DCRAmCTL2_byteswapinmode_00 0 //default no byte swap of input data
#define DCRAmCTL2_byteswapinmode_01 BIT(0)
#define DCRAmCTL2_byteswapinmode_10 BIT(1)
#define DCRAmCTL2_byteswapinmode_11 (3 << 0)

#endif /* _RENESAS_CRC_H_ */
