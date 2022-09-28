// SPDX-License-Identifier: GPL-2.0
/* Renesas Ethernet Switch device driver
 *
 * Copyright (C) 2020 Renesas Electronics Corporation
 */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/ip.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/net_tstamp.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/reset.h>
#include <linux/inetdevice.h>
#include <net/rtnetlink.h>
#include <net/nexthop.h>
#include <net/netns/generic.h>
#include <net/arp.h>

#include "rtsn_ptp.h"
#include "rswitch.h"
#include "rswitch_tc_filters.h"

static void *debug_addr;

#define RSWITCH_NUM_HW		5

#define RSWITCH_GWCA_IDX_TO_HW_NUM(i)	((i) + RSWITCH_MAX_NUM_ETHA)
#define RSWITCH_HW_NUM_TO_GWCA_IDX(i)	((i) - RSWITCH_MAX_NUM_ETHA)

#define RSWITCH_COMA_OFFSET	0x00009000
#define RSWITCH_ETHA_OFFSET	0x0000a000	/* with RMAC */
#define RSWITCH_ETHA_SIZE	0x00002000	/* with RMAC */
#define RSWITCH_GWCA0_OFFSET	0x00010000
#define RSWITCH_GWCA1_OFFSET	0x00012000
#define RSWITCH_GPTP_OFFSET    0x00018000

#define FWRO	0
#define CARO	RSWITCH_COMA_OFFSET
#define GWRO	RSWITCH_GWCA1_OFFSET
/*#define GWRO	RSWITCH_GWCA0_OFFSET*/
#define TARO	0
#define RMRO	0x1000
enum rswitch_reg {
	FWGC		= FWRO + 0x0000,
	FWTTC0		= FWRO + 0x0010,
	FWTTC1		= FWRO + 0x0014,
	FWLBMC		= FWRO + 0x0018,
	FWCEPTC		= FWRO + 0x020,
	FWCEPRC0	= FWRO + 0x024,
	FWCEPRC1	= FWRO + 0x028,
	FWCEPRC2	= FWRO + 0x02C,
	FWCLPTC		= FWRO + 0x030,
	FWCLPRC		= FWRO + 0x034,
	FWCMPTC		= FWRO + 0x040,
	FWEMPTC		= FWRO + 0x044,
	FWSDMPTC	= FWRO + 0x050,
	FWSDMPVC	= FWRO + 0x054,
	FWLBWMC0	= FWRO + 0x080,
	FWPC00		= FWRO + 0x100,
	FWPC10		= FWRO + 0x104,
	FWPC20		= FWRO + 0x108,
	FWCTGC00	= FWRO + 0x400,
	FWCTGC10	= FWRO + 0x404,
	FWCTTC00	= FWRO + 0x408,
	FWCTTC10	= FWRO + 0x40C,
	FWCTTC200	= FWRO + 0x410,
	FWCTSC00	= FWRO + 0x420,
	FWCTSC10	= FWRO + 0x424,
	FWCTSC20	= FWRO + 0x428,
	FWCTSC30	= FWRO + 0x42C,
	FWCTSC40	= FWRO + 0x430,
	FWTWBFC0	= FWRO + 0x1000,
	FWTWBFVC0	= FWRO + 0x1004,
	FWTHBFC0	= FWRO + 0x1400,
	FWTHBFV0C0	= FWRO + 0x1404,
	FWTHBFV1C0	= FWRO + 0x1408,
	FWFOBFC0	= FWRO + 0x1800,
	FWFOBFV0C0	= FWRO + 0x1804,
	FWFOBFV1C0	= FWRO + 0x1808,
	FWRFC0		= FWRO + 0x1C00,
	FWRFVC0		= FWRO + 0x1C04,
	FWCFC0		= FWRO + 0x2000,
	FWCFMC00	= FWRO + 0x2004,
	FWIP4SC		= FWRO + 0x4008,
	FWIP6SC		= FWRO + 0x4018,
	FWIP6OC		= FWRO + 0x401C,
	FWL2SC		= FWRO + 0x4020,
	FWSFHEC		= FWRO + 0x4030,
	FWSHCR0		= FWRO + 0x4040,
	FWSHCR1		= FWRO + 0x4044,
	FWSHCR2		= FWRO + 0x4048,
	FWSHCR3		= FWRO + 0x404C,
	FWSHCR4		= FWRO + 0x4050,
	FWSHCR5		= FWRO + 0x4054,
	FWSHCR6		= FWRO + 0x4058,
	FWSHCR7		= FWRO + 0x405C,
	FWSHCR8		= FWRO + 0x4060,
	FWSHCR9		= FWRO + 0x4064,
	FWSHCR10	= FWRO + 0x4068,
	FWSHCR11	= FWRO + 0x406C,
	FWSHCR12	= FWRO + 0x4070,
	FWSHCR13	= FWRO + 0x4074,
	FWSHCRR		= FWRO + 0x4078,
	FWLTHHEC	= FWRO + 0x4090,
	FWLTHHC		= FWRO + 0x4094,
	FWLTHTL0	= FWRO + 0x40A0,
	FWLTHTL1	= FWRO + 0x40A4,
	FWLTHTL2	= FWRO + 0x40A8,
	FWLTHTL3	= FWRO + 0x40AC,
	FWLTHTL4	= FWRO + 0x40B0,
	FWLTHTL5	= FWRO + 0x40B4,
	FWLTHTL6	= FWRO + 0x40B8,
	FWLTHTL7	= FWRO + 0x40BC,
	FWLTHTL80	= FWRO + 0x40C0,
	FWLTHTL9	= FWRO + 0x40D0,
	FWLTHTLR	= FWRO + 0x40D4,
	FWLTHTIM	= FWRO + 0x40E0,
	FWLTHTEM	= FWRO + 0x40E4,
	FWLTHTS0	= FWRO + 0x4100,
	FWLTHTS1	= FWRO + 0x4104,
	FWLTHTS2	= FWRO + 0x4108,
	FWLTHTS3	= FWRO + 0x410C,
	FWLTHTS4	= FWRO + 0x4110,
	FWLTHTSR0	= FWRO + 0x4120,
	FWLTHTSR1	= FWRO + 0x4124,
	FWLTHTSR2	= FWRO + 0x4128,
	FWLTHTSR3	= FWRO + 0x412C,
	FWLTHTSR40	= FWRO + 0x4130,
	FWLTHTSR5	= FWRO + 0x4140,
	FWLTHTR		= FWRO + 0x4150,
	FWLTHTRR0	= FWRO + 0x4154,
	FWLTHTRR1	= FWRO + 0x4158,
	FWLTHTRR2	= FWRO + 0x415C,
	FWLTHTRR3	= FWRO + 0x4160,
	FWLTHTRR4	= FWRO + 0x4164,
	FWLTHTRR5	= FWRO + 0x4168,
	FWLTHTRR6	= FWRO + 0x416C,
	FWLTHTRR7	= FWRO + 0x4170,
	FWLTHTRR8	= FWRO + 0x4174,
	FWLTHTRR9	= FWRO + 0x4180,
	FWLTHTRR10	= FWRO + 0x4190,
	FWIPHEC		= FWRO + 0x4214,
	FWIPHC		= FWRO + 0x4218,
	FWIPTL0		= FWRO + 0x4220,
	FWIPTL1		= FWRO + 0x4224,
	FWIPTL2		= FWRO + 0x4228,
	FWIPTL3		= FWRO + 0x422C,
	FWIPTL4		= FWRO + 0x4230,
	FWIPTL5		= FWRO + 0x4234,
	FWIPTL6		= FWRO + 0x4238,
	FWIPTL7		= FWRO + 0x4240,
	FWIPTL8		= FWRO + 0x4250,
	FWIPTLR		= FWRO + 0x4254,
	FWIPTIM		= FWRO + 0x4260,
	FWIPTEM		= FWRO + 0x4264,
	FWIPTS0		= FWRO + 0x4270,
	FWIPTS1		= FWRO + 0x4274,
	FWIPTS2		= FWRO + 0x4278,
	FWIPTS3		= FWRO + 0x427C,
	FWIPTS4		= FWRO + 0x4280,
	FWIPTSR0	= FWRO + 0x4284,
	FWIPTSR1	= FWRO + 0x4288,
	FWIPTSR2	= FWRO + 0x428C,
	FWIPTSR3	= FWRO + 0x4290,
	FWIPTSR4	= FWRO + 0x42A0,
	FWIPTR		= FWRO + 0x42B0,
	FWIPTRR0	= FWRO + 0x42B4,
	FWIPTRR1	= FWRO + 0x42B8,
	FWIPTRR2	= FWRO + 0x42BC,
	FWIPTRR3	= FWRO + 0x42C0,
	FWIPTRR4	= FWRO + 0x42C4,
	FWIPTRR5	= FWRO + 0x42C8,
	FWIPTRR6	= FWRO + 0x42CC,
	FWIPTRR7	= FWRO + 0x42D0,
	FWIPTRR8	= FWRO + 0x42E0,
	FWIPTRR9	= FWRO + 0x42F0,
	FWIPHLEC	= FWRO + 0x4300,
	FWIPAGUSPC	= FWRO + 0x4500,
	FWIPAGC		= FWRO + 0x4504,
	FWIPAGM0	= FWRO + 0x4510,
	FWIPAGM1	= FWRO + 0x4514,
	FWIPAGM2	= FWRO + 0x4518,
	FWIPAGM3	= FWRO + 0x451C,
	FWIPAGM4	= FWRO + 0x4520,
	FWMACHEC	= FWRO + 0x4620,
	FWMACHC		= FWRO + 0x4624,
	FWMACTL0	= FWRO + 0x4630,
	FWMACTL1	= FWRO + 0x4634,
	FWMACTL2	= FWRO + 0x4638,
	FWMACTL3	= FWRO + 0x463C,
	FWMACTL4	= FWRO + 0x4640,
	FWMACTL5	= FWRO + 0x4650,
	FWMACTLR	= FWRO + 0x4654,
	FWMACTIM	= FWRO + 0x4660,
	FWMACTEM	= FWRO + 0x4664,
	FWMACTS0	= FWRO + 0x4670,
	FWMACTS1	= FWRO + 0x4674,
	FWMACTSR0	= FWRO + 0x4678,
	FWMACTSR1	= FWRO + 0x467C,
	FWMACTSR2	= FWRO + 0x4680,
	FWMACTSR3	= FWRO + 0x4690,
	FWMACTR		= FWRO + 0x46A0,
	FWMACTRR0	= FWRO + 0x46A4,
	FWMACTRR1	= FWRO + 0x46A8,
	FWMACTRR2	= FWRO + 0x46AC,
	FWMACTRR3	= FWRO + 0x46B0,
	FWMACTRR4	= FWRO + 0x46B4,
	FWMACTRR5	= FWRO + 0x46C0,
	FWMACTRR6	= FWRO + 0x46D0,
	FWMACHLEC	= FWRO + 0x4700,
	FWMACAGUSPC	= FWRO + 0x4880,
	FWMACAGC	= FWRO + 0x4884,
	FWMACAGM0	= FWRO + 0x4888,
	FWMACAGM1	= FWRO + 0x488C,
	FWVLANTEC	= FWRO + 0x4900,
	FWVLANTL0	= FWRO + 0x4910,
	FWVLANTL1	= FWRO + 0x4914,
	FWVLANTL2	= FWRO + 0x4918,
	FWVLANTL3	= FWRO + 0x4920,
	FWVLANTL4	= FWRO + 0x4930,
	FWVLANTLR	= FWRO + 0x4934,
	FWVLANTIM	= FWRO + 0x4940,
	FWVLANTEM	= FWRO + 0x4944,
	FWVLANTS	= FWRO + 0x4950,
	FWVLANTSR0	= FWRO + 0x4954,
	FWVLANTSR1	= FWRO + 0x4958,
	FWVLANTSR2	= FWRO + 0x4960,
	FWVLANTSR3	= FWRO + 0x4970,
	FWPBFCi		= FWRO + 0x4A00,
	FWPBFCSDC00	= FWRO + 0x4A04,
	FWL23URL0	= FWRO + 0x4E00,
	FWL23URL1	= FWRO + 0x4E04,
	FWL23URL2	= FWRO + 0x4E08,
	FWL23URL3	= FWRO + 0x4E0C,
	FWL23URLR	= FWRO + 0x4E10,
	FWL23UTIM	= FWRO + 0x4E20,
	FWL23URR	= FWRO + 0x4E30,
	FWL23URRR0	= FWRO + 0x4E34,
	FWL23URRR1	= FWRO + 0x4E38,
	FWL23URRR2	= FWRO + 0x4E3C,
	FWL23URRR3	= FWRO + 0x4E40,
	FWL23URMC0	= FWRO + 0x4F00,
	FWPMFGC0	= FWRO + 0x5000,
	FWPGFC0		= FWRO + 0x5100,
	FWPGFIGSC0	= FWRO + 0x5104,
	FWPGFENC0	= FWRO + 0x5108,
	FWPGFENM0	= FWRO + 0x510c,
	FWPGFCSTC00	= FWRO + 0x5110,
	FWPGFCSTC10	= FWRO + 0x5114,
	FWPGFCSTM00	= FWRO + 0x5118,
	FWPGFCSTM10	= FWRO + 0x511C,
	FWPGFCTC0	= FWRO + 0x5120,
	FWPGFCTM0	= FWRO + 0x5124,
	FWPGFHCC0	= FWRO + 0x5128,
	FWPGFSM0	= FWRO + 0x512C,
	FWPGFGC0	= FWRO + 0x5130,
	FWPGFGL0	= FWRO + 0x5500,
	FWPGFGL1	= FWRO + 0x5504,
	FWPGFGLR	= FWRO + 0x5518,
	FWPGFGR		= FWRO + 0x5510,
	FWPGFGRR0	= FWRO + 0x5514,
	FWPGFGRR1	= FWRO + 0x5518,
	FWPGFRIM	= FWRO + 0x5520,
	FWPMTRFC0	= FWRO + 0x5600,
	FWPMTRCBSC0	= FWRO + 0x5604,
	FWPMTRC0RC0	= FWRO + 0x5608,
	FWPMTREBSC0	= FWRO + 0x560C,
	FWPMTREIRC0	= FWRO + 0x5610,
	FWPMTRFM0	= FWRO + 0x5614,
	FWFTL0		= FWRO + 0x6000,
	FWFTL1		= FWRO + 0x6004,
	FWFTLR		= FWRO + 0x6008,
	FWFTOC		= FWRO + 0x6010,
	FWFTOPC		= FWRO + 0x6014,
	FWFTIM		= FWRO + 0x6020,
	FWFTR		= FWRO + 0x6030,
	FWFTRR0		= FWRO + 0x6034,
	FWFTRR1		= FWRO + 0x6038,
	FWFTRR2		= FWRO + 0x603C,
	FWSEQNGC0	= FWRO + 0x6100,
	FWSEQNGM0	= FWRO + 0x6104,
	FWSEQNRC	= FWRO + 0x6200,
	FWCTFDCN0	= FWRO + 0x6300,
	FWLTHFDCN0	= FWRO + 0x6304,
	FWIPFDCN0	= FWRO + 0x6308,
	FWLTWFDCN0	= FWRO + 0x630C,
	FWPBFDCN0	= FWRO + 0x6310,
	FWMHLCN0	= FWRO + 0x6314,
	FWIHLCN0	= FWRO + 0x6318,
	FWICRDCN0	= FWRO + 0x6500,
	FWWMRDCN0	= FWRO + 0x6504,
	FWCTRDCN0	= FWRO + 0x6508,
	FWLTHRDCN0	= FWRO + 0x650C,
	FWIPRDCN0	= FWRO + 0x6510,
	FWLTWRDCN0	= FWRO + 0x6514,
	FWPBRDCN0	= FWRO + 0x6518,
	FWPMFDCN0	= FWRO + 0x6700,
	FWPGFDCN0	= FWRO + 0x6780,
	FWPMGDCN0	= FWRO + 0x6800,
	FWPMYDCN0	= FWRO + 0x6804,
	FWPMRDCN0	= FWRO + 0x6808,
	FWFRPPCN0	= FWRO + 0x6A00,
	FWFRDPCN0	= FWRO + 0x6A04,
	FWEIS00		= FWRO + 0x7900,
	FWEIE00		= FWRO + 0x7904,
	FWEID00		= FWRO + 0x7908,
	FWEIS1		= FWRO + 0x7A00,
	FWEIE1		= FWRO + 0x7A04,
	FWEID1		= FWRO + 0x7A08,
	FWEIS2		= FWRO + 0x7A10,
	FWEIE2		= FWRO + 0x7A14,
	FWEID2		= FWRO + 0x7A18,
	FWEIS3		= FWRO + 0x7A20,
	FWEIE3		= FWRO + 0x7A24,
	FWEID3		= FWRO + 0x7A28,
	FWEIS4		= FWRO + 0x7A30,
	FWEIE4		= FWRO + 0x7A34,
	FWEID4		= FWRO + 0x7A38,
	FWEIS5		= FWRO + 0x7A40,
	FWEIE5		= FWRO + 0x7A44,
	FWEID5		= FWRO + 0x7A48,
	FWEIS60		= FWRO + 0x7A50,
	FWEIE60		= FWRO + 0x7A54,
	FWEID60		= FWRO + 0x7A58,
	FWEIS61		= FWRO + 0x7A60,
	FWEIE61		= FWRO + 0x7A64,
	FWEID61		= FWRO + 0x7A68,
	FWEIS62		= FWRO + 0x7A70,
	FWEIE62		= FWRO + 0x7A74,
	FWEID62		= FWRO + 0x7A78,
	FWEIS63		= FWRO + 0x7A80,
	FWEIE63		= FWRO + 0x7A84,
	FWEID63		= FWRO + 0x7A88,
	FWEIS70		= FWRO + 0x7A90,
	FWEIE70		= FWRO + 0x7A94,
	FWEID70		= FWRO + 0x7A98,
	FWEIS71		= FWRO + 0x7AA0,
	FWEIE71		= FWRO + 0x7AA4,
	FWEID71		= FWRO + 0x7AA8,
	FWEIS72		= FWRO + 0x7AB0,
	FWEIE72		= FWRO + 0x7AB4,
	FWEID72		= FWRO + 0x7AB8,
	FWEIS73		= FWRO + 0x7AC0,
	FWEIE73		= FWRO + 0x7AC4,
	FWEID73		= FWRO + 0x7AC8,
	FWEIS80		= FWRO + 0x7AD0,
	FWEIE80		= FWRO + 0x7AD4,
	FWEID80		= FWRO + 0x7AD8,
	FWEIS81		= FWRO + 0x7AE0,
	FWEIE81		= FWRO + 0x7AE4,
	FWEID81		= FWRO + 0x7AE8,
	FWEIS82		= FWRO + 0x7AF0,
	FWEIE82		= FWRO + 0x7AF4,
	FWEID82		= FWRO + 0x7AF8,
	FWEIS83		= FWRO + 0x7B00,
	FWEIE83		= FWRO + 0x7B04,
	FWEID83		= FWRO + 0x7B08,
	FWMIS0		= FWRO + 0x7C00,
	FWMIE0		= FWRO + 0x7C04,
	FWMID0		= FWRO + 0x7C08,
	FWSCR0		= FWRO + 0x7D00,
	FWSCR1		= FWRO + 0x7D04,
	FWSCR2		= FWRO + 0x7D08,
	FWSCR3		= FWRO + 0x7D0C,
	FWSCR4		= FWRO + 0x7D10,
	FWSCR5		= FWRO + 0x7D14,
	FWSCR6		= FWRO + 0x7D18,
	FWSCR7		= FWRO + 0x7D1C,
	FWSCR8		= FWRO + 0x7D20,
	FWSCR9		= FWRO + 0x7D24,
	FWSCR10		= FWRO + 0x7D28,
	FWSCR11		= FWRO + 0x7D2C,
	FWSCR12		= FWRO + 0x7D30,
	FWSCR13		= FWRO + 0x7D34,
	FWSCR14		= FWRO + 0x7D38,
	FWSCR15		= FWRO + 0x7D3C,
	FWSCR16		= FWRO + 0x7D40,
	FWSCR17		= FWRO + 0x7D44,
	FWSCR18		= FWRO + 0x7D48,
	FWSCR19		= FWRO + 0x7D4C,
	FWSCR20		= FWRO + 0x7D50,
	FWSCR21		= FWRO + 0x7D54,
	FWSCR22		= FWRO + 0x7D58,
	FWSCR23		= FWRO + 0x7D5C,
	FWSCR24		= FWRO + 0x7D60,
	FWSCR25		= FWRO + 0x7D64,
	FWSCR26		= FWRO + 0x7D68,
	FWSCR27		= FWRO + 0x7D6C,
	FWSCR28		= FWRO + 0x7D70,
	FWSCR29		= FWRO + 0x7D74,
	FWSCR30		= FWRO + 0x7D78,
	FWSCR31		= FWRO + 0x7D7C,
	FWSCR32		= FWRO + 0x7D80,
	FWSCR33		= FWRO + 0x7D84,
	FWSCR34		= FWRO + 0x7D88,
	FWSCR35		= FWRO + 0x7D8C,
	FWSCR36		= FWRO + 0x7D90,
	FWSCR37		= FWRO + 0x7D94,
	FWSCR38		= FWRO + 0x7D98,
	FWSCR39		= FWRO + 0x7D9C,
	FWSCR40		= FWRO + 0x7DA0,
	FWSCR41		= FWRO + 0x7DA4,
	FWSCR42		= FWRO + 0x7DA8,
	FWSCR43		= FWRO + 0x7DAC,
	FWSCR44		= FWRO + 0x7DB0,
	FWSCR45		= FWRO + 0x7DB4,
	FWSCR46		= FWRO + 0x7DB8,

