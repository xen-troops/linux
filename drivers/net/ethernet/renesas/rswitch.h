/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver common functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#include <linux/kernel.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/io.h>

static inline u32 rs_read32(void *addr)
{
	return ioread32(addr);
}

static inline void rs_write32(u32 data, void *addr)
{
	iowrite32(data, addr);
}

enum rswitch_gwca_mode {
	GWMC_OPC_RESET,
	GWMC_OPC_DISABLE,
	GWMC_OPC_CONFIG,
	GWMC_OPC_OPERATION,
};

/* Descriptors */
enum RX_DS_CC_BIT {
	RX_DS	= 0x0fff, /* Data size */
	RX_TR	= 0x1000, /* Truncation indication */
	RX_EI	= 0x2000, /* Error indication */
	RX_PS	= 0xc000, /* Padding selection */
};

enum TX_DS_TAGL_BIT {
	TX_DS	= 0x0fff, /* Data size */
	TX_TAGL	= 0xf000, /* Frame tag LSBs */
};

enum DIE_DT {
	/* Frame data */
	DT_FSINGLE	= 0x80,
	DT_FSTART	= 0x90,
	DT_FMID		= 0xA0,
	DT_FEND		= 0xB0,

	/* Chain control */
	DT_LEMPTY	= 0xC0,
	DT_EEMPTY	= 0xD0,
	DT_LINKFIX	= 0x00,
	DT_LINK		= 0xE0,
	DT_EOS		= 0xF0,
	/* HW/SW arbitration */
	DT_FEMPTY	= 0x40,
	DT_FEMPTY_IS	= 0x10,
	DT_FEMPTY_IC	= 0x20,
	DT_FEMPTY_ND	= 0x30,
	DT_FEMPTY_START	= 0x50,
	DT_FEMPTY_MID	= 0x60,
	DT_FEMPTY_END	= 0x70,

	DT_MASK		= 0xF0,
	DIE		= 0x08,	/* Descriptor Interrupt Enable */
};

struct rswitch_desc {
	__le16 info_ds;	/* Descriptor size */
	u8 die_dt;	/* Descriptor interrupt enable and type */
	__u8  dptrh;	/* Descriptor pointer MSB */
	__le32 dptrl;	/* Descriptor pointer LSW */
} __packed;

struct rswitch_ts_desc {
	__le16 info_ds;	/* Descriptor size */
	u8 die_dt;	/* Descriptor interrupt enable and type */
	__u8  dptrh;	/* Descriptor pointer MSB */
	__le32 dptrl;	/* Descriptor pointer LSW */
	__le32 ts_nsec;
	__le32 ts_sec;
} __packed;

struct rswitch_ext_desc {
	__le16 info_ds;	/* Descriptor size */
	u8 die_dt;	/* Descriptor interrupt enable and type */
	__u8  dptrh;	/* Descriptor pointer MSB */
	__le32 dptrl;	/* Descriptor pointer LSW */
	__le64 info1;
} __packed;

struct rswitch_ext_ts_desc {
	__le16 info_ds;	/* Descriptor size */
	u8 die_dt;	/* Descriptor interrupt enable and type */
	__u8  dptrh;	/* Descriptor pointer MSB */
	__le32 dptrl;	/* Descriptor pointer LSW */
	__le64 info1;
	__le32 ts_nsec;
	__le32 ts_sec;
} __packed;

struct rswitch_gwca_chain {
	union {
		struct rswitch_ext_desc *tx_ring;
		struct rswitch_ext_ts_desc *rx_ring;
		struct rswitch_ts_desc *ts_ring;
	};

	/* Common */
	dma_addr_t ring_dma;
	u32 num_ring;
	u32 cur;
	u32 dirty;

	/* For [rt]x_ring */
	int index;
	bool dir_tx;
	struct sk_buff **skb;
	struct net_device *ndev;	/* chain to ndev for irq */

