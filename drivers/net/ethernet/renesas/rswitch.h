/* SPDX-License-Identifier: GPL-2.0 */
/* Renesas Ethernet Switch Driver common functions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 * Copyright (C) 2022 EPAM Systems
 */

#ifndef __RSWITCH_H__
#define __RSWITCH_H__

#include <linux/kernel.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/io.h>
#include <linux/if_vlan.h>
#include <net/flow_offload.h>
#include <net/fib_notifier.h>
#include <net/ip_fib.h>
#include <net/tc_act/tc_mirred.h>
#include <net/tc_act/tc_skbmod.h>
#include <net/tc_act/tc_gact.h>

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
	DT_FEMPTY_ND	= 0x30,
	DT_FEMPTY_START	= 0x50,
	DT_FEMPTY_MID	= 0x60,
	DT_FEMPTY_END	= 0x70,

	DT_MASK		= 0xF0,
	DIE		= 0x08,	/* Descriptor Interrupt Enable */
};

#define RSWITCH_MAX_NUM_ETHA	3
#define RSWITCH_MAX_NUM_NDEV	8
#define RSWITCH_MAX_NUM_CHAINS	128
#define RSWITCH_NUM_IRQ_REGS	(RSWITCH_MAX_NUM_CHAINS / BITS_PER_TYPE(u32))
#define MAX_PF_ENTRIES (7)
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

#define RSWITCH_PF_MASK_MODE (0)
#define RSWITCH_PF_EXPAND_MODE (BIT(0))
#define RSWITCH_PF_PRECISE_MODE (BIT(1))

/* Valid only for two-byte filters (TWBFMi) */
#define RSWITCH_PF_OFFSET_FILTERING (0)
#define RSWITCH_PF_TAG_FILTERING (BIT(0))

#define RSWITCH_PF_DISABLE_FILTER (0)
#define RSWITCH_PF_ENABLE_FILTER (BIT(15))

#define RSWITCH_MAC_DST_OFFSET (0)
#define RSWITCH_MAC_SRC_OFFSET (6)
#define RSWITCH_IP_VERSION_OFFSET (12)
#define RSWITCH_MAC_HEADER_LEN (14)
#define RSWITCH_IPV4_TOS_OFFSET (15)
#define RSWITCH_IPV4_TTL_OFFSET (22)
#define RSWITCH_IPV4_PROTO_OFFSET (23)
#define RSWITCH_IPV4_SRC_OFFSET (26)
#define RSWITCH_IPV4_DST_OFFSET (30)
#define RSWITCH_IPV6_SRC_OFFSET (22)
#define RSWITCH_IPV6_DST_OFFSET (38)
#define RSWITCH_L4_SRC_PORT_OFFSET (34)
#define RSWITCH_L4_DST_PORT_OFFSET (36)
#define RSWITCH_VLAN_STAG_OFFSET (0)
#define RSWITCH_VLAN_CTAG_OFFSET (2)

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
	u32 osid;

	/* For [rt]x_ring */
	int index;
	bool dir_tx;
	struct sk_buff **skb;
	struct rswitch_device *rdev;	/* chain to rdev for irq */
	struct rswitch_vmq_back_info *back_info;
};


#define RSWITCH_MAX_NUM_CHAINS	128
#define RSWITCH_NUM_IRQ_REGS	(RSWITCH_MAX_NUM_CHAINS / BITS_PER_TYPE(u32))
#define RSWITCH_NUM_HW		5
#define RSWITCH_MAX_NUM_ETHA	3
#define RSWITCH_MAX_RMON_DEV	3
#define RSWITCH_MAX_NUM_NDEV	8
#define RSWITCH_MAX_NUM_L23	256

#define TX_RING_SIZE		1024
#define RX_RING_SIZE		1024

#define PKT_BUF_SZ		1584
#define RSWITCH_ALIGN		128
#define RSWITCH_MAX_CTAG_PCP	7

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

struct rswitch_mfwd_mac_table_entry {
	int chain_index;
	unsigned char addr[MAX_ADDR_LEN];
};