	RIPV		= CARO + 0x0000,
	RRC		= CARO + 0x0004,
	RCEC		= CARO + 0x0008,
	RCDC		= CARO + 0x000C,
	RSSIS		= CARO + 0x0010,
	RSSIE		= CARO + 0x0014,
	RSSID		= CARO + 0x0018,
	CABPIBWMC	= CARO + 0x0020,
	CABPWMLC	= CARO + 0x0040,
	CABPPFLC0	= CARO + 0x0050,
	CABPPWMLC0	= CARO + 0x0060,
	CABPPPFLC00	= CARO + 0x00A0,
	CABPULC		= CARO + 0x0100,
	CABPIRM		= CARO + 0x0140,
	CABPPCM		= CARO + 0x0144,
	CABPLCM		= CARO + 0x0148,
	CABPCPM		= CARO + 0x0180,
	CABPMCPM	= CARO + 0x0200,
	CARDNM		= CARO + 0x0280,
	CARDMNM		= CARO + 0x0284,
	CARDCN		= CARO + 0x0290,
	CAEIS0		= CARO + 0x0300,
	CAEIE0		= CARO + 0x0304,
	CAEID0		= CARO + 0x0308,
	CAEIS1		= CARO + 0x0310,
	CAEIE1		= CARO + 0x0314,
	CAEID1		= CARO + 0x0318,
	CAMIS0		= CARO + 0x0340,
	CAMIE0		= CARO + 0x0344,
	CAMID0		= CARO + 0x0348,
	CAMIS1		= CARO + 0x0350,
	CAMIE1		= CARO + 0x0354,
	CAMID1		= CARO + 0x0358,
	CASCR		= CARO + 0x0380,

	/* Ethernet Agent Address space Empty in spec */
	EAMC		= TARO + 0x0000,
	EAMS		= TARO + 0x0004,
	EAIRC		= TARO + 0x0010,
	EATDQSC		= TARO + 0x0014,
	EATDQC		= TARO + 0x0018,
	EATDQAC		= TARO + 0x001C,
	EATPEC		= TARO + 0x0020,
	EATMFSC0	= TARO + 0x0040,
	EATDQDC0	= TARO + 0x0060,
	EATDQM0		= TARO + 0x0080,
	EATDQMLM0	= TARO + 0x00A0,
	EACTQC		= TARO + 0x0100,
	EACTDQDC	= TARO + 0x0104,
	EACTDQM		= TARO + 0x0108,
	EACTDQMLM	= TARO + 0x010C,
	EAVCC		= TARO + 0x0130,
	EAVTC		= TARO + 0x0134,
	EATTFC		= TARO + 0x0138,
	EACAEC		= TARO + 0x0200,
	EACC		= TARO + 0x0204,
	EACAIVC0	= TARO + 0x0220,
	EACAULC0	= TARO + 0x0240,
	EACOEM		= TARO + 0x0260,
	EACOIVM0	= TARO + 0x0280,
	EACOULM0	= TARO + 0x02A0,
	EACGSM		= TARO + 0x02C0,
	EATASC		= TARO + 0x0300,
	EATASENC0	= TARO + 0x0320,
	EATASCTENC	= TARO + 0x0340,
	EATASENM0	= TARO + 0x0360,
	EATASCTENM	= TARO + 0x0380,
	EATASCSTC0	= TARO + 0x03A0,
	EATASCSTC1	= TARO + 0x03A4,
	EATASCSTM0	= TARO + 0x03A8,
	EATASCSTM1	= TARO + 0x03AC,
	EATASCTC	= TARO + 0x03B0,
	EATASCTM	= TARO + 0x03B4,
	EATASGL0	= TARO + 0x03C0,
	EATASGL1	= TARO + 0x03C4,
	EATASGLR	= TARO + 0x03C8,
	EATASGR		= TARO + 0x03D0,
	EATASGRR	= TARO + 0x03D4,
	EATASHCC	= TARO + 0x03E0,
	EATASRIRM	= TARO + 0x03E4,
	EATASSM		= TARO + 0x03E8,
	EAUSMFSECN	= TARO + 0x0400,
	EATFECN		= TARO + 0x0404,
	EAFSECN		= TARO + 0x0408,
	EADQOECN	= TARO + 0x040C,
	EADQSECN	= TARO + 0x0410,
	EACKSECN	= TARO + 0x0414,
	EAEIS0		= TARO + 0x0500,
	EAEIE0		= TARO + 0x0504,
	EAEID0		= TARO + 0x0508,
	EAEIS1		= TARO + 0x0510,
	EAEIE1		= TARO + 0x0514,
	EAEID1		= TARO + 0x0518,
	EAEIS2		= TARO + 0x0520,
	EAEIE2		= TARO + 0x0524,
	EAEID2		= TARO + 0x0528,
	EASCR		= TARO + 0x0580,

	MPSM		= RMRO + 0x0000,
	MPIC		= RMRO + 0x0004,
	MPIM		= RMRO + 0x0008,
	MIOC		= RMRO + 0x0010,
	MIOM		= RMRO + 0x0014,
	MXMS		= RMRO + 0x0018,
	MTFFC		= RMRO + 0x0020,
	MTPFC		= RMRO + 0x0024,
	MTPFC2		= RMRO + 0x0028,
	MTPFC30		= RMRO + 0x0030,
	MTATC0		= RMRO + 0x0050,
	MTIM		= RMRO + 0x0060,
	MRGC		= RMRO + 0x0080,
	MRMAC0		= RMRO + 0x0084,
	MRMAC1		= RMRO + 0x0088,
	MRAFC		= RMRO + 0x008C,
	MRSCE		= RMRO + 0x0090,
	MRSCP		= RMRO + 0x0094,
	MRSCC		= RMRO + 0x0098,
	MRFSCE		= RMRO + 0x009C,
	MRFSCP		= RMRO + 0x00a0,
	MTRC		= RMRO + 0x00a4,
	MRIM		= RMRO + 0x00a8,
	MRPFM		= RMRO + 0x00aC,
	MPFC0		= RMRO + 0x0100,
	MLVC		= RMRO + 0x0180,
	MEEEC		= RMRO + 0x0184,
	MLBC		= RMRO + 0x0188,
	MXGMIIC		= RMRO + 0x0190,
	MPCH		= RMRO + 0x0194,
	MANC		= RMRO + 0x0198,
	MANM		= RMRO + 0x019C,
	MPLCA1		= RMRO + 0x01a0,
	MPLCA2		= RMRO + 0x01a4,
	MPLCA3		= RMRO + 0x01a8,
	MPLCA4		= RMRO + 0x01ac,
	MPLCAM		= RMRO + 0x01b0,
	MHDC1		= RMRO + 0x01c0,
	MHDC2		= RMRO + 0x01c4,
	MEIS		= RMRO + 0x0200,
	MEIE		= RMRO + 0x0204,
	MEID		= RMRO + 0x0208,
	MMIS0		= RMRO + 0x0210,
	MMIE0		= RMRO + 0x0214,
	MMID0		= RMRO + 0x0218,
	MMIS1		= RMRO + 0x0220,
	MMIE1		= RMRO + 0x0224,
	MMID1		= RMRO + 0x0228,
	MMIS2		= RMRO + 0x0230,
	MMIE2		= RMRO + 0x0234,
	MMID2		= RMRO + 0x0238,
	MMPFTCT		= RMRO + 0x0300,
	MAPFTCT		= RMRO + 0x0304,
	MPFRCT		= RMRO + 0x0308,
	MFCICT		= RMRO + 0x030c,
	MEEECT		= RMRO + 0x0310,
	MMPCFTCT0	= RMRO + 0x0320,
	MAPCFTCT0	= RMRO + 0x0330,
	MPCFRCT0	= RMRO + 0x0340,
	MHDCC		= RMRO + 0x0350,
	MROVFC		= RMRO + 0x0354,
	MRHCRCEC	= RMRO + 0x0358,
	MRXBCE		= RMRO + 0x0400,
	MRXBCP		= RMRO + 0x0404,
	MRGFCE		= RMRO + 0x0408,
	MRGFCP		= RMRO + 0x040C,
	MRBFC		= RMRO + 0x0410,
	MRMFC		= RMRO + 0x0414,
	MRUFC		= RMRO + 0x0418,
	MRPEFC		= RMRO + 0x041C,
	MRNEFC		= RMRO + 0x0420,
	MRFMEFC		= RMRO + 0x0424,
	MRFFMEFC	= RMRO + 0x0428,
	MRCFCEFC	= RMRO + 0x042C,
	MRFCEFC		= RMRO + 0x0430,
	MRRCFEFC	= RMRO + 0x0434,
	MRUEFC		= RMRO + 0x043C,
	MROEFC		= RMRO + 0x0440,
	MRBOEC		= RMRO + 0x0444,
	MTXBCE		= RMRO + 0x0500,
	MTXBCP		= RMRO + 0x0504,
	MTGFCE		= RMRO + 0x0508,
	MTGFCP		= RMRO + 0x050C,
	MTBFC		= RMRO + 0x0510,
	MTMFC		= RMRO + 0x0514,
	MTUFC		= RMRO + 0x0518,
	MTEFC		= RMRO + 0x051C,

	GWMC		= GWRO + 0x0000,
	GWMS		= GWRO + 0x0004,
	GWIRC		= GWRO + 0x0010,
	GWRDQSC		= GWRO + 0x0014,
	GWRDQC		= GWRO + 0x0018,
	GWRDQAC		= GWRO + 0x001C,
	GWRGC		= GWRO + 0x0020,
	GWRMFSC0	= GWRO + 0x0040,
	GWRDQDC0	= GWRO + 0x0060,
	GWRDQM0		= GWRO + 0x0080,
	GWRDQMLM0	= GWRO + 0x00A0,
	GWMTIRM		= GWRO + 0x0100,
	GWMSTLS		= GWRO + 0x0104,
	GWMSTLR		= GWRO + 0x0108,
	GWMSTSS		= GWRO + 0x010C,
	GWMSTSR		= GWRO + 0x0110,
	GWMAC0		= GWRO + 0x0120,
	GWMAC1		= GWRO + 0x0124,
	GWVCC		= GWRO + 0x0130,
	GWVTC		= GWRO + 0x0134,
	GWTTFC		= GWRO + 0x0138,
	GWTDCAC00	= GWRO + 0x0140,
	GWTDCAC10	= GWRO + 0x0144,
	GWTSDCC0	= GWRO + 0x0160,
	GWTNM		= GWRO + 0x0180,
	GWTMNM		= GWRO + 0x0184,
	GWAC		= GWRO + 0x0190,
	GWDCBAC0	= GWRO + 0x0194,
	GWDCBAC1	= GWRO + 0x0198,
	GWIICBSC	= GWRO + 0x019C,
	GWMDNC		= GWRO + 0x01A0,
	GWTRC0		= GWRO + 0x0200,
	GWTPC0		= GWRO + 0x0300,
	GWARIRM		= GWRO + 0x0380,
	GWDCC0		= GWRO + 0x0400,
	GWAARSS		= GWRO + 0x0800,
	GWAARSR0	= GWRO + 0x0804,
	GWAARSR1	= GWRO + 0x0808,
	GWIDAUAS0	= GWRO + 0x0840,
	GWIDASM0	= GWRO + 0x0880,
	GWIDASAM00	= GWRO + 0x0900,
	GWIDASAM10	= GWRO + 0x0904,
	GWIDACAM00	= GWRO + 0x0980,
	GWIDACAM10	= GWRO + 0x0984,
	GWGRLC		= GWRO + 0x0A00,
	GWGRLULC	= GWRO + 0x0A04,
	GWRLIVC0	= GWRO + 0x0A80,
	GWRLULC0	= GWRO + 0x0A84,
	GWIDPC		= GWRO + 0x0B00,
	GWIDC0		= GWRO + 0x0C00,
	GWDIS0		= GWRO + 0x1100,
	GWDIE0		= GWRO + 0x1104,
	GWDID0		= GWRO + 0x1108,
	GWTSDIS		= GWRO + 0x1180,
	GWTSDIE		= GWRO + 0x1184,
	GWTSDID		= GWRO + 0x1188,
	GWEIS0		= GWRO + 0x1190,
	GWEIE0		= GWRO + 0x1194,
	GWEID0		= GWRO + 0x1198,
	GWEIS1		= GWRO + 0x11A0,
	GWEIE1		= GWRO + 0x11A4,
	GWEID1		= GWRO + 0x11A8,
	GWEIS20		= GWRO + 0x1200,
	GWEIE20		= GWRO + 0x1204,
	GWEID20		= GWRO + 0x1208,
	GWEIS3		= GWRO + 0x1280,
	GWEIE3		= GWRO + 0x1284,
	GWEID3		= GWRO + 0x1288,
	GWEIS4		= GWRO + 0x1290,
	GWEIE4		= GWRO + 0x1294,
	GWEID4		= GWRO + 0x1298,
	GWEIS5		= GWRO + 0x12A0,
	GWEIE5		= GWRO + 0x12A4,
	GWEID5		= GWRO + 0x12A8,
	GWSCR0		= GWRO + 0x1800,
	GWSCR1		= GWRO + 0x1900,
};

/* ETHA/RMAC */
enum rswitch_etha_mode {
	EAMC_OPC_RESET,
	EAMC_OPC_DISABLE,
	EAMC_OPC_CONFIG,
	EAMC_OPC_OPERATION,
};
#define EAMS_OPS_MASK	EAMC_OPC_OPERATION

#define EAVCC_VEM_SC_TAG	(0x3 << 16)

#define MPIC_PIS_MII	0x00
#define MPIC_PIS_GMII	0x02
#define MPIC_PIS_XGMII	0x04
#define MPIC_LSC_SHIFT	3
#define MPIC_LSC_10M	(0 << MPIC_LSC_SHIFT)
#define MPIC_LSC_100M	(1 << MPIC_LSC_SHIFT)
#define MPIC_LSC_1G	(2 << MPIC_LSC_SHIFT)
#define MPIC_LSC_2_5G	(3 << MPIC_LSC_SHIFT)
#define MPIC_LSC_5G	(4 << MPIC_LSC_SHIFT)
#define MPIC_LSC_10G	(5 << MPIC_LSC_SHIFT)

#define MDIO_READ_C45		0x03
#define MDIO_WRITE_C45		0x01

#define REG_MASK		0xffff
#define DEV_MASK		GENMASK(24, 16)
#define ACCESS_MODE		BIT(30)