	/* For RX multi-descriptor handling */
	void **rx_bufs;
	bool multi_desc;
	u16 total_len;
	struct sk_buff *skb_multi;
};

#define INFO1_FMT		BIT(2)
#define INFO1_TXC		BIT(3)
#define RSWITCH_MAX_NUM_CHAINS	128
#define RSWITCH_NUM_IRQ_REGS	(RSWITCH_MAX_NUM_CHAINS / BITS_PER_TYPE(u32))
#define RSWITCH_NUM_HW		5
#define RSWITCH_MAX_NUM_ETHA	3
#define RSWITCH_MAX_NUM_NDEV	8

#define TX_RING_SIZE		1024
#define RX_RING_SIZE		1024

#define RSWITCH_ALIGN		128
#define RSWITCH_MAX_CTAG_PCP	7
#define MAX_MTU_SZ		9000
#define MAX_DESC_SZ		2048

struct rswitch_gwca_ts_info {
	struct sk_buff *skb;
	struct list_head list;

	int port;
	u8 tag;
};

struct rswitch_gwca {
	int index;
	struct rswitch_gwca_chain *chains;
	int num_chains;
	struct rswitch_gwca_chain ts_queue;
	struct list_head ts_info_list;
	DECLARE_BITMAP(used, RSWITCH_MAX_NUM_CHAINS);
	u32 tx_irq_bits[RSWITCH_NUM_IRQ_REGS];
	u32 rx_irq_bits[RSWITCH_NUM_IRQ_REGS];
	int speed;
};

struct rswitch_etha {
	int index;
	void __iomem *addr;
	void __iomem *serdes_addr;
	bool external_phy;
	struct mii_bus *mii;
	phy_interface_t phy_interface;
	u32 psmcs;
	u8 mac_addr[MAX_ADDR_LEN];
	int link;
	int speed;
	bool operated;
};

struct rswitch_mfwd_mac_table_entry {
	int chain_index;
	unsigned char addr[MAX_ADDR_LEN];
};

struct rswitch_mfwd {
	struct rswitch_mac_table_entry *mac_table_entries;
	int num_mac_table_entries;
};

struct rswitch_device {
	struct rswitch_private *priv;
	struct net_device *ndev;
	struct napi_struct napi;
	void __iomem *addr;
	struct rswitch_gwca_chain *tx_chain;
	struct rswitch_gwca_chain *rx_chain;
	spinlock_t lock;
	u8 ts_tag;

	int port;
	struct rswitch_etha *etha;
	int remote_chain;
};

struct rswitch_private {
	struct platform_device *pdev;
	void __iomem *addr;
	void __iomem *serdes_addr;
	struct rtsn_ptp_private *ptp_priv;
	struct rswitch_desc *desc_bat;
	dma_addr_t desc_bat_dma;
	u32 desc_bat_size;

	struct rswitch_device *rdev[RSWITCH_MAX_NUM_NDEV];

	struct rswitch_gwca gwca;
	struct rswitch_etha etha[RSWITCH_MAX_NUM_ETHA];
	struct rswitch_mfwd mfwd;

	struct clk *rsw_clk;
	struct clk *phy_clk;

	struct reset_control *sd_rst;

	u8 chan_running;
	bool serdes_common_init;

	spinlock_t lock;	/* lock interrupt registers' control */
	struct clk *clk;
};

extern const struct net_device_ops rswitch_netdev_ops;

int rswitch_txdmac_init(struct net_device *ndev, struct rswitch_private *priv);
void rswitch_txdmac_free(struct net_device *ndev, struct rswitch_private *priv);

int rswitch_rxdmac_init(struct net_device *ndev, struct rswitch_private *priv);
void rswitch_rxdmac_free(struct net_device *ndev, struct rswitch_private *priv);

int rswitch_poll(struct napi_struct *napi, int budget);
struct rswitch_private *rswitch_find_priv(void);