struct rswitch_mfwd {
	struct rswitch_mac_table_entry *mac_table_entries;
	int num_mac_table_entries;
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

#define filter_index_check(idx, idx_max) ((idx) < (idx_max)) ? (idx) : -1

#define get_two_byte_filter(priv) \
	filter_index_check(find_first_zero_bit(priv->filters.two_bytes, PFL_TWBF_N), PFL_TWBF_N)
#define get_three_byte_filter(priv) \
	filter_index_check(find_first_zero_bit(priv->filters.three_bytes, PFL_THBF_N), PFL_THBF_N)
#define get_four_byte_filter(priv) \
	filter_index_check(find_first_zero_bit(priv->filters.four_bytes, PFL_FOBF_N), PFL_FOBF_N)

struct rswitch_filters {
	DECLARE_BITMAP(two_bytes, PFL_TWBF_N);
	DECLARE_BITMAP(three_bytes, PFL_THBF_N);
	DECLARE_BITMAP(four_bytes, PFL_FOBF_N);
	DECLARE_BITMAP(range_byte, PFL_RAGF_N);
	DECLARE_BITMAP(cascade, PFL_CADF_N);
};

struct rswitch_device {
	struct list_head list;
	struct rswitch_private *priv;
	struct net_device *ndev;
	struct napi_struct napi;
	void __iomem *addr;
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
#if IS_ENABLED(CONFIG_IP_MROUTE)
	/* List for L3 multicast routing offload */
	struct list_head mult_routing_list;
#endif

	struct list_head tc_u32_list;
	struct list_head tc_flower_list;
	struct list_head tc_matchall_list;

	/* For VLAN devices, kernel constructs ndev and fills needed structures such as dev.parent,
	 * but for proper chain mapping R-Switch driver requires real device parent. So we need to
	 * save pointer to ndev->dev.parent and restore it for proper kernel deinit ndev.
	 */
	struct device *vlan_parent;
	bool mondev;
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

	struct list_head rdev_list;
	struct rswitch_device *rmon_dev[RSWITCH_MAX_RMON_DEV];
	rwlock_t rdev_list_lock;

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
	struct rswitch_gwca_chain *mon_rx_chain;
	struct rswitch_gwca_chain *mon_tx_chain;

	struct workqueue_struct *rswitch_netevent_wq;

	bool ipv4_forward_enabled;
	bool offload_enabled;
	struct mutex ipv4_forward_lock;
	struct workqueue_struct *rswitch_forward_wq;
	/* Maximum number of hash collisions for L3 hash forwarding
	 * table that can guarantee appropriate speed.
	 */
	u16 max_collisions;
	u16 hash_equation;

	u8 chan_running;
	spinlock_t lock;	/* lock interrupt registers' control */
};

enum pf_type {
	PF_TWO_BYTE,
	PF_THREE_BYTE,
	PF_FOUR_BYTE,
};

struct rswitch_pf_entry {
	u32 val;
	union {
		/* Used in mask mode */
		u32 mask;
		/* Used in expand mode */
		u32 ext_val;
	};
	u32 off;
	enum pf_type type;

	u8 match_mode;		/* RSWITCH_PF_*_MODE */
	/* Valid only for two-byte filters */
	u8 filtering_mode;	/* RSWITCH_PF_*_FILTERING */

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
	bool update_ctag_vlan_id;
	bool update_ctag_vlan_prio;
	u16 vlan_id;
	u8 vlan_prio;
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

int rswitch_add_l3fwd(struct l3_ipv4_fwd_param *param);
int rswitch_remove_l3fwd(struct l3_ipv4_fwd_param *param);
void rswitch_put_pf(struct l3_ipv4_fwd_param *param);
int rswitch_setup_pf(struct rswitch_pf_param *pf_param);
int rswitch_rn_get(struct rswitch_private *priv);

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

void rswitch_ndev_unregister(struct rswitch_device *rdev, int index);

int rswitch_poll(struct napi_struct *napi, int budget);
int rswitch_tx_free(struct net_device *ndev, bool free_txed_only);

void rswitch_gwca_chain_register(struct rswitch_private *priv,
				 struct rswitch_gwca_chain *c, bool ts);

void rswitch_trigger_chain(struct rswitch_private *priv,
			   struct rswitch_gwca_chain *chain);
void rswitch_enadis_rdev_irqs(struct rswitch_device *rdev, bool enable);

struct rswitch_private *rswitch_find_priv(void);

void rswitch_vmq_front_trigger_tx(struct rswitch_device *rdev);
void rswitch_vmq_front_rx_done(struct rswitch_device *rdev);
void rswitch_vmq_back_data_irq(struct rswitch_gwca_chain *c);

int rswitch_desc_alloc(struct rswitch_private *priv);
void rswitch_desc_free(struct rswitch_private *priv);

void rswitch_mfwd_set_port_based(struct rswitch_private *priv, u8 port,
				 struct rswitch_gwca_chain *rx_chain);

void rswitch_enadis_data_irq(struct rswitch_private *priv, int index,
			     bool enable);

static inline bool rswitch_is_front_dev(struct rswitch_device *rdev)
{
	return rdev->front_info;
}

static inline bool rswitch_is_front_priv(struct rswitch_private *priv)
{
	return !priv->addr;
}

static inline struct rswitch_device *rswitch_find_rdev_by_port(struct rswitch_private *priv,
							       int port)
{
	struct rswitch_device *rdev;

	read_lock(&priv->rdev_list_lock);
	list_for_each_entry(rdev, &priv->rdev_list, list) {
		if (rdev->port == port) {
			read_unlock(&priv->rdev_list_lock);
			return rdev;
		}
	}
	read_unlock(&priv->rdev_list_lock);

	return NULL;
}

static inline bool ndev_is_tsn_dev(const struct net_device *ndev,
			struct rswitch_private *priv)
{
	struct rswitch_device *rdev;