#define MPSM_PSME		BIT(0)
#define MPSM_MFF_C45		BIT(2)
#define MPSM_PDA_SHIFT		3
#define MPSM_PDA_MASK		GENMASK(7, MPSM_PDA_SHIFT)
#define MPSM_PDA(val)		((val) << MPSM_PDA_SHIFT)
#define MPSM_PRA_SHIFT		8
#define MPSM_PRA_MASK		GENMASK(12, MPSM_PRA_SHIFT)
#define MPSM_PRA(val)		((val) << MPSM_PRA_SHIFT)
#define MPSM_POP_SHIFT		13
#define MPSM_POP_MASK		GENMASK(14, MPSM_POP_SHIFT)
#define MPSM_POP(val)		((val) << MPSM_POP_SHIFT)
#define MPSM_PRD_SHIFT		16
#define MPSM_PRD_MASK		GENMASK(31, MPSM_PRD_SHIFT)
#define MPSM_PRD_WRITE(val)	((val) << MPSM_PRD_SHIFT)
#define MPSM_PRD_READ(val)	((val) & MPSM_PRD_MASK >> MPSM_PRD_SHIFT)

/* Completion flags */
#define MMIS1_PAACS             BIT(2) /* Address */
#define MMIS1_PWACS             BIT(1) /* Write */
#define MMIS1_PRACS             BIT(0) /* Read */
#define MMIS1_CLEAR_FLAGS       0xf

#define MPIC_PSMCS_SHIFT	16
#define MPIC_PSMCS_MASK		GENMASK(22, MPIC_PSMCS_SHIFT)
#define MPIC_PSMCS(val)		((val) << MPIC_PSMCS_SHIFT)

#define MPIC_PSMHT_SHIFT	24
#define MPIC_PSMHT_MASK		GENMASK(26, MPIC_PSMHT_SHIFT)
#define MPIC_PSMHT(val)		((val) << MPIC_PSMHT_SHIFT)

#define MLVC_PLV	BIT(16)

/* GWCA */
#define GWMS_OPS_MASK	GWMC_OPC_OPERATION

#define GWMTIRM_MTIOG	BIT(0)
#define GWMTIRM_MTR	BIT(1)

#define GWVCC_VEM_SC_TAG	(0x3 << 16)

#define GWARIRM_ARIOG	BIT(0)
#define GWARIRM_ARR	BIT(1)

#define GWDCC_BALR		BIT(24)
#define GWDCC_DCP(q, idx)	((q + (idx * 2)) << 16)
#define GWDCC_DQT		BIT(11)
#define GWDCC_ETS		BIT(9)
#define GWDCC_EDE		BIT(8)
#define GWDCC_OSID(val)		((val & 0x7) << 28)

#define GWMDNC_TXDMN(val)	((val & 0x1f) << 8)

#define GWDCC_OFFS(chain)	(GWDCC0 + (chain) * 4)
/* COMA */
#define RRC_RR		BIT(0)
#define RRC_RR_CLR	(0)
#define RCEC_RCE	BIT(16)
#define RCDC_RCD	BIT(16)

#define CABPIRM_BPIOG	BIT(0)
#define CABPIRM_BPR	BIT(1)

/* MFWD */
#define FWPC0_LTHTA	BIT(0)
#define FWPC0_IP4UE	BIT(3)
#define FWPC0_IP4TE	BIT(4)
#define FWPC0_IP4OE	BIT(5)
#define FWPC0_L2SE	BIT(9)
#define FWPC0_IP4EA	BIT(10)
#define FWPC0_IPDSA	BIT(12)
#define FWPC0_IPHLA	BIT(18)
#define FWPC0_MACSDA	BIT(20)
#define FWPC0_MACHLA	BIT(26)
#define FWPC0_MACHMA	BIT(27)
#define FWPC0_VLANSA	BIT(28)

#define LTHSLP0NONE (0)
#define LTHSLP0v4OTHER (1)
#define LTHSLP0v4UDP (2)
#define LTHSLP0v4TCP (3)
#define LTHSLP0v6 (6)
/* L3 Routing Valid Learn */
#define LTHRVL (BIT(15))
#define LTHTL (BIT(31))
#define LTHTS (BIT(31))
#define LTHTIOG (BIT(0))
#define LTHTR (BIT(1))
/* L3 Entry Delete */
#define LTHED (BIT(16))

/* Update TTL */
#define L23UTTLUL (BIT(16))
/* Update destination MAC */
#define L23UMDAUL (BIT(17))
/* Update source MAC */
#define L23UMSAUL (BIT(18))

#define FWTWBFVCi(i) (FWTWBFVC0 + ((i) * 0x10))
#define FWTWBFVCVALMASK(val, mask) ((val) | (~mask) << 16)
#define FWTHBFV0Ci(i) (FWTHBFV0C0 + ((i) * 0x10))
#define FWTHBFV1Ci(i) (FWTHBFV1C0 + ((i) * 0x10))
#define FWFOBFV0Ci(i) (FWFOBFV0C0 + ((i) * 0x10))
#define FWFOBFV1Ci(i) (FWFOBFV1C0 + ((i) * 0x10))

#define FWTWBFCi(i) (FWTWBFC0 + ((i) * 0x10))
#define FWTHBFCi(i) (FWTHBFC0 + ((i) * 0x10))
#define FWFOBFCi(i) (FWFOBFC0 + ((i) * 0x10))
#define FWCFMCij(i, j) (FWCFMC00 + ((i) * 0x40 + (j) * 0x4))
#define FWCFCi(i) (FWCFC0 + ((i) * 0x40))
#define SNOOPING_BUS_OFFSET(offset) ((offset) << 16)
#define TWBFILTER_NUM(i) (2 * (i))
#define THBFILTER_NUM(i) (2 * (PFL_TWBF_N + i))
#define FBFILTER_NUM(i) (2 * (PFL_TWBF_N + PFL_THBF_N + i))
#define TBWFILTER_IDX(i) ((i / 2))
#define THBFILTER_IDX(i) ((i / 2) - PFL_TWBF_N)
#define FBFILTER_IDX(i) ((i / 2) - PFL_TWBF_N - PFL_THBF_N)

#define filter_index_check(idx, idx_max) \
	(idx < idx_max) ? idx : -1;

#define get_two_byte_filter(priv) \
	filter_index_check(find_first_zero_bit(priv->filters.two_bytes, PFL_TWBF_N), PFL_TWBF_N)
#define get_three_byte_filter(priv) \
	filter_index_check(find_first_zero_bit(priv->filters.three_bytes, PFL_THBF_N), PFL_THBF_N)
#define get_four_byte_filter(priv) \
	filter_index_check(find_first_zero_bit(priv->filters.four_bytes, PFL_FOBF_N), PFL_FOBF_N)


#define MASK_MODE (0)
#define EXPAND_MODE (BIT(0))
#define PRECISE_MODE (BIT(1))

#define DISABLE_FILTER (0)
#define ENABLE_FILTER (BIT(15))

#define FWPC0_DEFAULT	(FWPC0_LTHTA | FWPC0_IP4UE | FWPC0_IP4TE | \
			 FWPC0_IP4OE | FWPC0_L2SE | FWPC0_IP4EA | \
			 FWPC0_IPDSA | FWPC0_IPHLA | FWPC0_MACSDA | \
			 FWPC0_MACHLA |	FWPC0_MACHMA | FWPC0_VLANSA)

#define FWPC1_DDE	BIT(0)

#define	FWPBFC(i)		(FWPBFCi + (i) * 0x10)
#define	FWPBFC_PBDV_MASK	(GENMASK(RSWITCH_NUM_HW - 1, 0)

#define FWPBFCSDC(j, i)         (FWPBFCSDC00 + (i) * 0x10 + (j) * 0x04)

/* SerDes */
enum rswitch_serdes_mode {
	USXGMII,
	SGMII,
	COMBINATION,
};

#define RSWITCH_SERDES_OFFSET                   0x0400
#define RSWITCH_SERDES_BANK_SELECT              0x03fc

#define BANK_180                                0x0180
#define VR_XS_PMA_MP_12G_16G_25G_SRAM           0x026c
#define VR_XS_PMA_MP_12G_16G_25G_REF_CLK_CTRL   0x0244
#define VR_XS_PMA_MP_10G_MPLLA_CTRL2            0x01cc
#define VR_XS_PMA_MP_12G_16G_25G_MPLL_CMN_CTRL  0x01c0
#define VR_XS_PMA_MP_12G_16G_MPLLA_CTRL0        0x01c4
#define VR_XS_PMA_MP_12G_MPLLA_CTRL1            0x01c8
#define VR_XS_PMA_MP_12G_MPLLA_CTRL3            0x01dc
#define VR_XS_PMA_MP_12G_16G_25G_VCO_CAL_LD0    0x0248
#define VR_XS_PMA_MP_12G_VCO_CAL_REF0           0x0258
#define VR_XS_PMA_MP_12G_16G_25G_RX_GENCTRL1    0x0144
#define VR_XS_PMA_CONSUMER_10G_RX_GENCTRL4      0x01a0
#define VR_XS_PMA_MP_12G_16G_25G_TX_RATE_CTRL   0x00d0
#define VR_XS_PMA_MP_12G_16G_25G_RX_RATE_CTRL   0x0150
#define VR_XS_PMA_MP_12G_16G_TX_GENCTRL2        0x00c8
#define VR_XS_PMA_MP_12G_16G_RX_GENCTRL2        0x0148
#define VR_XS_PMA_MP_12G_AFE_DFE_EN_CTRL        0x0174
#define VR_XS_PMA_MP_12G_RX_EQ_CTRL0            0x0160
#define VR_XS_PMA_MP_10G_RX_IQ_CTRL0            0x01ac
#define VR_XS_PMA_MP_12G_16G_25G_TX_GENCTRL1    0x00c4
#define VR_XS_PMA_MP_12G_16G_TX_GENCTRL2        0x00c8
#define VR_XS_PMA_MP_12G_16G_RX_GENCTRL2        0x0148
#define VR_XS_PMA_MP_12G_16G_25G_TX_GENCTRL1    0x00c4
#define VR_XS_PMA_MP_12G_16G_25G_TX_EQ_CTRL0    0x00d8
#define VR_XS_PMA_MP_12G_16G_25G_TX_EQ_CTRL1    0x00dc
#define VR_XS_PMA_MP_12G_16G_MPLLB_CTRL0        0x01d0
#define VR_XS_PMA_MP_12G_MPLLB_CTRL1            0x01d4
#define VR_XS_PMA_MP_12G_16G_MPLLB_CTRL2        0x01d8
#define VR_XS_PMA_MP_12G_MPLLB_CTRL3            0x01e0

#define BANK_300                                0x0300
#define SR_XS_PCS_CTRL1                         0x0000
#define SR_XS_PCS_STS1                          0x0004
#define SR_XS_PCS_CTRL2                         0x001c

#define BANK_380                                0x0380
#define VR_XS_PCS_DIG_CTRL1                     0x0000
#define VR_XS_PCS_DEBUG_CTRL                    0x0014
#define VR_XS_PCS_KR_CTRL                       0x001c

#define BANK_1F00                               0x1f00
#define SR_MII_CTRL                             0x0000

#define BANK_1F80                               0x1f80
#define VR_MII_AN_CTRL                          0x0004

#define NUM_CHAINS_PER_NDEV	3

struct rswitch_fib_event_work {
	struct work_struct work;
	union {
		struct fib_entry_notifier_info fen_info;
		struct fib_rule_notifier_info fr_info;
	};
	struct rswitch_private *priv;
	unsigned long event;
};

struct l3_ipv4_fwd_param_list {
	struct l3_ipv4_fwd_param *param;
	struct list_head list;
};

struct rswitch_ipv4_route {
	u32 ip;
	u32 subnet;
	u32 mask;
	struct rswitch_device *dev;
	struct list_head param_list;
	struct list_head list;
};

static int num_ndev = 3;
module_param(num_ndev, int, 0644);
MODULE_PARM_DESC(num_ndev, "Number of creating network devices");

static int num_etha_ports = 3;
module_param(num_etha_ports, int, 0644);
MODULE_PARM_DESC(num_etha_ports, "Number of using ETHA ports");

static bool parallel_mode;
module_param(parallel_mode, bool, 0644);
MODULE_PARM_DESC(parallel_mode, "Operate simultaneously with Realtime core");

static int num_virt_devices = 6;
module_param(num_virt_devices, int, 0644);
MODULE_PARM_DESC(num_virt_devices, "Number of virtual interfaces");

struct rswitch_net {
	struct rswitch_private *priv;
};

static unsigned int rswitch_net_id;

#define RSWITCH_TIMEOUT_MS	1000

/* HACK: store rswitch_priv globally so Xen backend can access it */
/* TODO: Implement correct way of accessing private data */
static struct rswitch_private *glob_priv;
struct rswitch_private *rswitch_find_priv(void)
{
	return glob_priv;
};

static int rswitch_reg_wait(void __iomem *addr, u32 offs, u32 mask, u32 expected)
{
	int i;

	for (i = 0; i < RSWITCH_TIMEOUT_MS; i++) {
		if ((rs_read32(addr + offs) & mask) == expected)
			return 0;

		mdelay(1);
	}

	return -ETIMEDOUT;
}

static u32 rswitch_etha_offs(int index)
{
	return RSWITCH_ETHA_OFFSET + index * RSWITCH_ETHA_SIZE;
}

static u32 rswitch_etha_read(struct rswitch_etha *etha, enum rswitch_reg reg)
{
	return rs_read32(etha->addr + reg);
}

static void rswitch_etha_write(struct rswitch_etha *etha, u32 data, enum rswitch_reg reg)
{
	rs_write32(data, etha->addr + reg);
}

static void rswitch_etha_modify(struct rswitch_etha *etha, enum rswitch_reg reg,
				u32 clear, u32 set)
{
	rswitch_etha_write(etha, (rswitch_etha_read(etha, reg) & ~clear) | set, reg);
}

static void rswitch_modify(void __iomem *addr, enum rswitch_reg reg, u32 clear, u32 set)
{
	rs_write32((rs_read32(addr + reg) & ~clear) | set, addr + reg);
}

static bool __maybe_unused rswitch_is_any_data_irq(struct rswitch_private *priv, u32 *dis, bool tx)
{
	int i;
	u32 *mask = tx ? priv->gwca.tx_irq_bits : priv->gwca.rx_irq_bits;

	for (i = 0; i < RSWITCH_NUM_IRQ_REGS; i++) {
		if (dis[i] & mask[i])
			return true;
	}

	return false;
}

static void rswitch_get_data_irq_status(struct rswitch_private *priv, u32 *dis)
{
	int i;

	for (i = 0; i < RSWITCH_NUM_IRQ_REGS; i++)
		dis[i] = rs_read32(priv->addr + GWDIS0 + i * 0x10);
}

void rswitch_enadis_data_irq(struct rswitch_private *priv, int index, bool enable)
{
	u32 offs = (enable ? GWDIE0 : GWDID0) + (index / 32) * 0x10;
	u32 tmp = 0;

	/* For VPF? */
	if (enable)
		tmp = rs_read32(priv->addr + offs);

	rs_write32(BIT(index % 32) | tmp, priv->addr + offs);
}

void rswitch_enadis_rdev_irqs(struct rswitch_device *rdev, bool enable)
{
	if (!rswitch_is_front_dev(rdev))
	{
		rswitch_enadis_data_irq(rdev->priv, rdev->rx_default_chain->index,
					enable);
		rswitch_enadis_data_irq(rdev->priv, rdev->rx_learning_chain->index,
					enable);
		rswitch_enadis_data_irq(rdev->priv, rdev->tx_chain->index,
					enable);
	}
	else
	{
		if (enable)
			rswitch_vmq_front_rx_done(rdev);
	}
}

void rswitch_trigger_chain(struct rswitch_private *priv,
			   struct rswitch_gwca_chain *chain)
{
	if (!rswitch_is_front_priv(priv))
		rswitch_modify(priv->addr, GWTRC0, 0, BIT(chain->index));
	else
		rswitch_vmq_front_trigger_tx(netdev_priv(chain->ndev));
}

static void rswitch_ack_data_irq(struct rswitch_private *priv, int index)
{
	u32 offs = GWDIS0 + (index / 32) * 0x10;

	rs_write32(BIT(index % 32), priv->addr + offs);
}

static bool rswitch_is_chain_rxed(struct rswitch_gwca_chain *c, u8 unexpected)
{
	int entry;
	struct rswitch_ext_ts_desc *desc;

	entry = c->dirty % c->num_ring;
	desc = &c->ts_ring[entry];

	if ((desc->die_dt & DT_MASK) != unexpected)
		return true;

	return false;
}

void rswitch_add_ipv4_forward(struct rswitch_private *priv, u32 src_ip, u32 dst_ip);

