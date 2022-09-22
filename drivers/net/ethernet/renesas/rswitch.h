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
#include <net/ip_fib.h>

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
	DT_FEND		= 0xB8,

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
	DT_FEMPTY_ND	= 0x38,
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

#define DESC_INFO1_FMT		BIT(2)
#define DESC_INFO1_CSD0_SHIFT	32
#define DESC_INFO1_CSD1_SHIFT	40
#define DESC_INFO1_DV_SHIFT	48

struct rswitch_gwca_chain {
	int index;
	bool dir_tx;
	bool gptp;
	union {
		struct rswitch_ext_desc *ring;
		struct rswitch_ext_ts_desc *ts_ring;
	};
	dma_addr_t ring_dma;
	u32 num_ring;
	u32 cur;
	u32 dirty;
	u32 osid;
	struct sk_buff **skb;

	struct net_device *ndev;	/* chain to ndev for irq */
	struct rswitch_vmq_back_info *back_info;
};

#define RSWITCH_MAX_NUM_CHAINS	128
#define RSWITCH_NUM_IRQ_REGS	(RSWITCH_MAX_NUM_CHAINS / BITS_PER_TYPE(u32))
#define RSWITCH_NUM_HW		5
#define RSWITCH_MAX_NUM_ETHA	3
#define RSWITCH_MAX_NUM_NDEV	8
#define RSWITCH_MAX_NUM_L23	256

#define TX_RING_SIZE		1024
#define RX_RING_SIZE		1024

#define PKT_BUF_SZ		1584
#define RSWITCH_ALIGN		128
#define RSWITCH_MAX_CTAG_PCP	7

struct rswitch_gwca {
	int index;
	struct rswitch_gwca_chain *chains;
	int num_chains;
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

struct l23_update_info {
	struct rswitch_private *priv;
	u8 dst_mac[ETH_ALEN];
	u32 routing_port_valid;
	u32 routing_number;
	bool update_ttl;
	bool update_dst_mac;
	bool update_src_mac;
};

struct l3_ipv4_fwd_param {
	struct rswitch_private *priv;
	struct l23_update_info l23_info;
	u32 src_ip;
	union {
		u32 dst_ip;
		u32 pf_cascade_index;
	};
	/* CPU sub destination */
	u32 csd;
	/* Destination vector */
	u32 dv;
	/* Source lock vector */
	u32 slv;
	u8 frame_type;
	bool enable_sub_dst;
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

enum rswitch_tc_u32_action {
	FILTER_PASS = 0,
	FILTER_DROP,
	FILTER_SWITCH
};

struct rswitch_tc_u32_filter {
	u32 ip;
	u32 subnet;
	u32 mask;
	u32 offset;
	enum rswitch_tc_u32_action action;
	struct rswitch_device *dev;
	struct list_head list;
	struct l3_ipv4_fwd_param param;
};

struct rswitch_device {
	struct rswitch_private *priv;
	struct net_device *ndev;
	struct napi_struct napi;
	void __iomem *addr;
	bool gptp_master;
	struct rswitch_gwca_chain *tx_chain;
	struct rswitch_gwca_chain *rx_default_chain;
	struct rswitch_gwca_chain *rx_learning_chain;
	spinlock_t lock;
	u8 ts_tag;

	int port;
	struct rswitch_etha *etha;
	u8 remote_chain;
	struct rswitch_vmq_front_info *front_info;
	struct list_head routing_list;
	struct list_head tc_u32_list;
};

/* Two-byte filter number */
#define PFL_TWBF_N (48)
/* Three-byte filter number */
#define PFL_THBF_N (16)
/* Four-byte filter number */
#define PFL_FOBF_N (48)
/* Range-byte filter number */
#define PFL_RAGF_N (16)
/* Cascade filter number */
#define PFL_CADF_N (64)

struct rswitch_filters {
	DECLARE_BITMAP(two_bytes, PFL_TWBF_N);
	DECLARE_BITMAP(three_bytes, PFL_THBF_N);
	DECLARE_BITMAP(four_bytes, PFL_FOBF_N);
	DECLARE_BITMAP(range_byte, PFL_RAGF_N);
	DECLARE_BITMAP(cascade, PFL_CADF_N);
};

struct rswitch_private {
	struct platform_device *pdev;
	void __iomem *addr;
	void __iomem *serdes_addr;
	struct rtsn_ptp_private *ptp_priv;
	struct rswitch_desc *desc_bat;
	dma_addr_t desc_bat_dma;
	u32 desc_bat_size;
	phys_addr_t dev_id;

	struct rswitch_device *rdev[RSWITCH_MAX_NUM_NDEV];

	struct rswitch_gwca gwca;
	struct rswitch_etha etha[RSWITCH_MAX_NUM_ETHA];
	struct rswitch_mfwd mfwd;
	struct rswitch_filters filters;

	struct clk *rsw_clk;
	struct clk *phy_clk;

	struct notifier_block fib_nb;
	struct workqueue_struct *rswitch_fib_wq;
	DECLARE_BITMAP(l23_routing_number, RSWITCH_MAX_NUM_L23);
	struct reset_control *sd_rst;
};

struct rswitch_fib_event_work {
	struct work_struct work;
	union {
		struct fib_entry_notifier_info fen_info;
		struct fib_rule_notifier_info fr_info;
	};
	struct rswitch_private *priv;
	unsigned long event;
};

extern const struct net_device_ops rswitch_netdev_ops;

struct rswitch_gwca_chain *rswitch_gwca_get(struct rswitch_private *priv);
void rswitch_gwca_put(struct rswitch_private *priv,
		      struct rswitch_gwca_chain *c);

int rswitch_txdmac_init(struct net_device *ndev, struct rswitch_private *priv,
			int chain_num);
void rswitch_txdmac_free(struct net_device *ndev, struct rswitch_private *priv);

int rswitch_rxdmac_init(struct net_device *ndev, struct rswitch_private *priv,
			int chain_num);
void rswitch_rxdmac_free(struct net_device *ndev, struct rswitch_private *priv);

void rswitch_ndev_unregister(struct rswitch_private *priv, int index);

int rswitch_poll(struct napi_struct *napi, int budget);
int rswitch_tx_free(struct net_device *ndev, bool free_txed_only);

void rswitch_gwca_chain_register(struct rswitch_private *priv,
				 struct rswitch_gwca_chain *c, bool ts);

void rswitch_trigger_chain(struct rswitch_private *priv,
			   struct rswitch_gwca_chain *chain);
void rswitch_enadis_rdev_irqs(struct rswitch_device *rdev, bool enable);

struct rswitch_private *rswitch_find_priv(void);

void rswitch_vmq_front_trigger_tx(struct rswitch_device* rdev);
void rswitch_vmq_front_rx_done(struct rswitch_device* rdev);
void rswitch_vmq_back_data_irq(struct rswitch_gwca_chain *c);

int rswitch_desc_alloc(struct rswitch_private *priv);
void rswitch_desc_free(struct rswitch_private *priv);

void rswitch_mfwd_set_port_based(struct rswitch_private *priv, u8 port,
				 struct rswitch_gwca_chain *rx_chain);

void rswitch_enadis_data_irq(struct rswitch_private *priv, int index,
			     bool enable);

static inline bool rswitch_is_front_dev(struct rswitch_device *rdev)
{
	return rdev->front_info != NULL;
}

static inline bool rswitch_is_front_priv(struct rswitch_private *priv)
{
	return priv->addr == NULL;
}
