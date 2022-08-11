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
#include <net/flow_offload.h>
#include <net/fib_notifier.h>
#include <net/ip_fib.h>
#include <net/tc_act/tc_mirred.h>
#include <net/tc_act/tc_skbmod.h>
#include <net/tc_act/tc_gact.h>

#define RSWITCH_MAX_NUM_ETHA	3
#define RSWITCH_MAX_NUM_NDEV	8
#define RSWITCH_MAX_NUM_CHAINS	128
#define RSWITCH_NUM_IRQ_REGS	(RSWITCH_MAX_NUM_CHAINS / BITS_PER_TYPE(u32))
#define MAX_PF_ENTRIES (8)
#define RSWITCH_MAX_NUM_L23 256

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

#define RSWITCH_MAC_DST_OFFSET (0)
#define RSWITCH_MAC_SRC_OFFSET (6)
#define RSWITCH_IP_VERSION_OFFSET (12)
#define RSWITCH_IPV4_HEADER_OFFSET (14)
#define RSWITCH_IPV4_TOS_OFFSET (15)
#define RSWITCH_IPV4_TTL_OFFSET (22)
#define RSWITCH_IPV4_PROTO_OFFSET (23)
#define RSWITCH_IPV4_SRC_OFFSET (26)
#define RSWITCH_IPV4_DST_OFFSET (30)
#define RSWITCH_L4_SRC_PORT_OFFSET (34)
#define RSWITCH_L4_DST_PORT_OFFSET (36)

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
	struct sk_buff **skb;

	struct net_device *ndev;	/* chain to ndev for irq */
};

struct rswitch_gwca {
	int index;
	struct rswitch_gwca_chain *chains;
	int num_chains;
	DECLARE_BITMAP(used, RSWITCH_MAX_NUM_CHAINS);
	u32 tx_irq_bits[RSWITCH_NUM_IRQ_REGS];
	u32 rx_irq_bits[RSWITCH_NUM_IRQ_REGS];
};

struct rswitch_mfwd_mac_table_entry {
	int chain_index;
	unsigned char addr[MAX_ADDR_LEN];
};

struct rswitch_mfwd {
	struct rswitch_mac_table_entry *mac_table_entries;
	int num_mac_table_entries;
};

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
	struct list_head routing_list;

	struct list_head tc_u32_list;
	struct list_head tc_flower_list;
	struct list_head tc_matchall_list;
};

enum pf_type {
	PF_TWO_BYTE,
	PF_THREE_BYTE,
	PF_FOUR_BYTE,
};

struct rswitch_pf_entry {
	u32 val;
	u32 mask;
	u32 off;
	enum pf_type type;

	void *cfg0_addr;
	void *cfg1_addr;
	void *offs_addr;
	u32 pf_idx;
	/* Used for cascade filter config */
	u32 pf_num;
};

struct rswitch_pf_param {
	struct rswitch_device *rdev;
	struct rswitch_pf_entry entries[MAX_PF_ENTRIES];
	int used_entries;
	bool all_sources;
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

static inline u32 rs_read32(void *addr)
{
	return ioread32(addr);
}

static inline void rs_write32(u32 data, void *addr)
{
	iowrite32(data, addr);
}

int rswitch_add_l3fwd(struct l3_ipv4_fwd_param *param);
int rswitch_remove_l3fwd(struct l3_ipv4_fwd_param *param);
void rswitch_put_pf(struct l3_ipv4_fwd_param *param);
int rswitch_setup_pf(struct rswitch_pf_param *pf_param);
int rswitch_rn_get(struct rswitch_private *priv);