static bool rswitch_rx_chain(struct net_device *ndev, int *quota, struct rswitch_gwca_chain *c, bool learn_chain)
//static bool rswitch_rx(struct net_device *ndev, int *quota)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
//	struct rswitch_gwca_chain *c = rdev->rx_chain;
	int boguscnt = c->dirty + c->num_ring - c->cur;
	int entry = c->cur % c->num_ring;
	struct rswitch_ext_ts_desc *desc = &c->ts_ring[entry];
	int limit;
	u16 pkt_len;
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	u32 get_ts;

	boguscnt = min(boguscnt, *quota);
	limit = boguscnt;

	while ((desc->die_dt & DT_MASK) != DT_FEMPTY) {
		dma_rmb();
		pkt_len = le16_to_cpu(desc->info_ds) & RX_DS;
		if (--boguscnt < 0)
			break;
		skb = c->skb[entry];

		if (learn_chain) {
			struct rswitch_private *priv = rdev->priv;
			struct iphdr *iphdr;
			struct ethhdr *ethhdr;

			skb_reset_mac_header(skb);
			skb_reset_network_header(skb);

			ethhdr = (struct ethhdr*)skb_mac_header(skb);
			skb_set_network_header(skb, sizeof(*ethhdr));

			iphdr = ip_hdr(skb);
			rswitch_add_ipv4_forward(priv, be32_to_cpu(iphdr->saddr), be32_to_cpu(iphdr->daddr));
		}

		c->skb[entry] = NULL;
		dma_addr = le32_to_cpu(desc->dptrl) | ((__le64)le32_to_cpu(desc->dptrh) << 32);
		dma_unmap_single(ndev->dev.parent, dma_addr, PKT_BUF_SZ, DMA_FROM_DEVICE);
		if (!rswitch_is_front_dev(rdev))
			get_ts = rdev->priv->ptp_priv->tstamp_rx_ctrl & RTSN_RXTSTAMP_TYPE_V2_L2_EVENT;
		if (get_ts) {
			struct skb_shared_hwtstamps *shhwtstamps;
			struct timespec64 ts;

			shhwtstamps = skb_hwtstamps(skb);
			memset(shhwtstamps, 0, sizeof(*shhwtstamps));
			ts.tv_sec = (u64)le32_to_cpu(desc->ts_sec);
			ts.tv_nsec = le32_to_cpu(desc->ts_nsec & 0x3FFFFFFF);
			shhwtstamps->hwtstamp = timespec64_to_ktime(ts);
		}
		skb_put(skb, pkt_len);
		skb->protocol = eth_type_trans(skb, ndev);
		netif_receive_skb(skb);
		rdev->ndev->stats.rx_packets++;
		rdev->ndev->stats.rx_bytes += pkt_len;

		entry = (++c->cur) % c->num_ring;
		desc = &c->ts_ring[entry];
	}

	/* Refill the RX ring buffers */
	for (; c->cur - c->dirty > 0; c->dirty++) {
		entry = c->dirty % c->num_ring;
		desc = &c->ts_ring[entry];
		desc->info_ds = cpu_to_le16(PKT_BUF_SZ);

		if (!c->skb[entry]) {
			skb = dev_alloc_skb(PKT_BUF_SZ + RSWITCH_ALIGN - 1);
			if (!skb)
				break;	/* Better luch next round */
			skb_reserve(skb, NET_IP_ALIGN);
			dma_addr = dma_map_single(ndev->dev.parent, skb->data,
						  le16_to_cpu(desc->info_ds),
						  DMA_FROM_DEVICE);
			if (dma_mapping_error(ndev->dev.parent, dma_addr))
				desc->info_ds = cpu_to_le16(0);
			desc->dptrl = cpu_to_le32(lower_32_bits(dma_addr));
			desc->dptrh = cpu_to_le32(upper_32_bits(dma_addr));
			skb_checksum_none_assert(skb);
			c->skb[entry] = skb;
		}
		dma_wmb();
		desc->die_dt = DT_FEMPTY | DIE;
	}

	*quota -= limit - (++boguscnt);

	return boguscnt <= 0;
}

static bool rswitch_rx(struct net_device *ndev, int *quota)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct rswitch_gwca_chain *default_chain = rdev->rx_default_chain;
	struct rswitch_gwca_chain *learning_chain = rdev->rx_learning_chain;
	bool res;

	res = rswitch_rx_chain(ndev, quota, default_chain, false);

	if (res)
		return res;

	if (learning_chain)
		res = rswitch_rx_chain(ndev, quota, learning_chain, true);

	return res;
}

static void rswitch_get_timestamp(struct rswitch_private *priv,
				  struct timespec64 *ts)
{
	struct rtsn_ptp_private *ptp_priv = priv->ptp_priv;

	ptp_priv->info.gettime64(&ptp_priv->info, ts);
}

int rswitch_tx_free(struct net_device *ndev, bool free_txed_only)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct rswitch_ext_desc *desc;
	int free_num = 0;
	int entry, size;
	dma_addr_t dma_addr;
	struct rswitch_gwca_chain *c = rdev->tx_chain;
	struct sk_buff *skb;

	for (; c->cur - c->dirty > 0; c->dirty++) {
		entry = c->dirty % c->num_ring;
		desc = &c->ring[entry];
		if (free_txed_only && (desc->die_dt & DT_MASK) != DT_FEMPTY)
			break;

		dma_rmb();
		size = le16_to_cpu(desc->info_ds) & TX_DS;
		skb = c->skb[entry];
		if (skb) {
			if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP &&
			    !rswitch_is_front_dev(rdev)) {
				struct skb_shared_hwtstamps shhwtstamps;
				struct timespec64 ts;

				rswitch_get_timestamp(rdev->priv, &ts);
				memset(&shhwtstamps, 0, sizeof(shhwtstamps));
				shhwtstamps.hwtstamp = timespec64_to_ktime(ts);
				skb_tstamp_tx(skb, &shhwtstamps);
			}
			dma_addr = le32_to_cpu(desc->dptrl) |
				   ((__le64)le32_to_cpu(desc->dptrh) << 32);
			dma_unmap_single(ndev->dev.parent, dma_addr,
					size, DMA_TO_DEVICE);
			dev_kfree_skb_any(c->skb[entry]);
			c->skb[entry] = NULL;
			free_num++;
		}
		desc->die_dt = DT_EEMPTY;
		rdev->ndev->stats.tx_packets++;
		rdev->ndev->stats.tx_bytes += size;
	}

	return free_num;
}

int rswitch_poll(struct napi_struct *napi, int budget)
{
	struct net_device *ndev = napi->dev;
	struct rswitch_device *rdev = netdev_priv(ndev);
	int quota = budget;

retry:
	rswitch_tx_free(ndev, true);

	if (rswitch_rx(ndev, &quota))
		goto out;
	else if (rswitch_is_chain_rxed(rdev->rx_default_chain, DT_FEMPTY))
		goto retry;
	else if (rswitch_is_chain_rxed(rdev->rx_learning_chain, DT_FEMPTY))
		goto retry;

	netif_wake_subqueue(ndev, 0);

	napi_complete(napi);

	/* Re-enable RX/TX interrupts */
	rswitch_enadis_rdev_irqs(rdev, true);
	__iowmb();

out:
	return budget - quota;
}

static bool rswitch_agent_clock_is_enabled(void __iomem *base_addr, int port)
{
	u32 val = rs_read32(base_addr + RCEC);

	if (val & RCEC_RCE)
		return (val & BIT(port)) ? true : false;
	else
		return false;
}

static void rswitch_agent_clock_ctrl(void __iomem *base_addr, int port, int enable)
{
	u32 val;

	if (enable) {
		val = rs_read32(base_addr + RCEC);
		rs_write32(val | RCEC_RCE | BIT(port), base_addr + RCEC);
	} else {
		val = rs_read32(base_addr + RCDC);
		rs_write32(val | BIT(port), base_addr + RCDC);
	}
}

static int rswitch_etha_change_mode(struct rswitch_etha *etha,
				    enum rswitch_etha_mode mode)
{
	void __iomem *base_addr;
	int ret;

	base_addr = etha->addr - rswitch_etha_offs(etha->index);

	/* Enable clock */
	if (!rswitch_agent_clock_is_enabled(base_addr, etha->index))
		rswitch_agent_clock_ctrl(base_addr, etha->index, 1);

	rs_write32(mode, etha->addr + EAMC);

	ret = rswitch_reg_wait(etha->addr, EAMS, EAMS_OPS_MASK, mode);

	/* Disable clock */
	if (mode == EAMC_OPC_DISABLE)
		rswitch_agent_clock_ctrl(base_addr, etha->index, 0);

	return ret;
}

static void rswitch_etha_read_mac_address(struct rswitch_etha *etha)
{
	u8 *mac = &etha->mac_addr[0];
	u32 mrmac0 = rswitch_etha_read(etha, MRMAC0);
	u32 mrmac1 = rswitch_etha_read(etha, MRMAC1);

	mac[0] = (mrmac0 >>  8) & 0xFF;
	mac[1] = (mrmac0 >>  0) & 0xFF;
	mac[2] = (mrmac1 >> 24) & 0xFF;
	mac[3] = (mrmac1 >> 16) & 0xFF;
	mac[4] = (mrmac1 >>  8) & 0xFF;
	mac[5] = (mrmac1 >>  0) & 0xFF;
}

static bool rswitch_etha_wait_link_verification(struct rswitch_etha *etha)
{
	/* Request Link Verification */
	rswitch_etha_write(etha, MLVC_PLV, MLVC);
	return rswitch_reg_wait(etha->addr, MLVC, MLVC_PLV, 0);
}

static void rswitch_rmac_setting(struct rswitch_etha *etha, const u8 *mac)
{
	u32 val;

	/* FIXME */
	/* Set xMII type */
	switch (etha->speed) {
	case 10:
		val = MPIC_LSC_10M;
		break;
	case 100:
		val = MPIC_LSC_100M;
		break;
	case 1000:
		val = MPIC_LSC_1G;
		break;
	default:
		return;
	}

	rswitch_etha_write(etha, MPIC_PIS_GMII | val, MPIC);

#if 0
	/* Set Interrupt enable */
	rswitch_etha_write(etha, 0, MEIE);
	rswitch_etha_write(etha, 0, MMIE0);
	rswitch_etha_write(etha, 0, MMIE1);
	rswitch_etha_write(etha, 0, MMIE2);
	rswitch_etha_write(etha, 0, MMIE2);
	/* Set Tx function */
	rswitch_etha_write(etha, 0, MTFFC);
	rswitch_etha_write(etha, 0, MTPFC);
	rswitch_etha_write(etha, 0, MTPFC2);
	rswitch_etha_write(etha, 0, MTPFC30);
	rswitch_etha_write(etha, 0, MTATC0);
	/* Set Rx function */
	rswitch_etha_write(etha, 0, MRGC);
	rswitch_etha_write(etha, 0x00070007, MRAFC);
	rswitch_etha_write(etha, 0, MRFSCE);
	rswitch_etha_write(etha, 0, MRFSCP);
	rswitch_etha_write(etha, 0, MTRC);

	/* Set Address Filtering function */
	/* Set XGMII function */
	/* Set Half Duplex function */
	/* Set PLCA function */
#endif
}

static void rswitch_etha_enable_mii(struct rswitch_etha *etha)
{
	rswitch_etha_modify(etha, MPIC, MPIC_PSMCS_MASK | MPIC_PSMHT_MASK,
			    MPIC_PSMCS(0x05) | MPIC_PSMHT(0x06));
	rswitch_etha_modify(etha, MPSM, 0, MPSM_MFF_C45);
}

static int rswitch_etha_hw_init(struct rswitch_etha *etha, const u8 *mac)
{
	int err;

	/* Change to CONFIG Mode */
	err = rswitch_etha_change_mode(etha, EAMC_OPC_DISABLE);
	if (err < 0)
		return err;
	err = rswitch_etha_change_mode(etha, EAMC_OPC_CONFIG);
	if (err < 0)
		return err;

	rs_write32(EAVCC_VEM_SC_TAG, etha->addr + EAVCC);

	rswitch_rmac_setting(etha, mac);
	rswitch_etha_enable_mii(etha);

	/* Change to OPERATION Mode */
	err = rswitch_etha_change_mode(etha, EAMC_OPC_OPERATION);
	if (err < 0)
		return err;

	/* Link Verification */
	return rswitch_etha_wait_link_verification(etha);
}

void rswitch_serdes_write32(void __iomem *addr, u32 offs,  u32 bank, u32 data)
{
	iowrite32(bank, addr + RSWITCH_SERDES_BANK_SELECT);
	iowrite32(data, addr + offs);
}

u32 rswitch_serdes_read32(void __iomem *addr, u32 offs,  u32 bank)
{
	iowrite32(bank, addr + RSWITCH_SERDES_BANK_SELECT);
	return ioread32(addr + offs);
}

static int rswitch_serdes_reg_wait(void __iomem *addr, u32 offs, u32 bank, u32 mask, u32 expected)
{
	int i;

	iowrite32(bank, addr + RSWITCH_SERDES_BANK_SELECT);
	mdelay(1);

	for (i = 0; i < RSWITCH_TIMEOUT_MS; i++) {
		if ((ioread32(addr + offs) & mask) == expected)
			return 0;
		mdelay(1);
	}

	return -ETIMEDOUT;
}

static int rswitch_serdes_common_init_ram(struct rswitch_etha *etha)
{
	void __iomem *common_addr = etha->serdes_addr - etha->index * RSWITCH_SERDES_OFFSET;
	int ret;

	ret = rswitch_serdes_reg_wait(etha->serdes_addr, VR_XS_PMA_MP_12G_16G_25G_SRAM, BANK_180,
				      BIT(0), 0x01);
	if (ret)
		return ret;

	rswitch_serdes_write32(common_addr, VR_XS_PMA_MP_12G_16G_25G_SRAM, BANK_180, 0x03);

	return 0;
}