	list_for_each_entry(rdev, &priv->rdev_list, list) {
		/* TSN devices contains valid etha pointer, VMQs contains NULL */
		if (rdev->ndev == ndev && rdev->etha != NULL)
			return true;
	}

	return false;
}

struct rswitch_device *ndev_to_rdev(const struct net_device *ndev);

int rswitch_add_l3fwd(struct l3_ipv4_fwd_param *param);
int rswitch_remove_l3fwd(struct l3_ipv4_fwd_param *param);
void rswitch_put_pf(struct l3_ipv4_fwd_param *param);
int rswitch_setup_pf(struct rswitch_pf_param *pf_param);
int rswitch_rn_get(struct rswitch_private *priv);

/* Helper functions for perfect filter initialization */
static inline int rswitch_init_mask_pf_entry(struct rswitch_pf_param *p,
		enum pf_type type, u32 value, u32 mask, u32 offset)
{
	int idx = p->used_entries;

	if (idx >= MAX_PF_ENTRIES) {
		return -E2BIG;
	}

	p->entries[idx].match_mode = RSWITCH_PF_MASK_MODE;
	p->entries[idx].filtering_mode = RSWITCH_PF_OFFSET_FILTERING;
	p->entries[idx].type = type;
	p->entries[idx].val = value;
	p->entries[idx].mask = mask;
	p->entries[idx].off = offset;
	p->used_entries++;

	return 0;
}

static inline int rswitch_init_tag_mask_pf_entry(struct rswitch_pf_param *p,
		u32 value, u32 mask, u32 offset)
{
	int idx = p->used_entries;

	if (idx >= MAX_PF_ENTRIES) {
		return -E2BIG;
	}

	p->entries[idx].match_mode = RSWITCH_PF_MASK_MODE;
	p->entries[idx].filtering_mode = RSWITCH_PF_TAG_FILTERING;
	/* Tag filtering supported only for two-byte filters */
	p->entries[idx].type = PF_TWO_BYTE;
	p->entries[idx].val = value;
	p->entries[idx].mask = mask;
	p->entries[idx].off = offset;
	p->used_entries++;

	return 0;
}

static inline int rswitch_init_tag_expand_pf_entry(struct rswitch_pf_param *p,
						   u32 value, u32 expand_value)
{
	int idx = p->used_entries;

	if (idx >= MAX_PF_ENTRIES)
		return -E2BIG;

	p->entries[idx].match_mode = RSWITCH_PF_EXPAND_MODE;
	p->entries[idx].filtering_mode = RSWITCH_PF_TAG_FILTERING;
	/* Tag filtering supported only for two-byte filters */
	p->entries[idx].type = PF_TWO_BYTE;
	p->entries[idx].val = value;
	p->entries[idx].ext_val = expand_value;
	p->entries[idx].off = 0;
	p->used_entries++;

	return 0;
}

static inline int rswitch_init_expand_pf_entry(struct rswitch_pf_param *p,
		enum pf_type type, u32 value, u32 expand_value, u32 offset)
{
	int idx = p->used_entries;

	if (idx >= MAX_PF_ENTRIES) {
		return -E2BIG;
	}

	p->entries[idx].match_mode = RSWITCH_PF_EXPAND_MODE;
	/* This mode is not supported for tag filtering */
	p->entries[idx].filtering_mode = RSWITCH_PF_OFFSET_FILTERING;
	p->entries[idx].type = type;
	p->entries[idx].val = value;
	p->entries[idx].ext_val = expand_value;
	p->entries[idx].off = offset;
	p->used_entries++;

	return 0;
}

static inline bool rswitch_ipv6_all_set(struct in6_addr *addr)
{
	return ( (addr->s6_addr32[0] & addr->s6_addr32[1] &
		  addr->s6_addr32[2] & addr->s6_addr32[3]) == 0xffffffff);
}

static inline bool rswitch_ipv6_all_zero(struct in6_addr *addr)
{
	return ( !(addr->s6_addr32[0] | addr->s6_addr32[1] |
		   addr->s6_addr32[2] | addr->s6_addr32[3]));
}

void rswitch_gwca_chain_set_irq_delay(struct rswitch_private *priv,
				     struct rswitch_gwca_chain *chain,
				     u16 delay);
#endif /* __RSWITCH_H__ */
