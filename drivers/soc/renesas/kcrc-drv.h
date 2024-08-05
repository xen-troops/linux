/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Renesas KCRC drivers header
 *
 * Copyright (C) 2024 Renesas Electronics Inc.
 *
 * Author: huybui2 <huy.bui.wm@renesas.com>
 */

#ifndef _RENESAS_KCRC_H_
#define _RENESAS_KCRC_H_

int kcrc_calculate(struct wcrc_info *info);
void kcrc_setting(struct wcrc_info *info);
void kcrc_return_reg(struct wcrc_info *info);

/* -----------------------------------------------------------------------------
 * Regrister Definition
 */

/* KCRC[m] data input register */
#define KCRCmDIN 0x0000

/* KCRC[m] data output register */
#define KCRCmDOUT 0x0080

#define KCRCmDOUT_INITIAL 0x0 //initialize value

/* KCRC[m] control register */
#define KCRCmCTL 0x0090

#define KCRCmCTL_PSIZE_32 (31 << 16) //default 32-bit
#define KCRCmCTL_PSIZE_16 (15 << 16) //16-bit
#define KCRCmCTL_PSIZE_8 (7 << 16) //8-bit

#define KCRCmCTL_CMD0 BIT(8) //0: Mode N (Normal), 1: Mode R (output reflect)
#define KCRCmCTL_CMD1 BIT(5) //0: Mode N (Normal), 1: Mode R (input reflect)
#define KCRCmCTL_CMD2 BIT(4) //0: Mode M (MSB shift), 1: Mode R (LSB shift)

#define KCRCmCTL_DW_32 0 //default 32-bit fix mode
#define KCRCmCTL_DW_16 BIT(0) //16-bit fix mode
#define KCRCmCTL_DW_8 (3 << 0) //8-bit fix mode

/* KCRC[m] Polynomial register */
#define KCRCmPOLY 0x00A0

#define KCRCmPOLY_32_ETHERNET 0x04C11DB7 //default 32-bit Ethernet CRC
#define KCRCmPOLY_16_CCITT 0x1021 //16-bit CCITT CRC
#define KCRCmPOLY_8_SAE_J1850 0x1D //8-bit SAE J1850 CRC
#define KCRCmPOLY_8_0x2F 0x2F //8-bit 0x2F CRC
#define KCRCmPOLY_32_CRC32C 0x1EDC6F41 //32-bit CRC32C (Castagnoli)

/* KCRC[m] XOR mask register */
#define KCRCmXOR 0x00B0

#define KCRCmXOR_XOR 0xFFFFFFFF //default value

#endif /* _RENESAS_KCRC_H_ */