static int rswitch_serdes_common_setting(struct rswitch_etha *etha, enum rswitch_serdes_mode mode)
{
	void __iomem *addr = etha->serdes_addr - etha->index * RSWITCH_SERDES_OFFSET;

	switch (mode) {
	case SGMII:
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_REF_CLK_CTRL, BANK_180, 0x97);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_MPLLB_CTRL0, BANK_180, 0x60);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_MPLLB_CTRL2, BANK_180, 0x2200);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_MPLLB_CTRL1, BANK_180, 0);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_MPLLB_CTRL3, BANK_180, 0x3d);

		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_serdes_chan_setting(struct rswitch_etha *etha, enum rswitch_serdes_mode mode)
{
	void __iomem *addr = etha->serdes_addr;
	int ret;

	switch (mode) {
	case SGMII:
		rswitch_serdes_write32(addr, VR_XS_PCS_DIG_CTRL1, BANK_380, 0x2000);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_MPLL_CMN_CTRL,
				       BANK_180, 0x11);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_VCO_CAL_LD0, BANK_180, 0x540);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_VCO_CAL_REF0, BANK_180, 0x15);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_RX_GENCTRL1, BANK_180, 0x100);
		rswitch_serdes_write32(addr, VR_XS_PMA_CONSUMER_10G_RX_GENCTRL4, BANK_180, 0);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_TX_RATE_CTRL, BANK_180, 0x02);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_RX_RATE_CTRL, BANK_180, 0x03);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_TX_GENCTRL2, BANK_180, 0x100);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_RX_GENCTRL2, BANK_180, 0x100);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_AFE_DFE_EN_CTRL, BANK_180, 0);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_RX_EQ_CTRL0, BANK_180, 0x07);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_10G_RX_IQ_CTRL0, BANK_180, 0);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_TX_GENCTRL1, BANK_180, 0x310);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_TX_GENCTRL2, BANK_180, 0x101);
		ret = rswitch_serdes_reg_wait(addr, VR_XS_PMA_MP_12G_16G_TX_GENCTRL2,
					      BANK_180, BIT(0), 0);
		if (ret)
			return ret;

		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_RX_GENCTRL2, BANK_180, 0x101);
		ret = rswitch_serdes_reg_wait(addr, VR_XS_PMA_MP_12G_16G_RX_GENCTRL2,
					      BANK_180, BIT(0), 0);
		if (ret)
			return ret;

		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_TX_GENCTRL1,
				       BANK_180, 0x1310);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_TX_EQ_CTRL0,
				       BANK_180, 0x1800);
		rswitch_serdes_write32(addr, VR_XS_PMA_MP_12G_16G_25G_TX_EQ_CTRL1, BANK_180, 0);
		rswitch_serdes_write32(addr, SR_XS_PCS_CTRL2, BANK_300, 0x01);
		rswitch_serdes_write32(addr, VR_XS_PCS_DIG_CTRL1, BANK_380, 0x2100);
		ret = rswitch_serdes_reg_wait(addr, VR_XS_PCS_DIG_CTRL1, BANK_380, BIT(8), 0);
		if (ret)
			return ret;

		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_serdes_set_speed(struct rswitch_etha *etha, enum rswitch_serdes_mode mode,
				    int speed)
{
	void __iomem *addr = etha->serdes_addr;

	switch (mode) {
	case SGMII:
		if (speed == 1000)
			rswitch_serdes_write32(addr, SR_MII_CTRL, BANK_1F00, 0x140);
		else if (speed == 100)
			rswitch_serdes_write32(addr, SR_MII_CTRL, BANK_1F00, 0x2100);

		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int __maybe_unused rswitch_serdes_init(struct rswitch_etha *etha)
{
	int ret;
	enum rswitch_serdes_mode mode;
	void __iomem *common_addr = etha->serdes_addr - etha->index * RSWITCH_SERDES_OFFSET;

	/* TODO: Support more modes */

	switch (etha->phy_interface) {
	case PHY_INTERFACE_MODE_SGMII:
		mode = SGMII;
		break;
	default:
		pr_debug("%s: Don't support this interface", __func__);
		return -EOPNOTSUPP;
	}

	/* Initialize SRAM */
	ret = rswitch_serdes_common_init_ram(etha);
	if (ret)
		return ret;

	ret = rswitch_serdes_reg_wait(etha->serdes_addr, SR_XS_PCS_CTRL1, BANK_300, BIT(15), 0);
	if (ret)
		return ret;

	rswitch_serdes_write32(etha->serdes_addr, 0x03d4, BANK_380, 0x443);

	/* Set common setting */
	ret = rswitch_serdes_common_setting(etha, mode);
	if (ret)
		return ret;

	rswitch_serdes_write32(etha->serdes_addr, 0x03d0, BANK_380, 0x01);

	/* Assert softreset for PHY */
	rswitch_serdes_write32(common_addr, VR_XS_PCS_DIG_CTRL1, BANK_380, 0x8000);

	/* Initialize SRAM */
	ret = rswitch_serdes_common_init_ram(etha);
	if (ret)
		return ret;

	ret = rswitch_serdes_reg_wait(common_addr, VR_XS_PCS_DIG_CTRL1, BANK_380, BIT(15), 0);
	if (ret)
		return ret;

	/* Set channel settings*/
	ret = rswitch_serdes_chan_setting(etha, mode);
	if (ret)
		return ret;

	/* Set speed (bps) */
	ret = rswitch_serdes_set_speed(etha, mode, etha->speed);
	if (ret)
		return ret;

	rswitch_serdes_write32(etha->serdes_addr, 0x03c0, BANK_380, 0);
	rswitch_serdes_write32(etha->serdes_addr, 0x03d0, BANK_380, 0);

	ret = rswitch_serdes_reg_wait(etha->serdes_addr, SR_XS_PCS_STS1, BANK_300, BIT(2), BIT(2));
	if (ret) {
		pr_debug("\n%s: SerDes Link up failed", __func__);
		return ret;
	}

	return 0;
}

static int rswitch_etha_set_access(struct rswitch_etha *etha, bool read,
				   int phyad, int devad, int regad, int data)
{
	int pop = read ? MDIO_READ_C45 : MDIO_WRITE_C45;
	u32 val;
	int ret;

	/* No match device */
	if (devad == 0xffffffff)
		return 0;

	/* Clear completion flags */
	writel(MMIS1_CLEAR_FLAGS, etha->addr + MMIS1);

	/* Submit address to PHY (MDIO_ADDR_C45 << 13) */
	val = MPSM_PSME | MPSM_MFF_C45;
	rs_write32((regad << 16) | (devad << 8) | (phyad << 3) | val, etha->addr + MPSM);

	ret = rswitch_reg_wait(etha->addr, MMIS1, MMIS1_PAACS, MMIS1_PAACS);
	if (ret)
		return ret;

	/* Clear address completion flag */
	rswitch_modify(etha, MMIS1, MMIS1_PAACS, MMIS1_PAACS);

	/* Read/Write PHY register */
	if (read) {
		writel((pop << 13) | (devad << 8) | (phyad << 3) | val, etha->addr + MPSM);

		ret = rswitch_reg_wait(etha->addr, MMIS1, MMIS1_PRACS, MMIS1_PRACS);
		if (ret)
			return ret;

		/* Read data */
		ret = (rs_read32(etha->addr + MPSM) & MPSM_PRD_MASK) >> 16;

		/* Clear read completion flag */
		rswitch_etha_modify(etha, MMIS1, MMIS1_PRACS, MMIS1_PRACS);
	} else {
		rs_write32((data << 16) | (pop << 13) | (devad << 8) | (phyad << 3) | val,
			   etha->addr + MPSM);

		ret = rswitch_reg_wait(etha->addr, MMIS1, MMIS1_PWACS, MMIS1_PWACS);
	}

	return ret;
}

static int rswitch_etha_mii_read(struct mii_bus *bus, int addr, int regnum)
{
	struct rswitch_etha *etha = bus->priv;
	int mode, devad, regad;

	mode = regnum & MII_ADDR_C45;
	devad = (regnum >> MII_DEVADDR_C45_SHIFT) & 0x1f;
	regad = regnum & MII_REGADDR_C45_MASK;

	/* Not support Clause 22 access method */
	if (!mode)
		return 0;

	return rswitch_etha_set_access(etha, true, addr, devad, regad, 0);
}

static int rswitch_etha_mii_write(struct mii_bus *bus, int addr, int regnum, u16 val)
{
	struct rswitch_etha *etha = bus->priv;
	int mode, devad, regad;

	mode = regnum & MII_ADDR_C45;
	devad = (regnum >> MII_DEVADDR_C45_SHIFT) & 0x1f;
	regad = regnum & MII_REGADDR_C45_MASK;

	/* Not support Clause 22 access method */
	if (!mode)
		return 0;

	return rswitch_etha_set_access(etha, false, addr, devad, regad, val);
}

static int rswitch_etha_mii_reset(struct mii_bus *bus)
{
	/* TODO */
	return 0;
}

/* Use of_node_put() on it when done */
static struct device_node *rswitch_get_phy_node(struct rswitch_device *rdev)
{
	struct device_node *ports, *port, *phy = NULL;
	int err = 0;
	u32 index;

	ports = of_get_child_by_name(rdev->ndev->dev.parent->of_node, "ports");
	if (!ports)
		return NULL;

	for_each_child_of_node(ports, port) {
		err = of_property_read_u32(port, "reg", &index);
		if (err < 0)
			return NULL;
		if (index != rdev->etha->index)
			continue;

		/* The default is SGMII interface */
		err = of_get_phy_mode(port, &rdev->etha->phy_interface);
		if (err < 0)
			rdev->etha->phy_interface = PHY_INTERFACE_MODE_SGMII;

		pr_info("%s PHY interface = %s", __func__, phy_modes(rdev->etha->phy_interface));

		phy = of_parse_phandle(port, "phy-handle", 0);
		if (phy) {
			rdev->etha->speed = 1000;
			break;
		} else {
			if (of_phy_is_fixed_link(port)) {
				struct device_node *fixed_link;

				fixed_link = of_get_child_by_name(port, "fixed-link");
				err = of_property_read_u32(fixed_link, "speed", &rdev->etha->speed);
				if (err)
					break;

				err = of_phy_register_fixed_link(port);
				if (err)
					break;

				phy = of_node_get(port);
			}
		}
	}

	of_node_put(ports);

	return phy;
}

static struct device_node *rswitch_get_port_node(struct rswitch_device *rdev)
{
	struct device_node *ports, *port;
	int err = 0;
	u32 index;

	ports = of_get_child_by_name(rdev->ndev->dev.parent->of_node, "ports");
	if (!ports)
		return NULL;

	for_each_child_of_node(ports, port) {
		err = of_property_read_u32(port, "reg", &index);
		if (err < 0)
			return NULL;
		if (index == rdev->etha->index)
			break;
	}

	of_node_put(ports);

	return port;
}

static int rswitch_mii_register(struct rswitch_device *rdev)
{
	struct mii_bus *mii_bus;
	struct device_node *port;
	int err;

	mii_bus = mdiobus_alloc();
	if (!mii_bus)
		return -ENOMEM;

	mii_bus->name = "rswitch_mii";
	sprintf(mii_bus->id, "etha%d", rdev->etha->index);
	mii_bus->priv = rdev->etha;
	mii_bus->read = rswitch_etha_mii_read;
	mii_bus->write = rswitch_etha_mii_write;
	mii_bus->reset = rswitch_etha_mii_reset;
	mii_bus->parent = &rdev->ndev->dev;

	port = rswitch_get_port_node(rdev);
	of_node_get(port);
	err = of_mdiobus_register(mii_bus, port);
	if (err < 0) {
		mdiobus_free(mii_bus);
		goto out;
	}

	rdev->etha->mii = mii_bus;

out:
	of_node_put(port);

	return err;
}

static void rswitch_mii_unregister(struct rswitch_device *rdev)
{
	if (rdev->etha->mii) {
		mdiobus_unregister(rdev->etha->mii);
		mdiobus_free(rdev->etha->mii);
		rdev->etha->mii = NULL;
	}
}

static void rswitch_adjust_link(struct net_device *ndev)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;

	if (phydev->link != rdev->etha->link) {
		phy_print_status(phydev);
		rdev->etha->link = phydev->link;
	}
}

static int rswitch_phy_init(struct rswitch_device *rdev, struct device_node *phy)
{
	struct phy_device *phydev;
	int err = 0;

	phydev = of_phy_connect(rdev->ndev, phy, rswitch_adjust_link, 0,
				rdev->etha->phy_interface);
	if (!phydev) {
		err = -ENOENT;
		goto out;
	}

	phy_attached_info(phydev);

out:
	return err;
}

static void rswitch_phy_deinit(struct rswitch_device *rdev)
{
	if (rdev->ndev->phydev) {
		struct device_node *ports, *port;
		u32 index;

		phy_disconnect(rdev->ndev->phydev);
		rdev->ndev->phydev = NULL;

		ports = of_get_child_by_name(rdev->ndev->dev.parent->of_node, "ports");
		for_each_child_of_node(ports, port) {
			of_property_read_u32(port, "reg", &index);
			if (index == rdev->etha->index)
				break;
		}

		if (of_phy_is_fixed_link(port))
			of_phy_deregister_fixed_link(port);

		of_node_put(ports);
	}
}

static int rswitch_open(struct net_device *ndev)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct device_node *phy;
	int err = 0;
	bool phy_started = false;

	napi_enable(&rdev->napi);

	if (!parallel_mode && rdev->etha) {
		if (!rdev->etha->operated) {
			phy = rswitch_get_phy_node(rdev);
			if (!phy)
				goto error;
			err = rswitch_etha_hw_init(rdev->etha, ndev->dev_addr);
			if (err < 0)
				goto error;
			err = rswitch_mii_register(rdev);
			if (err < 0)
				goto error;
			err = rswitch_phy_init(rdev, phy);
			if (err < 0)
				goto error;

			of_node_put(phy);
		}

		ndev->phydev->speed = rdev->etha->speed;
		phy_set_max_speed(ndev->phydev, rdev->etha->speed);

		phy_start(ndev->phydev);
		phy_started = true;

		if (!rdev->etha->operated) {
			err = rswitch_serdes_init(rdev->etha);
			if (err < 0)
				goto error;
		}

		rdev->etha->operated = true;
	}

	netif_start_queue(ndev);

	/* Enable RX */
	if (!rswitch_is_front_dev(rdev)) {
		rswitch_modify(rdev->addr, GWTRC0, 0, BIT(rdev->rx_default_chain->index));
		rswitch_modify(rdev->addr, GWTRC0, 0, BIT(rdev->rx_learning_chain->index));
	}

	/* Enable interrupt */
	pr_debug("%s: tx = %d, rx = %d\n", __func__, rdev->tx_chain->index, rdev->rx_default_chain->index);
	rswitch_enadis_rdev_irqs(rdev, true);

	if (!rswitch_is_front_dev(rdev))
		rtsn_ptp_init(rdev->priv->ptp_priv, RTSN_PTP_REG_LAYOUT_S4,
			      RTSN_PTP_CLOCK_S4);

out:
	return err;

error:
	if (phy_started)
		phy_stop(ndev->phydev);
	rswitch_phy_deinit(rdev);
	rswitch_mii_unregister(rdev);
	napi_disable(&rdev->napi);
	goto out;
};

static int rswitch_stop(struct net_device *ndev)
{
	struct rswitch_device *rdev = netdev_priv(ndev);

	if (rdev->etha && ndev->phydev)
		phy_stop(ndev->phydev);

	napi_disable(&rdev->napi);

	return 0;
};

static u32 rswitch_search_l3fwd(struct l3_ipv4_fwd_param *param)
{
	struct rswitch_private *priv = param->priv;

	rs_write32(param->frame_type, priv->addr + FWLTHTS0);
	rs_write32(0, priv->addr + FWLTHTS1);
	rs_write32(0, priv->addr + FWLTHTS2);
	rs_write32(param->src_ip, priv->addr + FWLTHTS3);
	rs_write32(param->dst_ip, priv->addr + FWLTHTS4);

	rswitch_reg_wait(priv->addr, FWLTHTSR0, LTHTS, 0);

	return (rs_read32(priv->addr + FWLTHTSR0) & BIT(1));
}

bool is_l3_exist(struct rswitch_private *priv, u32 src_ip, u32 dst_ip)
{
	struct l3_ipv4_fwd_param param = {
		.priv = priv,
		.src_ip = src_ip,
		.dst_ip = dst_ip,
		.frame_type = LTHSLP0v4OTHER,
	};

	return !rswitch_search_l3fwd(&param);
}

static struct rswitch_device *get_dev_by_ip(struct rswitch_private *priv, u32 ip_search, bool use_mask)
{
	struct in_device *ip;
	struct in_ifaddr *in;
	u32 ip_addr, mask;
	int i;

	for (i = 0; i < RSWITCH_NUM_HW; i++) {
		ip = priv->rdev[i]->ndev->ip_ptr;
		if (ip == NULL)
			continue;

		in = ip->ifa_list;
		while (in != NULL) {
			memcpy(&ip_addr, &in->ifa_address, 4);
			memcpy(&mask, &in->ifa_mask, 4);
			ip_addr = be32_to_cpu(ip_addr);
			mask = be32_to_cpu(mask);
			in = in->ifa_next;

			if (use_mask && (ip_search & mask) == (ip_addr & mask))
				return priv->rdev[i];

			if (ip_search == ip_addr)
				return priv->rdev[i];
		}
	}

	return NULL;
}

static int rswitch_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	int ret = NETDEV_TX_OK;
	int entry;
	dma_addr_t dma_addr;
	struct rswitch_ext_desc *desc;
	unsigned long flags;
	struct rswitch_gwca_chain *c = rdev->tx_chain;

	spin_lock_irqsave(&rdev->lock, flags);

	if (c->cur - c->dirty > c->num_ring - 1) {
		netif_stop_subqueue(ndev, 0);
		ret = NETDEV_TX_BUSY;
		goto out;
	}

	if (skb_put_padto(skb, ETH_ZLEN))
		goto out;

	dma_addr = dma_map_single(ndev->dev.parent, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(ndev->dev.parent, dma_addr))
		goto drop;

	entry = c->cur % c->num_ring;
	c->skb[entry] = skb;
	desc = &c->ring[entry];
	desc->dptrl = cpu_to_le32(lower_32_bits(dma_addr));
	desc->dptrh = cpu_to_le32(upper_32_bits(dma_addr));
	desc->info_ds = cpu_to_le16(skb->len);

	if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) {
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		rdev->ts_tag++;
		desc->info1 = (rdev->ts_tag << 8) | BIT(3);
	}

	desc->info1 |= ((u64)rdev->remote_chain << DESC_INFO1_CSD1_SHIFT) |
		((BIT(rdev->port)) << DESC_INFO1_DV_SHIFT) |  DESC_INFO1_FMT;

	skb_tx_timestamp(skb);
	dma_wmb();

	desc->die_dt = DT_FSINGLE | DIE;

	c->cur++;
	rswitch_trigger_chain(rdev->priv, c);

out:
	spin_unlock_irqrestore(&rdev->lock, flags);

	return ret;

drop:
	dev_kfree_skb_any(skb);
	goto out;

}

static struct net_device_stats *rswitch_get_stats(struct net_device *ndev)
{
	return &ndev->stats;
}

static int rswitch_hwstamp_get(struct net_device *ndev, struct ifreq *req)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct rswitch_private *priv = rdev->priv;
	struct rtsn_ptp_private *ptp_priv = priv->ptp_priv;
	struct hwtstamp_config config;

	config.flags = 0;
	config.tx_type = ptp_priv->tstamp_tx_ctrl ? HWTSTAMP_TX_ON :
						    HWTSTAMP_TX_OFF;
	switch (ptp_priv->tstamp_rx_ctrl & RTSN_RXTSTAMP_TYPE) {
	case RTSN_RXTSTAMP_TYPE_V2_L2_EVENT:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		break;
	case RTSN_RXTSTAMP_TYPE_ALL:
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	default:
		config.rx_filter = HWTSTAMP_FILTER_NONE;
		break;
	}

	return copy_to_user(req->ifr_data, &config, sizeof(config)) ? -EFAULT : 0;
}

LIST_HEAD(rswitch_block_cb_list);

static int rswitch_setup_l23_update(struct l23_update_info *l23_info)
{
	u32 update_rule = 0;

	if (l23_info->update_ttl)
		update_rule |= L23UTTLUL;
	if (l23_info->update_dst_mac)
		update_rule |= L23UMDAUL;
	if (l23_info->update_src_mac)
		update_rule |= L23UMSAUL;

	rs_write32(l23_info->routing_number | l23_info->routing_port_valid << 16, l23_info->priv->addr + FWL23URL0);
	rs_write32(l23_info->dst_mac[0] << 8 | l23_info->dst_mac[1] | update_rule, l23_info->priv->addr + FWL23URL1);
	rs_write32(l23_info->dst_mac[2] << 24 | l23_info->dst_mac[3] << 16 | l23_info->dst_mac[4] << 8 |
		l23_info->dst_mac[5], l23_info->priv->addr + FWL23URL2);
	rs_write32(0, l23_info->priv->addr + FWL23URL3);

	return rs_read32(l23_info->priv->addr + FWL23URLR);
}

static int rswitch_modify_l3fwd(struct l3_ipv4_fwd_param *param, bool delete)
{
	struct rswitch_private *priv = param->priv;

	if (!delete) {
		if (param->l23_info.update_dst_mac || param->l23_info.update_src_mac ||
			param->l23_info.update_ttl) {
			rswitch_setup_l23_update(&param->l23_info);
		}
	}

	if (delete)
		rs_write32(param->frame_type | LTHED, priv->addr + FWLTHTL0);
	else
		rs_write32(param->frame_type, priv->addr + FWLTHTL0);

	rs_write32(0, priv->addr + FWLTHTL1);
	rs_write32(0, priv->addr + FWLTHTL2);
	rs_write32(param->src_ip, priv->addr + FWLTHTL3);
	rs_write32(param->dst_ip, priv->addr + FWLTHTL4);

	rs_write32(0, priv->addr + FWLTHTL5);
	rs_write32(0, priv->addr + FWLTHTL6);
	rs_write32(param->l23_info.routing_number | LTHRVL | param->slv << 16, priv->addr + FWLTHTL7);
	if (param->enable_sub_dst)
		rs_write32(param->csd, priv->addr + FWLTHTL80 + 4 * RSWITCH_HW_NUM_TO_GWCA_IDX(priv->gwca.index));
	else
		rs_write32(0, priv->addr + FWLTHTL80 + 4 * RSWITCH_HW_NUM_TO_GWCA_IDX(priv->gwca.index));
	rs_write32(param->dv, priv->addr + FWLTHTL9);

	return rswitch_reg_wait(priv->addr, FWLTHTLR, LTHTL, 0);
}

int rswitch_add_l3fwd(struct l3_ipv4_fwd_param *param)
{
	return rswitch_modify_l3fwd(param, false);
}

static enum pf_type rswitch_get_pf_type_by_num(int num)
{
	if (num >= FBFILTER_NUM(0))
		return PF_FOUR_BYTE;
	if (num >= THBFILTER_NUM(0))
		return PF_THREE_BYTE;
	return PF_TWO_BYTE;
}

void rswitch_put_pf(struct l3_ipv4_fwd_param *param)
{
	int i, idx, pf_num;
	enum pf_type type;

	rs_write32(DISABLE_FILTER, param->priv->addr + FWCFCi(param->pf_cascade_index));

	for (i = 0; i < MAX_PF_ENTRIES; i++) {
		pf_num = rs_read32(param->priv->addr + FWCFMCij(param->pf_cascade_index, i)) & 0xff;
		if (!(rs_read32(param->priv->addr + FWCFMCij(param->pf_cascade_index, i)) & ENABLE_FILTER))
			break;

		type = rswitch_get_pf_type_by_num(pf_num);
		if (type == PF_TWO_BYTE) {
			idx = TBWFILTER_IDX(pf_num);
			rs_write32(DISABLE_FILTER, param->priv->addr + FWTWBFVCi(idx));
			rs_write32(DISABLE_FILTER, param->priv->addr + FWTWBFCi(idx));
			clear_bit(idx, param->priv->filters.two_bytes);
		} else if (type == PF_THREE_BYTE) {
			idx = THBFILTER_IDX(pf_num);
			rs_write32(DISABLE_FILTER, param->priv->addr + FWTHBFV0Ci(idx));
			rs_write32(DISABLE_FILTER, param->priv->addr + FWTHBFV1Ci(idx));
			rs_write32(DISABLE_FILTER, param->priv->addr + FWTHBFCi(idx));
			clear_bit(idx, param->priv->filters.three_bytes);
		} else if (type == PF_FOUR_BYTE) {
			idx = FBFILTER_IDX(pf_num);
			rs_write32(DISABLE_FILTER, param->priv->addr + FWFOBFV0Ci(idx));
			rs_write32(DISABLE_FILTER, param->priv->addr + FWFOBFV1Ci(idx));
			rs_write32(DISABLE_FILTER, param->priv->addr + FWFOBFCi(idx));
			clear_bit(idx, param->priv->filters.four_bytes);
		}
		rs_write32(DISABLE_FILTER, param->priv->addr + FWCFMCij(param->pf_cascade_index, i));
	}

	clear_bit(param->pf_cascade_index, param->priv->filters.cascade);
}

int rswitch_remove_l3fwd(struct l3_ipv4_fwd_param *param)
{
	clear_bit(param->l23_info.routing_number, param->priv->l23_routing_number);

	/* Using Perfect filter, reset it */
	if (param->frame_type == LTHSLP0NONE)
		rswitch_put_pf(param);

	return rswitch_modify_l3fwd(param, true);
}

static int rswitch_get_pf_config(struct rswitch_private *priv, struct rswitch_pf_entry *entry)
{
	if (entry->type == PF_TWO_BYTE) {
		entry->pf_idx = get_two_byte_filter(priv);
	} else if (entry->type == PF_THREE_BYTE) {
		entry->pf_idx = get_three_byte_filter(priv);
	} else if (entry->type == PF_FOUR_BYTE) {
		entry->pf_idx = get_four_byte_filter(priv);
	} else {
		return -1;
	}

	if (entry->pf_idx < 0) {
		return -1;
	}

	if (entry->type == PF_TWO_BYTE) {
		entry->cfg0_addr = priv->addr + FWTWBFVCi(entry->pf_idx);
		/* There is no second config register for Two-Byte filter */
		entry->cfg1_addr = 0;
		entry->offs_addr = priv->addr + FWTWBFCi(entry->pf_idx);
		entry->pf_num = TWBFILTER_NUM(entry->pf_idx);
		set_bit(entry->pf_idx, priv->filters.two_bytes);
		return entry->pf_idx;
	} else if (entry->type == PF_THREE_BYTE) {
		entry->cfg0_addr = priv->addr + FWTHBFV0Ci(entry->pf_idx);
		entry->cfg1_addr = priv->addr + FWTHBFV1Ci(entry->pf_idx);
		entry->offs_addr = priv->addr + FWTHBFCi(entry->pf_idx);
		entry->pf_num = THBFILTER_NUM(entry->pf_idx);
		set_bit(entry->pf_idx, priv->filters.three_bytes);
		return entry->pf_idx;
	} else {
		entry->cfg0_addr = priv->addr + FWFOBFV0Ci(entry->pf_idx);
		entry->cfg1_addr = priv->addr + FWFOBFV1Ci(entry->pf_idx);
		entry->offs_addr = priv->addr + FWFOBFCi(entry->pf_idx);
		entry->pf_num = FBFILTER_NUM(entry->pf_idx);
		set_bit(entry->pf_idx, priv->filters.four_bytes);
		return entry->pf_idx;
	}
}

int rswitch_setup_pf(struct rswitch_pf_param *pf_param)
{
	int cascade_idx, i;
	struct rswitch_device *rdev = pf_param->rdev;
	struct rswitch_private *priv = rdev->priv;

	cascade_idx = find_first_zero_bit(priv->filters.cascade, PFL_CADF_N);

	if (cascade_idx == PFL_CADF_N)
		return -1;

	if (pf_param->used_entries > MAX_PF_ENTRIES)
		return -1;

	rs_write32(DISABLE_FILTER, priv->addr + FWCFCi(cascade_idx));

	for (i = 0; i < pf_param->used_entries; i++) {
		/* TODO: Free allocated resources */
		if (rswitch_get_pf_config(priv, &pf_param->entries[i]) < 0)
			return -1;

		/* There is no second config register for Two-Byte filter */
		if (pf_param->entries[i].type == PF_TWO_BYTE) {
			rs_write32(FWTWBFVCVALMASK(pf_param->entries[i].val, pf_param->entries[i].mask),
				pf_param->entries[i].cfg0_addr);
		} else {
			rs_write32(pf_param->entries[i].val, pf_param->entries[i].cfg0_addr);
			rs_write32(~pf_param->entries[i].mask, pf_param->entries[i].cfg1_addr);
		}

		rs_write32(MASK_MODE | SNOOPING_BUS_OFFSET(pf_param->entries[i].off), pf_param->entries[i].offs_addr);
		rs_write32(pf_param->entries[i].pf_num | ENABLE_FILTER, priv->addr + FWCFMCij(cascade_idx, i));
	}

	if (pf_param->all_sources) {
		rs_write32(0x000f007f, priv->addr + FWCFCi(cascade_idx));
	} else {
		rs_write32(0x000f0000 | BIT(rdev->port), priv->addr + FWCFCi(cascade_idx));
	}

	set_bit(cascade_idx, priv->filters.cascade);

	return cascade_idx;
}

int rswitch_rn_get(struct rswitch_private *priv)
{
	int index;

	index = find_first_zero_bit(priv->l23_routing_number, RSWITCH_MAX_NUM_L23);
	set_bit(index, priv->l23_routing_number);

	return index;
}

static int rswitch_setup_tc_matchall(struct net_device *dev,
		struct tc_cls_matchall_offload *cls_matchall)
{
	return -EOPNOTSUPP;
}

static int rswitch_setup_tc_block_cb(enum tc_setup_type type,
		void *type_data,
		void *cb_priv)
{
	struct net_device *dev = cb_priv;

	switch (type) {
		case TC_SETUP_CLSU32:
			return rswitch_setup_tc_cls_u32(dev, type_data);
		case TC_SETUP_CLSFLOWER:
			return rswitch_setup_tc_flower(dev, type_data);
		case TC_SETUP_CLSMATCHALL:
			return rswitch_setup_tc_matchall(dev, type_data);
		default:
			return -EOPNOTSUPP;
	}

	return 0;
}

static int rswitch_setup_tc_block_bind(struct rswitch_device *rdev,
		struct flow_block_offload *f, bool ingress)
{
	return flow_block_cb_setup_simple(f, &rswitch_block_cb_list,
		rswitch_setup_tc_block_cb, rdev, rdev->ndev, ingress);
}

static int rswitch_setup_tc_block_unbind(struct rswitch_device *rdev,
		struct flow_block_offload *f, bool ingress)
{
	return 0;
}

static int rswitch_setup_tc_block_clsact(struct rswitch_device *rdev,
		struct flow_block_offload *f, bool ingress)
{
	f->driver_block_list = &rswitch_block_cb_list;

	switch (f->command) {
		case FLOW_BLOCK_BIND:
			return rswitch_setup_tc_block_bind(rdev, f, ingress);
		case FLOW_BLOCK_UNBIND:
			return rswitch_setup_tc_block_unbind(rdev, f, ingress);
		default:
			return -EOPNOTSUPP;
	}
}

static int rswitch_setup_tc_block(struct rswitch_device *rdev,
		struct flow_block_offload *f)
{
	switch (f->binder_type) {
		case FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS:
			return rswitch_setup_tc_block_clsact(rdev, f, true);
		case FLOW_BLOCK_BINDER_TYPE_CLSACT_EGRESS:
			return rswitch_setup_tc_block_clsact(rdev, f, false);
		default:
			return -EOPNOTSUPP;
	}
}

static int rswitch_setup_tc(struct net_device *ndev, enum tc_setup_type type,
		void *type_data)
{
	struct rswitch_device *rdev = netdev_priv(ndev);

	switch (type) {
		case TC_SETUP_BLOCK:
			return rswitch_setup_tc_block(rdev, type_data);
		default:
			return -EOPNOTSUPP;
	}
}

static int rswitch_hwstamp_set(struct net_device *ndev, struct ifreq *req)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	struct rswitch_private *priv = rdev->priv;
	struct rtsn_ptp_private *ptp_priv = priv->ptp_priv;
	struct hwtstamp_config config;
	u32 tstamp_rx_ctrl = RTSN_RXTSTAMP_ENABLED;
	u32 tstamp_tx_ctrl;

	if (copy_from_user(&config, req->ifr_data, sizeof(config)))
		return -EFAULT;

	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		tstamp_tx_ctrl = 0;
		break;
	case HWTSTAMP_TX_ON:
		tstamp_tx_ctrl = RTSN_TXTSTAMP_ENABLED;
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		tstamp_rx_ctrl = 0;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
		tstamp_rx_ctrl |= RTSN_RXTSTAMP_TYPE_V2_L2_EVENT;
		break;
	default:
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		tstamp_rx_ctrl |= RTSN_RXTSTAMP_TYPE_ALL;
		break;
	}

	ptp_priv->tstamp_tx_ctrl = tstamp_tx_ctrl;
	ptp_priv->tstamp_rx_ctrl = tstamp_rx_ctrl;

	return copy_to_user(req->ifr_data, &config, sizeof(config)) ? -EFAULT : 0;
}

static int rswitch_do_ioctl(struct net_device *ndev, struct ifreq *req, int cmd)
{
	if (!netif_running(ndev))
		return -EINVAL;

	switch (cmd) {
	case SIOCGHWTSTAMP:
		return rswitch_hwstamp_get(ndev, req);
	case SIOCSHWTSTAMP:
		return rswitch_hwstamp_set(ndev, req);
	default:
		break;
	}

	return 0;
}

static int rswitch_port_get_port_parent_id(struct net_device *ndev,
					  struct netdev_phys_item_id *ppid)
{
	struct rswitch_device *rdev = netdev_priv(ndev);

	ppid->id_len = sizeof(rdev->priv->dev_id);
	memcpy(&ppid->id, &rdev->priv->dev_id, ppid->id_len);

	return 0;
}

const struct net_device_ops rswitch_netdev_ops = {
	.ndo_open = rswitch_open,
	.ndo_stop = rswitch_stop,
	.ndo_start_xmit = rswitch_start_xmit,
	.ndo_get_stats = rswitch_get_stats,
	.ndo_do_ioctl = rswitch_do_ioctl,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_get_port_parent_id = rswitch_port_get_port_parent_id,
	.ndo_setup_tc           = rswitch_setup_tc,
//	.ndo_change_mtu = eth_change_mtu,
};

static int rswitch_add_ipv4_dst_route(struct rswitch_ipv4_route *routing_list, struct rswitch_device *dev, u32 ip)
{
	struct rswitch_private *priv = dev->priv;
	struct l3_ipv4_fwd_param_list *param_list;
	struct rswitch_pf_param pf_param;
	int ret = 0;

	param_list = kzalloc(sizeof(*param_list), GFP_KERNEL);
	if (!param_list)
		return -ENOMEM;

	param_list->param = kzalloc(sizeof(struct l3_ipv4_fwd_param), GFP_KERNEL);
	if (!param_list->param) {
		ret = -ENOMEM;
		goto free_param_list;
	}

	pf_param.rdev = dev;
	pf_param.all_sources = true;
	pf_param.used_entries = 1;
	pf_param.entries[0].val = ip;
	pf_param.entries[0].mask = 0xffffffff;
	pf_param.entries[0].off = IPV4_DST_OFFSET;
	pf_param.entries[0].type = PF_FOUR_BYTE;

	param_list->param->pf_cascade_index = rswitch_setup_pf(&pf_param);
	if (param_list->param->pf_cascade_index < 0)
		goto free_param;
	param_list->param->priv = priv;
	param_list->param->dv = BIT(priv->gwca.index);
	param_list->param->slv = 0x3F;
	param_list->param->csd = dev->rx_default_chain->index;
	param_list->param->frame_type = LTHSLP0NONE;
	param_list->param->enable_sub_dst = true;
	param_list->param->l23_info.priv = priv;
	param_list->param->l23_info.update_ttl = true;
	param_list->param->l23_info.update_dst_mac = true;
	param_list->param->l23_info.routing_port_valid = 0x3F;
	param_list->param->l23_info.routing_number = rswitch_rn_get(priv);
	memcpy(param_list->param->l23_info.dst_mac, dev->ndev->dev_addr, ETH_ALEN);

	ret = rswitch_add_l3fwd(param_list->param);
	if (ret)
		goto put_pf;

	list_add(&param_list->list, &routing_list->param_list);

	return ret;

put_pf:
	rswitch_put_pf(param_list->param);
free_param:
	kfree(param_list->param);
free_param_list:
	kfree(param_list);
	return ret;
}

static void rswitch_fib_event_add(struct rswitch_fib_event_work *fib_work)
{
	struct fib_entry_notifier_info fen;
	struct rswitch_ipv4_route *new_routing_list;
	struct rswitch_device *dev;
	struct fib_nh *nh;

	fen = fib_work->fen_info;
	nh = fib_info_nh(fen.fi, 0);

	if (fen.type != RTN_UNICAST)
		return;

	dev = get_dev_by_ip(fib_work->priv, be32_to_cpu(nh->nh_saddr), false);
	if (!dev)
		return;

	new_routing_list = kzalloc(sizeof(*new_routing_list), GFP_KERNEL);
	if (!new_routing_list)
		return;

	new_routing_list->ip = be32_to_cpu(nh->nh_saddr);
	new_routing_list->mask = be32_to_cpu(inet_make_mask(fen.dst_len));
	new_routing_list->subnet = fen.dst;
	new_routing_list->dev = dev;
	INIT_LIST_HEAD(&new_routing_list->param_list);

	list_add(&new_routing_list->list, &dev->routing_list);

	if (!rswitch_add_ipv4_dst_route(new_routing_list, dev, be32_to_cpu(nh->nh_saddr)))
		nh->fib_nh_flags |= RTNH_F_OFFLOAD;
}

static void rswitch_fib_event_remove(struct rswitch_fib_event_work *fib_work)
{
	struct fib_entry_notifier_info fen;
	struct rswitch_device *dev;
	struct fib_nh *nh;
	struct list_head *cur, *tmp;
	struct rswitch_ipv4_route *routing_list;
	struct l3_ipv4_fwd_param_list *param_list;
	bool route_found = false;

	fen = fib_work->fen_info;
	nh = fib_info_nh(fen.fi, 0);

	if (fen.type != RTN_UNICAST)
		return;

	dev = get_dev_by_ip(fib_work->priv, be32_to_cpu(nh->nh_saddr), false);
	if (!dev)
		return;

	list_for_each(cur, &dev->routing_list) {
		routing_list = list_entry(cur, struct rswitch_ipv4_route, list);
		if (routing_list->subnet == fen.dst && routing_list->ip == be32_to_cpu(nh->nh_saddr)) {
			route_found = true;
			break;
		}
	}

	/* There is nothing to free */
	if (!route_found)
		return;

	list_for_each_safe(cur, tmp, &routing_list->param_list) {
		param_list = list_entry(cur, struct l3_ipv4_fwd_param_list, list);
		rswitch_remove_l3fwd(param_list->param);
		list_del(cur);
		kfree(param_list->param);
		kfree(param_list);
	}

	list_del(&routing_list->list);
	kfree(routing_list);
}

static void rswitch_fib_event_work(struct work_struct *work)
{
	struct rswitch_fib_event_work *fib_work =
		container_of(work, struct rswitch_fib_event_work, work);

	/* Protect internal structures from changes */
	rtnl_lock();
	switch (fib_work->event) {
		case FIB_EVENT_ENTRY_REPLACE:
			rswitch_fib_event_add(fib_work);
			fib_info_put(fib_work->fen_info.fi);
			break;
		case FIB_EVENT_ENTRY_DEL:
			rswitch_fib_event_remove(fib_work);
			fib_info_put(fib_work->fen_info.fi);
			break;
	}

	rtnl_unlock();
	kfree(fib_work);
}

/* Called with rcu_read_lock() */
static int rswitch_fib_event(struct notifier_block *nb,
		unsigned long event, void *ptr)
{
	struct rswitch_private *priv = container_of(nb, struct rswitch_private, fib_nb);
	struct fib_notifier_info *info = ptr;
	struct rswitch_fib_event_work *fib_work;

	/* Handle only IPv4 routes */
	if (info->family != AF_INET)
		return NOTIFY_DONE;

	fib_work = kzalloc(sizeof(*fib_work), GFP_ATOMIC);
	if (WARN_ON(!fib_work))
		return NOTIFY_BAD;

	fib_work->event = event;
	fib_work->priv = priv;

	INIT_WORK(&fib_work->work, rswitch_fib_event_work);

	switch (event) {
		case FIB_EVENT_ENTRY_ADD:
		case FIB_EVENT_ENTRY_APPEND:
		case FIB_EVENT_ENTRY_DEL:
		case FIB_EVENT_ENTRY_REPLACE:
			if (info->family == AF_INET) {
				struct fib_entry_notifier_info *fen_info = ptr;

				if (fen_info->fi->fib_nh_is_v6) {
					NL_SET_ERR_MSG_MOD(info->extack, "IPv6 gateway with IPv4 route is not supported");
					kfree(fib_work);
					return notifier_from_errno(-EINVAL);
				}
				if (fen_info->fi->nh) {
					NL_SET_ERR_MSG_MOD(info->extack, "IPv4 route with nexthop objects is not supported");
					kfree(fib_work);
					return notifier_from_errno(-EINVAL);
				}
			}

			memcpy(&fib_work->fen_info, ptr, sizeof(fib_work->fen_info));
			/* Take referece on fib_info to prevent it from being
			   +		 * freed while work is queued. Release it afterwards.
			   +		 */
			fib_info_hold(fib_work->fen_info.fi);
			break;
	}

	queue_work(priv->rswitch_fib_wq, &fib_work->work);

	return NOTIFY_DONE;
}

static __net_init int rswitch_init_net(struct net *net)
{
	struct rswitch_net *rn_init = net_generic(&init_net, rswitch_net_id);

	/* Notifier for initial network is already registered */
	if (net == &init_net)
		return 0;

	rn_init->priv->fib_nb.notifier_call = rswitch_fib_event;
	return register_fib_notifier(net, &rn_init->priv->fib_nb, NULL, NULL);
}

static void __net_exit rswitch_exit_net(struct net *net)
{
	struct rswitch_net *rn_init = net_generic(&init_net, rswitch_net_id);

	if (net == &init_net)
		return;

	unregister_fib_notifier(net, &rn_init->priv->fib_nb);
}

struct pernet_operations rswitch_net_ops = {
	.init = rswitch_init_net,
	.exit = rswitch_exit_net,
	.id   = &rswitch_net_id,
	.size = sizeof(struct rswitch_net),
};

static int rswitch_get_ts_info(struct net_device *ndev, struct ethtool_ts_info *info)
{
	struct rswitch_device *rdev = netdev_priv(ndev);

	info->phc_index = ptp_clock_index(rdev->priv->ptp_priv->clock);
	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE |
				SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);
	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL);

	return 0;
}

static const struct ethtool_ops rswitch_ethtool_ops = {
	.get_ts_info = rswitch_get_ts_info,
};

static const struct of_device_id renesas_eth_sw_of_table[] = {
	{ .compatible = "renesas,etherswitch", },
	{ }
};
MODULE_DEVICE_TABLE(of, renesas_eth_sw_of_table);

static void rswitch_clock_enable(struct rswitch_private *priv)
{
	rs_write32(GENMASK(RSWITCH_NUM_HW - 1, 0) | RCEC_RCE, priv->addr + RCEC);
}

static void rswitch_reset(struct rswitch_private *priv)
{
	if (!parallel_mode) {
		rs_write32(RRC_RR, priv->addr + RRC);
		rs_write32(RRC_RR_CLR, priv->addr + RRC);

		reset_control_assert(priv->sd_rst);
		mdelay(1);
		reset_control_deassert(priv->sd_rst);
	} else {
		int gwca_idx;
		u32 gwro_offset;
		int mode;
		int count;

		if (priv->gwca.index == RSWITCH_GWCA_IDX_TO_HW_NUM(0)) {
			gwca_idx = 1;
			gwro_offset = RSWITCH_GWCA1_OFFSET;
		} else {
			gwca_idx = 0;
			gwro_offset = RSWITCH_GWCA0_OFFSET;
		}

		count = 0;
		do {
			mode = rs_read32(priv->addr + gwro_offset + 0x0004) & GWMS_OPS_MASK;
			if (mode == GWMC_OPC_OPERATION)
				break;

			count++;
			if (!(count % 100))
				pr_info(" rswitch wait for GWMS%d %d==%d\n", gwca_idx, mode,
					GWMC_OPC_OPERATION);

			mdelay(10);
		} while (1);
	}
}

static void rswitch_etha_init(struct rswitch_private *priv, int index)
{
	struct rswitch_etha *etha = &priv->etha[index];

	memset(etha, 0, sizeof(*etha));
	etha->index = index;
	etha->addr = priv->addr + rswitch_etha_offs(index);
	etha->serdes_addr = priv->serdes_addr + index * RSWITCH_SERDES_OFFSET;
}

static int rswitch_gwca_change_mode(struct rswitch_private *priv,
				    enum rswitch_gwca_mode mode)
{
	int ret;

	/* Enable clock */
	if (!rswitch_agent_clock_is_enabled(priv->addr, priv->gwca.index))
		rswitch_agent_clock_ctrl(priv->addr, priv->gwca.index, 1);

	rs_write32(mode, priv->addr + GWMC);

	ret = rswitch_reg_wait(priv->addr, GWMS, GWMS_OPS_MASK, mode);

	/* Disable clock */
	if (mode == GWMC_OPC_DISABLE)
		rswitch_agent_clock_ctrl(priv->addr, priv->gwca.index, 0);

	return ret;
}

static int rswitch_gwca_mcast_table_reset(struct rswitch_private *priv)
{
	rs_write32(GWMTIRM_MTIOG, priv->addr + GWMTIRM);
	return rswitch_reg_wait(priv->addr, GWMTIRM, GWMTIRM_MTR, GWMTIRM_MTR);
}

static int rswitch_gwca_axi_ram_reset(struct rswitch_private *priv)
{
	rs_write32(GWARIRM_ARIOG, priv->addr + GWARIRM);
	return rswitch_reg_wait(priv->addr, GWARIRM, GWARIRM_ARR, GWARIRM_ARR);
}

static int rswitch_gwca_hw_init(struct rswitch_private *priv)
{
	int err;

	err = rswitch_gwca_change_mode(priv, GWMC_OPC_DISABLE);
	if (err < 0)
		return err;
	err = rswitch_gwca_change_mode(priv, GWMC_OPC_CONFIG);
	if (err < 0)
		return err;
	err = rswitch_gwca_mcast_table_reset(priv);
	if (err < 0)
		return err;
	err = rswitch_gwca_axi_ram_reset(priv);
	if (err < 0)
		return err;

	/* Full setting flow */
	rs_write32(GWVCC_VEM_SC_TAG, priv->addr + GWVCC);
	rs_write32(0, priv->addr + GWTTFC);
	rs_write32(lower_32_bits(priv->desc_bat_dma), priv->addr + GWDCBAC1);
	rs_write32(upper_32_bits(priv->desc_bat_dma), priv->addr + GWDCBAC0);

	err = rswitch_gwca_change_mode(priv, GWMC_OPC_DISABLE);
	if (err < 0)
		return err;
	err = rswitch_gwca_change_mode(priv, GWMC_OPC_OPERATION);
	if (err < 0)
		return err;

	return 0;
}

static void rswitch_gwca_chain_free(struct net_device *ndev,
				    struct rswitch_private *priv,
				    struct rswitch_gwca_chain *c)
{
	int i;

	if (c->gptp) {
		dma_free_coherent(ndev->dev.parent,
				  sizeof(struct rswitch_ext_ts_desc) *
				  (c->num_ring + 1), c->ts_ring, c->ring_dma);
		c->ts_ring = NULL;
	} else {
		dma_free_coherent(ndev->dev.parent,
				  sizeof(struct rswitch_desc) *
				  (c->num_ring + 1), c->ring, c->ring_dma);
		c->ring = NULL;
	}

	if (!c->dir_tx) {
		for (i = 0; i < c->num_ring; i++)
			dev_kfree_skb(c->skb[i]);
	}

	kfree(c->skb);
	c->skb = NULL;
}

static int rswitch_gwca_chain_init(struct net_device *ndev,
				struct rswitch_private *priv,
				struct rswitch_gwca_chain *c,
				bool dir_tx, bool gptp, int num_ring)
{
	int i;
	int index = c->index;	/* Keep the index before memset() */
	struct sk_buff *skb;

	memset(c, 0, sizeof(*c));
	c->index = index;
	c->dir_tx = dir_tx;
	c->gptp = gptp;
	c->num_ring = num_ring;
	c->ndev = ndev;

	c->skb = kcalloc(c->num_ring, sizeof(*c->skb), GFP_KERNEL);
	if (!c->skb)
		return -ENOMEM;

	if (!dir_tx) {
		for (i = 0; i < c->num_ring; i++) {
			skb = dev_alloc_skb(PKT_BUF_SZ + RSWITCH_ALIGN - 1);
			if (!skb)
				goto out;
			skb_reserve(skb, NET_IP_ALIGN);
			c->skb[i] = skb;
		}
	}

	if (gptp)
		c->ts_ring = dma_alloc_coherent(ndev->dev.parent,
				sizeof(struct rswitch_ext_ts_desc) *
				(c->num_ring + 1), &c->ring_dma, GFP_KERNEL);
	else
		c->ring = dma_alloc_coherent(ndev->dev.parent,
				sizeof(struct rswitch_ext_desc) *
				(c->num_ring + 1), &c->ring_dma, GFP_KERNEL);
	if (!c->ts_ring && !c->ring)
		goto out;

	return 0;

out:
	rswitch_gwca_chain_free(ndev, priv, c);

	return -ENOMEM;
}

void rswitch_gwca_chain_register(struct rswitch_private *priv,
				 struct rswitch_gwca_chain *c, bool ts)
{
	struct rswitch_desc *desc;
	int bit;
	int index;

	desc = &priv->desc_bat[c->index];
	desc->die_dt = DT_LINKFIX;
	desc->dptrl = cpu_to_le32(lower_32_bits(c->ring_dma));
	desc->dptrh = cpu_to_le32(upper_32_bits(c->ring_dma));

	index = c->index / 32;
	bit = BIT(c->index % 32);

	if (!priv->addr)
		return;

	if (c->dir_tx)
		priv->gwca.tx_irq_bits[index] |= bit;
	else
		priv->gwca.rx_irq_bits[index] |= bit;

	/* FIXME: GWDCC_DCP */
	rs_write32(GWDCC_BALR | (c->dir_tx ? GWDCC_DQT : 0) |
		   (ts ? GWDCC_ETS : 0) |
		   GWDCC_EDE |
		   GWDCC_OSID(c->osid),
		   priv->addr + GWDCC_OFFS(c->index));
}

static int rswitch_gwca_chain_format(struct net_device *ndev,
				struct rswitch_private *priv,
				struct rswitch_gwca_chain *c)
{
	struct rswitch_ext_desc *ring;
	int tx_ring_size = sizeof(*ring) * c->num_ring;
	int i;
	dma_addr_t dma_addr;

	memset(c->ring, 0, tx_ring_size);
	for (i = 0, ring = c->ring; i < c->num_ring; i++, ring++) {
		if (!c->dir_tx) {
			dma_addr = dma_map_single(ndev->dev.parent,
					c->skb[i]->data, PKT_BUF_SZ,
					DMA_FROM_DEVICE);
			if (!dma_mapping_error(ndev->dev.parent, dma_addr))
				ring->info_ds = cpu_to_le16(PKT_BUF_SZ);
			ring->dptrl = cpu_to_le32(lower_32_bits(dma_addr));
			ring->dptrh = cpu_to_le32(upper_32_bits(dma_addr));
			ring->die_dt = DT_FEMPTY | DIE;
		} else {
			ring->die_dt = DT_EEMPTY | DIE;
		}
	}
	ring->dptrl = cpu_to_le32(lower_32_bits(c->ring_dma));
	ring->dptrh = cpu_to_le32(upper_32_bits(c->ring_dma));
	ring->die_dt = DT_LINKFIX;

	rswitch_gwca_chain_register(priv, c, false);

	return 0;
}

static int rswitch_gwca_chain_ts_format(struct net_device *ndev,
					struct rswitch_private *priv,
					struct rswitch_gwca_chain *c)
{
	struct rswitch_ext_ts_desc *ring;
	int tx_ts_ring_size = sizeof(*ring) * c->num_ring;
	int i;
	dma_addr_t dma_addr;

	memset(c->ts_ring, 0, tx_ts_ring_size);
	for (i = 0, ring = c->ts_ring; i < c->num_ring; i++, ring++) {
		if (!c->dir_tx) {
			dma_addr = dma_map_single(ndev->dev.parent,
					c->skb[i]->data, PKT_BUF_SZ,
					DMA_FROM_DEVICE);
			if (!dma_mapping_error(ndev->dev.parent, dma_addr))
				ring->info_ds = cpu_to_le16(PKT_BUF_SZ);
			ring->dptrl = cpu_to_le32(lower_32_bits(dma_addr));
			ring->dptrh = cpu_to_le32(upper_32_bits(dma_addr));
			ring->die_dt = DT_FEMPTY | DIE;
		} else {
			ring->die_dt = DT_EEMPTY | DIE;
		}
	}
	ring->dptrl = cpu_to_le32(lower_32_bits(c->ring_dma));
	ring->dptrh = cpu_to_le32(upper_32_bits(c->ring_dma));
	ring->die_dt = DT_LINKFIX;

	rswitch_gwca_chain_register(priv, c, true);

	return 0;
}

int rswitch_desc_alloc(struct rswitch_private *priv)
{
	struct device *dev = &priv->pdev->dev;
	int i, num_chains = priv->gwca.num_chains;
	struct resource r;
	struct device_node *node;
	int ret;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);

	of_node_put(node);

	if (ret)
		return ret;

	priv->desc_bat_size = sizeof(struct rswitch_desc) * num_chains;
	priv->desc_bat_dma = r.start;
	priv->desc_bat = memremap(r.start, resource_size(&r), MEMREMAP_WB);

	if (!priv->desc_bat)
		return -ENOMEM;
	for (i = 0; i < num_chains; i++)
		priv->desc_bat[i].die_dt = DT_EOS;

	return 0;
}

void rswitch_desc_free(struct rswitch_private *priv)
{
	if (priv->desc_bat)
		memunmap(priv->desc_bat);
	priv->desc_bat = NULL;
}

struct rswitch_gwca_chain *rswitch_gwca_get(struct rswitch_private *priv)
{
	int index;

	index = find_first_zero_bit(priv->gwca.used, priv->gwca.num_chains);
	if (index >= priv->gwca.num_chains)
		return NULL;
	set_bit(index, priv->gwca.used);
	priv->gwca.chains[index].index = index;

	return &priv->gwca.chains[index];
}

void rswitch_gwca_put(struct rswitch_private *priv,
		      struct rswitch_gwca_chain *c)
{
	clear_bit(c->index, priv->gwca.used);
}

int rswitch_txdmac_init(struct net_device *ndev, struct rswitch_private *priv,
			int chain_num)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	int err;

	if (chain_num < 0)
	{
		rdev->tx_chain = rswitch_gwca_get(priv);
		if (!rdev->tx_chain)
			return -EBUSY;
	}
	else
	{
		rdev->tx_chain = devm_kzalloc(ndev->dev.parent, sizeof(*rdev->tx_chain),
					      GFP_KERNEL);
		if (!rdev->tx_chain)
			return -ENOMEM;
		rdev->tx_chain->index = chain_num;
	}

	err = rswitch_gwca_chain_init(ndev, priv, rdev->tx_chain, true, false,
				      TX_RING_SIZE);
	if (err < 0)
		goto out_init;

	err = rswitch_gwca_chain_format(ndev, priv, rdev->tx_chain);
	if (err < 0)
		goto out_format;

	return 0;

out_format:
	rswitch_gwca_chain_free(ndev, priv, rdev->tx_chain);

out_init:
	if (priv)
		rswitch_gwca_put(priv, rdev->tx_chain);

	return err;
}

void rswitch_txdmac_free(struct net_device *ndev,
			 struct rswitch_private *priv)
{
	struct rswitch_device *rdev = netdev_priv(ndev);

	rswitch_gwca_chain_free(ndev, priv, rdev->tx_chain);
	rswitch_gwca_put(priv, rdev->tx_chain);
}

int rswitch_rxdmac_init(struct net_device *ndev, struct rswitch_private *priv,
			int chain_num)
{
	struct rswitch_device *rdev = netdev_priv(ndev);
	int err;

	if (chain_num < 0)
	{
		rdev->rx_default_chain = rswitch_gwca_get(priv);
		if (!rdev->rx_default_chain)
			return -EBUSY;
		rdev->rx_learning_chain = rswitch_gwca_get(priv);
		if (!rdev->rx_learning_chain)
			goto put_default;
	}
	else
	{
		rdev->rx_default_chain = devm_kzalloc(ndev->dev.parent, sizeof(*rdev->rx_default_chain),
					      GFP_KERNEL);
		if (!rdev->rx_default_chain)
			return -ENOMEM;
		rdev->rx_default_chain->index = chain_num;
		/* TODO need to init rdev->rx_learning_chain */
	}

	err = rswitch_gwca_chain_init(ndev, priv, rdev->rx_default_chain, false, true,
				      RX_RING_SIZE);
	if (err < 0)
		goto put_learning;
	err = rswitch_gwca_chain_init(ndev, priv, rdev->rx_learning_chain, false, true,
				      RX_RING_SIZE);
	if (err < 0)
		goto free_default;

	err = rswitch_gwca_chain_ts_format(ndev, priv, rdev->rx_default_chain);
	if (err < 0)
		goto free_learning;
	err = rswitch_gwca_chain_ts_format(ndev, priv, rdev->rx_learning_chain);
	if (err < 0)
		goto free_learning;

	return 0;

free_learning:
	rswitch_gwca_chain_free(ndev, priv, rdev->rx_learning_chain);
free_default:
	rswitch_gwca_chain_free(ndev, priv, rdev->rx_default_chain);
put_learning:
	rswitch_gwca_put(priv, rdev->rx_learning_chain);
put_default:
	rswitch_gwca_put(priv, rdev->rx_default_chain);

	return err;
}

void rswitch_rxdmac_free(struct net_device *ndev, struct rswitch_private *priv)
{
	struct rswitch_device *rdev = netdev_priv(ndev);

	rswitch_gwca_chain_free(ndev, priv, rdev->rx_default_chain);
	rswitch_gwca_chain_free(ndev, priv, rdev->rx_learning_chain);
	rswitch_gwca_put(priv, rdev->rx_default_chain);
	rswitch_gwca_put(priv, rdev->rx_learning_chain);
}

static void rswitch_set_mac_address(struct rswitch_device *rdev)
{
	struct net_device *ndev = rdev->ndev;
	struct device_node *ports, *port;
	u32 index;
	const u8 *mac;

	ports = of_get_child_by_name(ndev->dev.parent->of_node, "ports");

	for_each_child_of_node(ports, port) {
		of_property_read_u32(port, "reg", &index);
		if (index == rdev->etha->index)
			break;
	}

	mac = of_get_mac_address(port);
	if (!IS_ERR(mac))
		ether_addr_copy(ndev->dev_addr, mac);

	if (!is_valid_ether_addr(ndev->dev_addr))
		ether_addr_copy(ndev->dev_addr, rdev->etha->mac_addr);

	if (!is_valid_ether_addr(ndev->dev_addr))
		eth_hw_addr_random(ndev);

	of_node_put(ports);
}

static int rswitch_ndev_create(struct rswitch_private *priv, int index)
{
	struct platform_device *pdev = priv->pdev;
	struct net_device *ndev;
	struct rswitch_device *rdev;
	int err;

	ndev = alloc_etherdev_mqs(sizeof(struct rswitch_device), 1, 1);
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, &pdev->dev);
	ether_setup(ndev);

	rdev = netdev_priv(ndev);
	rdev->ndev = ndev;
	rdev->priv = priv;
	INIT_LIST_HEAD(&rdev->routing_list);
	INIT_LIST_HEAD(&rdev->tc_u32_list);
	INIT_LIST_HEAD(&rdev->tc_flower_list);
	priv->rdev[index] = rdev;
	/* TODO: netdev instance : ETHA port is 1:1 mapping */
	if (index < RSWITCH_MAX_NUM_ETHA) {
		rdev->port = index;
		rdev->etha = &priv->etha[index];
	} else {
		rdev->port = -1;
		rdev->etha = NULL;
	}
	rdev->remote_chain = 0;
	rdev->addr = priv->addr;

	spin_lock_init(&rdev->lock);

	ndev->features = NETIF_F_RXCSUM;
	ndev->hw_features = NETIF_F_RXCSUM;
	ndev->base_addr = (unsigned long)rdev->addr;
	snprintf(ndev->name, IFNAMSIZ, "tsn%d", index);
	ndev->netdev_ops = &rswitch_netdev_ops;
	ndev->ethtool_ops = &rswitch_ethtool_ops;

	netif_napi_add(ndev, &rdev->napi, rswitch_poll, 64);

	rswitch_set_mac_address(rdev);

	priv->rswitch_fib_wq = alloc_ordered_workqueue("rswitch_ordered", 0);
	if (!priv->rswitch_fib_wq)
		return -ENOMEM;

	/* FIXME: it seems S4 VPF has FWPBFCSDC0/1 only so that we cannot set
	 * CSD = 1 (rx_default_chain->index = 1) for FWPBFCS03. So, use index = 0
	 * for the RX.
	 */
	err = rswitch_rxdmac_init(ndev, priv, -1);
	if (err < 0)
		goto out_rxdmac;

	err = rswitch_txdmac_init(ndev, priv, -1);
	if (err < 0)
		goto out_txdmac;

	/* Print device information */
	netdev_info(ndev, "MAC address %pMn", ndev->dev_addr);

	return 0;

out_txdmac:
	rswitch_rxdmac_free(ndev, priv);

out_rxdmac:
	netif_napi_del(&rdev->napi);
	free_netdev(ndev);

	return err;
}

void rswitch_ndev_unregister(struct rswitch_private *priv, int index)
{
	struct rswitch_device *rdev = priv->rdev[index];
	struct net_device *ndev = rdev->ndev;

	rswitch_txdmac_free(ndev, priv);
	rswitch_rxdmac_free(ndev, priv);
	unregister_netdev(ndev);
	netif_napi_del(&rdev->napi);
	free_netdev(ndev);

	priv->rdev[index] = NULL;
}

static int rswitch_bpool_config(struct rswitch_private *priv)
{
	u32 val;

	val = rs_read32(priv->addr + CABPIRM);
	if (val & CABPIRM_BPR)
		return 0;

	rs_write32(CABPIRM_BPIOG, priv->addr + CABPIRM);
	return rswitch_reg_wait(priv->addr, CABPIRM, CABPIRM_BPR, CABPIRM_BPR);
}

static void rswitch_queue_interrupt(struct net_device *ndev)
{
	struct rswitch_device *rdev = netdev_priv(ndev);

	if (napi_schedule_prep(&rdev->napi)) {
		rswitch_enadis_data_irq(rdev->priv, rdev->tx_chain->index, false);
		rswitch_enadis_data_irq(rdev->priv, rdev->rx_default_chain->index, false);
		rswitch_enadis_data_irq(rdev->priv, rdev->rx_learning_chain->index, false);
		__napi_schedule(&rdev->napi);
	}
}

static irqreturn_t __maybe_unused rswitch_data_irq(struct rswitch_private *priv, u32 *dis)
{
	struct rswitch_gwca_chain *c;
	int i;
	int index, bit;

	for (i = 0; i < priv->gwca.num_chains; i++) {
		c = &priv->gwca.chains[i];
		index = c->index / 32;
		bit = BIT(c->index % 32);
		if (!(dis[index] & bit) || !(test_bit(i , priv->gwca.used)))
			continue;

		rswitch_ack_data_irq(priv, c->index);
		if (!c->back_info)
			rswitch_queue_interrupt(c->ndev);
		else
			rswitch_vmq_back_data_irq(c);
	}

	return IRQ_HANDLED;
}

static irqreturn_t rswitch_irq(int irq, void *dev_id)
{
	struct rswitch_private *priv = dev_id;
	irqreturn_t ret = IRQ_NONE;
	u32 dis[RSWITCH_NUM_IRQ_REGS];

	rswitch_get_data_irq_status(priv, dis);

	if (rswitch_is_any_data_irq(priv, dis, true) ||
	    rswitch_is_any_data_irq(priv, dis, false))
		ret = rswitch_data_irq(priv, dis);

	return ret;
}

static int rswitch_request_irqs(struct rswitch_private *priv)
{
	int irq, err;

	/* FIXME: other queues */
	irq = platform_get_irq_byname(priv->pdev, "gwca1_rxtx0");
	if (irq < 0)
		goto out;

	err = request_irq(irq, rswitch_irq, 0, "rswitch: gwca1_rxtx0", priv);
	if (err < 0)
		goto out;

out:
	return err;
}

static int rswitch_free_irqs(struct rswitch_private *priv)
{
	int irq;

	irq = platform_get_irq_byname(priv->pdev, "gwca1_rxtx0");
	if (irq < 0)
		return irq;

	free_irq(irq, priv);

	return 0;
}

static int rswitch_ipv4_resolve(struct rswitch_device *rdev, u32 ip, u8 mac[ETH_ALEN])
{
	__be32 be_ip = cpu_to_be32(ip);
	struct net_device *ndev = rdev->ndev;
	struct neighbour *neigh = neigh_lookup(&arp_tbl, &be_ip, ndev);
	int err = 0;

	if (!neigh) {
		neigh = neigh_create(&arp_tbl, &be_ip, ndev);
		if (IS_ERR(neigh))
			return PTR_ERR(neigh);
	}

	neigh_event_send(neigh, NULL);

	read_lock_bh(&neigh->lock);
	if ((neigh->nud_state & NUD_VALID) && !neigh->dead)
		memcpy(mac, neigh->ha, ETH_ALEN);
	else
		err = -ENOENT;
	read_unlock_bh(&neigh->lock);

	neigh_release(neigh);

	return err;
}

void rswitch_add_ipv4_forward_all_types(struct l3_ipv4_fwd_param *param, struct rswitch_ipv4_route *routing_list)
{
	struct l3_ipv4_fwd_param_list *param_list;
	const int frame_type_num = 3;
	int i, j;

	param_list = kcalloc(frame_type_num, sizeof(*param_list), GFP_KERNEL);
	if (!param_list)
		return;

	for (i = 0; i < frame_type_num; i++) {
		param_list[i].param = kcalloc(frame_type_num, sizeof(*param), GFP_KERNEL);
		if (!param_list[i].param)
			goto free;
		memcpy(param_list[i].param, param, sizeof(*param));
	}

	param_list[0].param->frame_type = LTHSLP0v4OTHER;
	if (rswitch_add_l3fwd(param_list[0].param))
		goto free;

	param_list[1].param->frame_type = LTHSLP0v4UDP;
	if (rswitch_add_l3fwd(param_list[1].param)) {
		rswitch_remove_l3fwd(param_list[0].param);
		goto free;
	}

	param_list[2].param->frame_type = LTHSLP0v4TCP;
	if (rswitch_add_l3fwd(param_list[2].param)) {
		rswitch_remove_l3fwd(param_list[0].param);
		rswitch_remove_l3fwd(param_list[1].param);
		goto free;
	}

	list_add(&param_list[0].list, &routing_list->param_list);
	list_add(&param_list[1].list, &routing_list->param_list);
	list_add(&param_list[2].list, &routing_list->param_list);

	return;

free:
	for (j = 0; j < i; j++)
		kfree(param_list[j].param);

	kfree(param_list);
}

static struct rswitch_ipv4_route *rswitch_get_route(struct rswitch_private *priv, u32 dst_ip)
{
	struct rswitch_ipv4_route *routing_list;
	struct list_head *cur;
	int i;

	for (i = 0; i < num_ndev; i++) {
		list_for_each(cur, &priv->rdev[i]->routing_list) {
			routing_list = list_entry(cur, struct rswitch_ipv4_route, list);
			if (routing_list->subnet == (dst_ip & routing_list->mask))
				return routing_list;
		}
	}

	return NULL;
}

void rswitch_add_ipv4_forward(struct rswitch_private *priv, u32 src_ip, u32 dst_ip)
{
	struct rswitch_device *dev;
	struct rswitch_ipv4_route *routing_list = NULL;
	struct l3_ipv4_fwd_param param;
	u8 mac[ETH_ALEN];

	if (is_l3_exist(priv, src_ip, dst_ip))
		return;

	routing_list = rswitch_get_route(priv, dst_ip);
	if (!routing_list)
		return;

	dev = routing_list->dev;
	if (rswitch_ipv4_resolve(dev, dst_ip, mac))
		return;

	param.dv = BIT(dev->port);
	param.csd = 0;
	param.enable_sub_dst = false;
	memcpy(param.l23_info.dst_mac, mac, ETH_ALEN);
	param.slv = 0x3F;
	param.l23_info.priv = priv;
	param.l23_info.update_ttl = true;
	param.l23_info.update_dst_mac = true;
	param.l23_info.update_src_mac = false;
	param.l23_info.routing_port_valid = 0x3F;
	param.l23_info.routing_number = rswitch_rn_get(priv);

	param.priv = priv;
	param.src_ip = src_ip;
	param.dst_ip = dst_ip;

	rswitch_add_ipv4_forward_all_types(&param, routing_list);
}

void rswitch_mfwd_set_port_based(struct rswitch_private *priv, u8 port,
		struct rswitch_gwca_chain *rx_chain)
{
	int gwca_hw_idx = RSWITCH_HW_NUM_TO_GWCA_IDX(priv->gwca.index);

	rs_write32(rx_chain->index, priv->addr + FWPBFCSDC(gwca_hw_idx, port));
	rs_write32(BIT(priv->gwca.index), priv->addr + FWPBFC(port));
}

static void rswitch_fwd_init(struct rswitch_private *priv)
{
	int i;

	for (i = 0; i < RSWITCH_NUM_HW; i++) {
		rs_write32(FWPC0_DEFAULT, priv->addr + FWPC00 + (i * 0x10));
		rs_write32(0, priv->addr + FWPBFC(i));
	}
	/*
	 * FIXME: hardcoded setting. Make a macro about port vector calc.
	 * ETHA0 = forward to GWCA0, GWCA0 = forward to ETHA0,...
	 * Currently, always forward to GWCA1.
	 */
	for (i = 0; i < num_ndev; i++)
		rswitch_mfwd_set_port_based(priv, i, priv->rdev[i]->rx_learning_chain);

	/* Enable Direct Descriptors for GWCA1 */
	rs_write32(FWPC1_DDE, priv->addr + FWPC10 + (priv->gwca.index * 0x10));
	/* Set L3 hash maximum unsecure entry to 512 */
	rs_write32(0x200 << 16, priv->addr + FWLTHHEC);
	/* Disable hash equation */
	rs_write32(0, priv->addr + FWSFHEC);
	/* Enable access from unsecure APB for the first 32 update rules */
	rs_write32(0xffffffff, priv->addr + FWSCR34);
	/* Enable access from unsecure APB for the first 32 four-byte filters */
	rs_write32(0xffffffff, priv->addr + FWSCR12);
	/* Enable access from unsecure APB for the first 32 cascade filters */
	rs_write32(0xffffffff, priv->addr + FWSCR20);
	/* Init parameters for IPv4/v6 hash extract */
	rs_write32(BIT(22) | BIT(23), priv->addr + FWIP4SC);
	/* Reset L3 table */
	rs_write32(LTHTIOG, priv->addr + FWLTHTIM);
	/* TODO: Check result */
	rswitch_reg_wait(priv->addr, FWLTHTIM, LTHTR, 1);
	/* Reset L2/3 update table */
	rs_write32(LTHTIOG, priv->addr + FWL23UTIM);
	/* TODO: Check result */
	rswitch_reg_wait(priv->addr, FWL23UTIM, BIT(1), 1);
	/* TODO: add chrdev for fwd */
	/* TODO: add proc for fwd */
}

static int rswitch_init(struct rswitch_private *priv)
{
	int i;
	int err;

	/* Non hardware initializations */
	for (i = 0; i < num_etha_ports; i++)
		rswitch_etha_init(priv, i);

	err = rswitch_desc_alloc(priv);
	if (err < 0)
		return -ENOMEM;

	/* Hardware initializations */
	if (!parallel_mode)
		rswitch_clock_enable(priv);
	for (i = 0; i < num_ndev; i++)
		rswitch_etha_read_mac_address(&priv->etha[i]);
	rswitch_reset(priv);
	err = rswitch_gwca_hw_init(priv);
	if (err < 0)
		goto out;

	for (i = 0; i < num_ndev; i++) {
		err = rswitch_ndev_create(priv, i);
		if (err < 0)
			goto out;
	}

	/* TODO: chrdev register */

	if (!parallel_mode) {
		err = rswitch_bpool_config(priv);
		if (err < 0)
			goto out;

		rswitch_fwd_init(priv);
	}

	err = rswitch_request_irqs(priv);
	if (err < 0)
		goto out;

	/* Register devices so Linux network stack can access them now */

	for (i = 0; i < num_ndev; i++) {
		err = register_netdev(priv->rdev[i]->ndev);
		if (err)
			goto out;
	}

	return 0;

out:
	for (i--; i >= 0; i--)
		rswitch_ndev_unregister(priv, i);

	rswitch_desc_free(priv);

	return err;
}

static void rswitch_deinit_rdev(struct rswitch_private *priv, int index)
{
	struct rswitch_device *rdev = priv->rdev[index];

	if (rdev->etha && rdev->etha->operated) {
		rswitch_phy_deinit(rdev);
		rswitch_mii_unregister(rdev);
	}
}

static void rswitch_deinit(struct rswitch_private *priv)
{
	int i;

	for (i = 0; i < num_ndev; i++) {
		rswitch_deinit_rdev(priv, i);
		rswitch_ndev_unregister(priv, i);
	}

	rswitch_free_irqs(priv);
	rswitch_desc_free(priv);
}

static int renesas_eth_sw_probe(struct platform_device *pdev)
{
	struct rswitch_private *priv;
	struct resource *res, *res_serdes;
	struct rswitch_net *rn;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	res_serdes = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res || !res_serdes) {
		dev_err(&pdev->dev, "invalid resource\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ptp_priv = rtsn_ptp_alloc(pdev);
	if (!priv->ptp_priv)
		return -ENOMEM;

	if (!parallel_mode)
		parallel_mode = of_property_read_bool(pdev->dev.of_node, "parallel_mode");

	if (parallel_mode) {
		num_ndev = 1;
		num_etha_ports = 1;
	}

	priv->ptp_priv->parallel_mode = parallel_mode;

	if (!parallel_mode) {
		priv->rsw_clk = devm_clk_get(&pdev->dev, "rsw2");
		if (IS_ERR(priv->rsw_clk)) {
			dev_err(&pdev->dev, "Failed to get rsw2 clock: %ld\n",
				PTR_ERR(priv->rsw_clk));
			return -PTR_ERR(priv->rsw_clk);
		}

		priv->phy_clk = devm_clk_get(&pdev->dev, "eth-phy");
		if (IS_ERR(priv->phy_clk)) {
			dev_err(&pdev->dev, "Failed to get eth-phy clock: %ld\n",
				PTR_ERR(priv->phy_clk));
			return -PTR_ERR(priv->phy_clk);
		}
	}

	priv->sd_rst = devm_reset_control_get(&pdev->dev, "eth-phy");

	platform_set_drvdata(pdev, priv);
	priv->pdev = pdev;
	priv->addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->addr))
		return PTR_ERR(priv->addr);

	priv->ptp_priv->addr = priv->addr + RSWITCH_GPTP_OFFSET;
	priv->serdes_addr = devm_ioremap_resource(&pdev->dev, res_serdes);
	if (IS_ERR(priv->serdes_addr))
		return PTR_ERR(priv->serdes_addr);

	debug_addr = priv->addr;
	priv->dev_id = res->start;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret < 0) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret < 0)
			return ret;
	}

	/* Fixed to use GWCA1 */
	priv->gwca.index = 4;
	priv->gwca.num_chains = num_ndev * NUM_CHAINS_PER_NDEV +
		num_virt_devices * 2 * NUM_CHAINS_PER_NDEV;

	priv->gwca.chains = devm_kcalloc(&pdev->dev, priv->gwca.num_chains,
					 sizeof(*priv->gwca.chains), GFP_KERNEL);
	if (!priv->gwca.chains)
		return -ENOMEM;

	if (!parallel_mode) {
		pm_runtime_enable(&pdev->dev);
		pm_runtime_get_sync(&pdev->dev);
		clk_prepare(priv->phy_clk);
		clk_enable(priv->phy_clk);
	}

	rswitch_init(priv);

	device_set_wakeup_capable(&pdev->dev, 1);

	glob_priv = priv;

	register_pernet_subsys(&rswitch_net_ops);

	rn = net_generic(&init_net, rswitch_net_id);
	rn->priv = priv;

	priv->fib_nb.notifier_call = rswitch_fib_event;

	return register_fib_notifier(&init_net, &priv->fib_nb, NULL, NULL);
}

static int renesas_eth_sw_remove(struct platform_device *pdev)
{
	struct rswitch_private *priv = platform_get_drvdata(pdev);

	if (!parallel_mode) {
		/* Disable R-Switch clock */
		rs_write32(RCDC_RCD, priv->addr + RCDC);
		rswitch_deinit(priv);

		pm_runtime_put(&pdev->dev);
		pm_runtime_disable(&pdev->dev);
		clk_disable(priv->phy_clk);
	}

	rswitch_desc_free(priv);

	platform_set_drvdata(pdev, NULL);
	unregister_fib_notifier(&init_net, &priv->fib_nb);
	glob_priv = NULL;

	return 0;
}

static struct platform_driver renesas_eth_sw_driver_platform = {
	.probe = renesas_eth_sw_probe,
	.remove = renesas_eth_sw_remove,
	.driver = {
		.name = "renesas_eth_sw",
		.of_match_table = renesas_eth_sw_of_table,
	}
};
module_platform_driver(renesas_eth_sw_driver_platform);
MODULE_AUTHOR("Yoshihiro Shimoda");
MODULE_DESCRIPTION("Renesas Ethernet Switch device driver");
MODULE_LICENSE("GPL v2");
