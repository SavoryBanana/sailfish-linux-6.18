// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone MSM8996 IPA v2.5 (BAM transport) driver for Google Pixel (sailfish).
 *
 * Configures IPA v2.5 hardware registers and SRAM tables via BAM immediate
 * commands, performs the AP <-> MPSS QMI IPA (0x31) handshake, and exposes
 * rmnet_ipa0 (ARPHRD_RAWIP / ETH_P_MAP) for QMAP packet data.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/etherdevice.h>
#include <linux/gfp.h>
#include <linux/if_arp.h>
#include <linux/if_rmnet.h>
#include <linux/interconnect.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/qrtr.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/workqueue.h>
#include <net/pkt_sched.h>

#include "ipa_qmi_msg.h"

/* IPA v2.5 register offsets relative to ipa-reg (0x006c0000) */
#define IPA_V2_REG_COMP_HW_VERSION		0x0030
#define IPA_V2_REG_VERSION			0x0034
#define IPA_V2_REG_COMP_CFG			0x0038
#define IPA_V2_REG_COMP_SW_RESET		0x003c
#define IPA_V2_REG_ROUTE			0x0044
#define IPA_V2_REG_SHARED_MEM_SIZE		0x0050
#define IPA_V2_REG_BCR				0x05b0
#define IPA_V2_REG_LOCAL_PKT_PROC_CNTXT_BASE	0x05e0
#define IPA_V2_REG_COUNTER_CFG			0x05e8

#define IPA_V2_REG_ENDP_INIT_CTRL(n)		(0x0070 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_CFG(n)		(0x00c0 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_NAT(n)		(0x0120 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_HDR(n)		(0x0170 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_HDR_EXT(n)		(0x01c0 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_HDR_MD_MASK(n)	(0x0220 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_MODE(n)		(0x02c0 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_AGGR(n)		(0x0320 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_HOL_BLOCK_EN(n)	(0x03c0 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_HOL_BLOCK_TIMER(n)	(0x0420 + 0x4 * (n))
#define IPA_V2_REG_ENDP_INIT_DEAGGR(n)		(0x0470 + 0x4 * (n))
#define IPA_V2_REG_ENDP_STATUS(n)		(0x04c0 + 0x4 * (n))

#define IPA_V2_REG_IRQ_STTS			0x1008
#define IPA_V2_REG_IRQ_EN			0x100c
#define IPA_V2_REG_IRQ_CLR			0x1010
#define IPA_V2_REG_IRQ_UC			0x101c
#define IPA_V2_REG_IRQ_SUSPEND_INFO		0x1098

#define IPA_V2_SHARED_MEM_SIZE_FMASK		GENMASK(15, 0)
#define IPA_V2_SHARED_MEM_BADDR_FMASK		GENMASK(31, 16)
#define IPA_V2_BCR_VAL				0x001fff7fU

/* IPA v2.5 IRQ bits */
#define IPA_V2_IRQ_UC_0				2
#define IPA_V2_IRQ_UC_1				3
#define IPA_V2_IRQ_TX_SUSPEND			16

/* AP BAM pipe IDs for IPA v2.5 */
#define IPA_V2_EP_AP_LAN_RX			2
#define IPA_V2_EP_AP_CMD_TX			3
#define IPA_V2_EP_AP_MODEM_TX			4
#define IPA_V2_EP_AP_MODEM_RX			5

/* IPA v2.5 immediate command opcodes */
#define IPA_V2_CMD_IP_V4_FILTER_INIT		3
#define IPA_V2_CMD_IP_V6_FILTER_INIT		4
#define IPA_V2_CMD_IP_V4_ROUTING_INIT		7
#define IPA_V2_CMD_IP_V6_ROUTING_INIT		8
#define IPA_V2_CMD_HDR_INIT_LOCAL		9
#define IPA_V2_CMD_DMA_SHARED_MEM		19

/* IPA v2.5 SRAM layout (offsets relative to ipa->mem_offset) */
#define IPA_V2_MEM_CANARY_VAL			cpu_to_le32(0xdeadbeefU)
#define IPA_V2_MEM_UC_SHARED_OFF		0x0000
#define IPA_V2_MEM_UC_SHARED_SZ			0x0080
#define IPA_V2_MEM_UC_INFO_OFF			0x0080
#define IPA_V2_MEM_UC_INFO_SZ			0x0200
#define IPA_V2_MEM_V4_FLT_OFF			0x0288
#define IPA_V2_MEM_V4_FLT_SZ			0x0058
#define IPA_V2_MEM_V6_FLT_OFF			0x02e8
#define IPA_V2_MEM_V6_FLT_SZ			0x0058
#define IPA_V2_MEM_V4_RT_OFF			0x0348
#define IPA_V2_MEM_V4_RT_SZ			0x003c
#define IPA_V2_MEM_V4_MODEM_RT_MAX_IDX		6
#define IPA_V2_MEM_V6_RT_OFF			0x0388
#define IPA_V2_MEM_V6_RT_SZ			0x003c
#define IPA_V2_MEM_V6_MODEM_RT_MAX_IDX		6
#define IPA_V2_MEM_MODEM_HDR_OFF		0x03c8
#define IPA_V2_MEM_MODEM_HDR_SZ			0x0140
#define IPA_V2_MEM_MODEM_PROC_CTX_OFF		0x0510
#define IPA_V2_MEM_MODEM_PROC_CTX_SZ		0x0200
#define IPA_V2_MEM_AP_PROC_CTX_OFF		0x0710
#define IPA_V2_MEM_AP_PROC_CTX_SZ		0x0200
#define IPA_V2_MEM_MODEM_OFF			0x0914
#define IPA_V2_MEM_MODEM_SZ			0x16a8
#define IPA_V2_MEM_END				0x1fc0

/* QMI service constants for IPA v2.5 */
#define IPA_HOST_SERVICE_SVC_ID			0x31
#define IPA_HOST_SVC_VERS			1
#define IPA_HOST_SERVICE_INS_ID			1
#define IPA_MODEM_SERVICE_SVC_ID		0x31
#define IPA_MODEM_SERVICE_INS_ID		2
#define IPA_MODEM_SVC_VERS			1
#define IPA_V2_QMI_MAX_RCV_SZ			4096

#define IPA_V2_MODEM_RX_COUNT			32
#define IPA_V2_LAN_RX_COUNT			16
#define IPA_V2_RX_BUF_SIZE			8192
#define IPA_V2_MTU				2000

struct ipa_v2_uc_mem_area {
	u8 command;
	u8 reserved0[3];
	__le32 command_param;
	u8 response;
	u8 reserved1[3];
	__le32 response_param;
	u8 event;
	u8 reserved2[3];
	__le32 event_param;
	__le32 reserved3;
	__le32 first_error_address;
	u8 hw_state;
	u8 warning_counter;
	__le16 reserved4;
	__le16 interface_version;
	__le16 reserved5;
};

struct ipa_v2_dev;

struct ipa_v2_rx_buf {
	struct ipa_v2_dev *ipa;
	struct dma_chan *chan;
	void *buf;
	dma_addr_t dma;
	u32 rx_len;
	bool is_modem_rx;
};

struct ipa_v2_tx_ctx {
	struct ipa_v2_dev *ipa;
	struct sk_buff *skb;
	dma_addr_t dma;
};

struct ipa_v2_netdev_priv {
	struct ipa_v2_dev *ipa;
};

struct ipa_v2_dev {
	struct platform_device *pdev;
	struct device *dev;
	void __iomem *reg_virt;
	void *mem_virt;
	phys_addr_t mem_addr;
	size_t mem_size;
	u32 mem_offset;

	struct clk *core_clk;
	struct icc_path *icc_mem;
	struct icc_path *icc_imem;

	int irq;
	u32 irq_en;

	struct dma_chan *ch_lan_rx;
	struct dma_chan *ch_cmd_tx;
	struct dma_chan *ch_modem_tx;
	struct dma_chan *ch_modem_rx;

	__le32 *table_virt;
	dma_addr_t table_dma;
	size_t table_size;

	void *zero_virt;
	dma_addr_t zero_dma;
	size_t zero_size;

	void *cmd_virt;
	dma_addr_t cmd_dma;

	struct ipa_v2_rx_buf lan_rx_pool[IPA_V2_LAN_RX_COUNT];
	struct ipa_v2_rx_buf modem_rx_pool[IPA_V2_MODEM_RX_COUNT];
	bool rx_active;
	bool modem_rx_posted;

	struct qmi_handle qmi_server;
	struct qmi_handle qmi_client;
	struct sockaddr_qrtr modem_sq;
	struct sockaddr_qrtr ind_sq;
	struct work_struct init_driver_work;

	struct mutex state_lock;
	bool modem_ready;
	bool indication_requested;
	bool indication_sent;
	bool uc_loaded;
	bool qmi_initialized;
	bool netdev_registered;

	struct net_device *netdev;
};

static inline void ipa_v2_write(struct ipa_v2_dev *ipa, u32 offset, u32 val)
{
	iowrite32(val, ipa->reg_virt + offset);
}

static inline u32 ipa_v2_read(struct ipa_v2_dev *ipa, u32 offset)
{
	return ioread32(ipa->reg_virt + offset);
}

static void ipa_v2_cmd_done_cb(void *arg)
{
	complete(arg);
}

static int ipa_v2_submit_cmd(struct ipa_v2_dev *ipa, u32 opcode,
			     const void *payload, size_t payload_len)
{
	struct dma_async_tx_descriptor *desc;
	struct completion done;
	dma_cookie_t cookie;
	unsigned long left;

	if (payload_len > 64)
		return -EINVAL;

	memcpy(ipa->cmd_virt, payload, payload_len);
	wmb();

	init_completion(&done);
	desc = dmaengine_prep_slave_single(ipa->ch_cmd_tx, ipa->cmd_dma,
					   opcode, DMA_MEM_TO_DEV,
					   DMA_PREP_IMM_CMD | DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err(ipa->dev, "failed to prep IPA imm cmd %u\n", opcode);
		return -ENOMEM;
	}

	desc->callback = ipa_v2_cmd_done_cb;
	desc->callback_param = &done;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		dev_err(ipa->dev, "failed to submit IPA imm cmd %u\n", opcode);
		return -EIO;
	}

	dma_async_issue_pending(ipa->ch_cmd_tx);
	left = wait_for_completion_timeout(&done, msecs_to_jiffies(2000));
	if (!left) {
		dev_err(ipa->dev, "timeout waiting for IPA imm cmd %u\n", opcode);
		return -ETIMEDOUT;
	}

	return 0;
}

static int ipa_v2_cmd_fltrt_init(struct ipa_v2_dev *ipa, u32 opcode,
				 dma_addr_t src_dma, u16 size,
				 u32 sram_off, bool ipv4)
{
	u32 local_off = ipa->mem_offset + sram_off;
	__le64 payload;

	if (ipv4)
		payload = cpu_to_le64((u64)(u32)src_dma |
				      ((u64)(size & 0xfffU) << 32) |
				      ((u64)(local_off & 0xffffU) << 44));
	else
		payload = cpu_to_le64((u64)(u32)src_dma |
				      ((u64)size << 32) |
				      ((u64)(local_off & 0xffffU) << 48));

	return ipa_v2_submit_cmd(ipa, opcode, &payload, sizeof(payload));
}

static int ipa_v2_cmd_dma_shared_mem(struct ipa_v2_dev *ipa, u32 sram_off,
				     u16 size, dma_addr_t sys_dma,
				     bool toward_ipa)
{
	u32 local_off = ipa->mem_offset + sram_off;
	__le32 payload[4];

	payload[0] = cpu_to_le32((u32)size << 16);
	payload[1] = cpu_to_le32((u32)sys_dma);
	payload[2] = cpu_to_le32((local_off & 0xffffU) |
				 (toward_ipa ? 0U : BIT(16)));
	payload[3] = 0;

	return ipa_v2_submit_cmd(ipa, IPA_V2_CMD_DMA_SHARED_MEM,
				 payload, sizeof(payload));
}

static int ipa_v2_rx_post_with_cb(struct ipa_v2_rx_buf *rxb);

static void ipa_v2_rx_complete(void *arg)
{
	struct ipa_v2_rx_buf *rxb = arg;
	struct ipa_v2_dev *ipa = rxb->ipa;
	u32 len = rxb->rx_len;

	dma_unmap_single(ipa->dev, rxb->dma, IPA_V2_RX_BUF_SIZE, DMA_FROM_DEVICE);

	if (rxb->is_modem_rx && ipa->netdev && netif_running(ipa->netdev)) {
		if (len > 0 && len <= IPA_V2_RX_BUF_SIZE) {
			struct sk_buff *skb;

			skb = netdev_alloc_skb_ip_align(ipa->netdev, len);
			if (skb) {
				skb_put_data(skb, rxb->buf, len);
				skb->protocol = htons(ETH_P_MAP);
				ipa->netdev->stats.rx_packets++;
				ipa->netdev->stats.rx_bytes += len;
				netif_rx(skb);
			} else {
				ipa->netdev->stats.rx_dropped++;
			}
		} else if (len > IPA_V2_RX_BUF_SIZE) {
			ipa->netdev->stats.rx_length_errors++;
			ipa->netdev->stats.rx_dropped++;
		}
	}

	if (ipa->rx_active && !ipa_v2_rx_post_with_cb(rxb))
		dma_async_issue_pending(rxb->chan);
}

static int ipa_v2_rx_post_with_cb(struct ipa_v2_rx_buf *rxb)
{
	struct ipa_v2_dev *ipa = rxb->ipa;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	int ret;

	if (!ipa->rx_active)
		return 0;

	rxb->rx_len = 0;
	rxb->dma = dma_map_single(ipa->dev, rxb->buf, IPA_V2_RX_BUF_SIZE,
				  DMA_FROM_DEVICE);
	if (dma_mapping_error(ipa->dev, rxb->dma))
		return -ENOMEM;

	desc = dmaengine_prep_slave_single(rxb->chan, rxb->dma,
					   IPA_V2_RX_BUF_SIZE, DMA_DEV_TO_MEM,
					   DMA_PREP_INTERRUPT);
	if (!desc) {
		dma_unmap_single(ipa->dev, rxb->dma, IPA_V2_RX_BUF_SIZE,
				 DMA_FROM_DEVICE);
		return -ENOMEM;
	}

	ret = dmaengine_desc_attach_metadata(desc, &rxb->rx_len,
					     sizeof(rxb->rx_len));
	if (ret) {
		dma_unmap_single(ipa->dev, rxb->dma, IPA_V2_RX_BUF_SIZE,
				 DMA_FROM_DEVICE);
		return ret;
	}

	desc->callback = ipa_v2_rx_complete;
	desc->callback_param = rxb;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		dma_unmap_single(ipa->dev, rxb->dma, IPA_V2_RX_BUF_SIZE,
				 DMA_FROM_DEVICE);
		return -EIO;
	}

	return 0;
}

static void ipa_v2_tx_complete(void *arg)
{
	struct ipa_v2_tx_ctx *ctx = arg;

	dma_unmap_single(ctx->ipa->dev, ctx->dma, ctx->skb->len, DMA_TO_DEVICE);
	dev_consume_skb_any(ctx->skb);
	kfree(ctx);
}

static int ipa_v2_net_open(struct net_device *netdev)
{
	struct ipa_v2_netdev_priv *priv = netdev_priv(netdev);
	struct ipa_v2_dev *ipa = priv->ipa;
	unsigned int i;
	int ret;

	if (!ipa->modem_rx_posted) {
		for (i = 0; i < IPA_V2_MODEM_RX_COUNT; i++) {
			ret = ipa_v2_rx_post_with_cb(&ipa->modem_rx_pool[i]);
			if (ret) {
				dev_err(ipa->dev,
					"failed posting modem RX buf %u: %d\n",
					i, ret);
				return ret;
			}
		}
		dma_async_issue_pending(ipa->ch_modem_rx);
		ipa->modem_rx_posted = true;
	}

	netif_start_queue(netdev);
	return 0;
}

static int ipa_v2_net_stop(struct net_device *netdev)
{
	netif_stop_queue(netdev);
	return 0;
}

static netdev_tx_t ipa_v2_net_xmit(struct sk_buff *skb,
				   struct net_device *netdev)
{
	struct ipa_v2_netdev_priv *priv = netdev_priv(netdev);
	struct ipa_v2_dev *ipa = priv->ipa;
	struct dma_async_tx_descriptor *desc;
	struct ipa_v2_tx_ctx *ctx;
	dma_cookie_t cookie;
	u32 skb_len = skb->len;

	if (!skb_len || skb->protocol != htons(ETH_P_MAP))
		goto drop;

	if (skb_linearize(skb))
		goto drop;

	ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx)
		return NETDEV_TX_BUSY;

	ctx->ipa = ipa;
	ctx->skb = skb;
	ctx->dma = dma_map_single(ipa->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(ipa->dev, ctx->dma)) {
		kfree(ctx);
		return NETDEV_TX_BUSY;
	}

	desc = dmaengine_prep_slave_single(ipa->ch_modem_tx, ctx->dma, skb->len,
					   DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
	if (!desc) {
		dma_unmap_single(ipa->dev, ctx->dma, skb->len, DMA_TO_DEVICE);
		kfree(ctx);
		return NETDEV_TX_BUSY;
	}

	desc->callback = ipa_v2_tx_complete;
	desc->callback_param = ctx;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		dma_unmap_single(ipa->dev, ctx->dma, skb->len, DMA_TO_DEVICE);
		kfree(ctx);
		return NETDEV_TX_BUSY;
	}

	dma_async_issue_pending(ipa->ch_modem_tx);
	netdev->stats.tx_packets++;
	netdev->stats.tx_bytes += skb_len;
	return NETDEV_TX_OK;

drop:
	dev_kfree_skb_any(skb);
	netdev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static const struct net_device_ops ipa_v2_netdev_ops = {
	.ndo_open	= ipa_v2_net_open,
	.ndo_stop	= ipa_v2_net_stop,
	.ndo_start_xmit	= ipa_v2_net_xmit,
};

static void ipa_v2_netdev_setup(struct net_device *netdev)
{
	netdev->netdev_ops = &ipa_v2_netdev_ops;
	netdev->header_ops = NULL;
	netdev->type = ARPHRD_RAWIP;
	netdev->hard_header_len = 0;
	netdev->min_header_len = ETH_HLEN;
	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = IPA_V2_MTU;
	netdev->mtu = IPA_V2_MTU;
	netdev->addr_len = 0;
	netdev->tx_queue_len = DEFAULT_TX_QUEUE_LEN;
	netdev->flags &= ~(IFF_BROADCAST | IFF_MULTICAST);
	netdev->priv_flags |= IFF_TX_SKB_SHARING;
	eth_broadcast_addr(netdev->broadcast);
	netdev->needed_headroom = sizeof(struct rmnet_map_header);
}

static void ipa_v2_qmi_try_ready_locked(struct ipa_v2_dev *ipa)
{
	struct ipa_v2_netdev_priv *priv;
	struct ipa_init_complete_ind ind = { };
	int ret;

	lockdep_assert_held(&ipa->state_lock);

	if (!ipa->modem_ready || !ipa->indication_requested)
		return;

	if (!ipa->indication_sent) {
		ind.status.result = QMI_RESULT_SUCCESS_V01;
		ind.status.error = QMI_ERR_NONE_V01;
		ret = qmi_send_indication(&ipa->qmi_server, &ipa->ind_sq,
					  IPA_QMI_INIT_COMPLETE,
					  IPA_QMI_INIT_COMPLETE_IND_SZ,
					  ipa_init_complete_ind_ei, &ind);
		if (ret) {
			dev_err(ipa->dev,
				"failed sending IPA_QMI_INIT_COMPLETE: %d\n",
				ret);
			return;
		}
		ipa->indication_sent = true;
		dev_info(ipa->dev, "sent QMI IPA_QMI_INIT_COMPLETE\n");
	}

	if (!ipa->netdev_registered) {
		priv = netdev_priv(ipa->netdev);
		priv->ipa = ipa;
		ret = register_netdev(ipa->netdev);
		if (ret) {
			dev_err(ipa->dev, "register_netdev(rmnet_ipa0) failed: %d\n",
				ret);
			return;
		}
		ipa->netdev_registered = true;
		netif_carrier_on(ipa->netdev);
		dev_info(ipa->dev, "registered %s\n", netdev_name(ipa->netdev));
	} else {
		netif_carrier_on(ipa->netdev);
	}
}

static void ipa_v2_qmi_server_bye(struct qmi_handle *qmi, unsigned int node)
{
	struct ipa_v2_dev *ipa = container_of(qmi, struct ipa_v2_dev, qmi_server);

	mutex_lock(&ipa->state_lock);
	ipa->modem_ready = false;
	ipa->indication_requested = false;
	ipa->indication_sent = false;
	if (ipa->netdev_registered)
		netif_carrier_off(ipa->netdev);
	mutex_unlock(&ipa->state_lock);
}

static void ipa_v2_qmi_indication_register(struct qmi_handle *qmi,
					   struct sockaddr_qrtr *sq,
					   struct qmi_txn *txn,
					   const void *decoded)
{
	struct ipa_v2_dev *ipa = container_of(qmi, struct ipa_v2_dev, qmi_server);
	struct ipa_indication_register_rsp rsp = { };
	int ret;

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INDICATION_REGISTER,
				IPA_QMI_INDICATION_REGISTER_RSP_SZ,
				ipa_indication_register_rsp_ei, &rsp);
	if (ret) {
		dev_err(ipa->dev,
			"error %d sending INDICATION_REGISTER rsp\n", ret);
		return;
	}

	mutex_lock(&ipa->state_lock);
	ipa->ind_sq = *sq;
	ipa->indication_requested = true;
	ipa_v2_qmi_try_ready_locked(ipa);
	mutex_unlock(&ipa->state_lock);
}

static void ipa_v2_qmi_driver_init_complete(struct qmi_handle *qmi,
					     struct sockaddr_qrtr *sq,
					     struct qmi_txn *txn,
					     const void *decoded)
{
	struct ipa_v2_dev *ipa = container_of(qmi, struct ipa_v2_dev, qmi_server);
	struct ipa_driver_init_complete_rsp rsp = { };
	int ret;

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_DRIVER_INIT_COMPLETE,
				IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ,
				ipa_driver_init_complete_rsp_ei, &rsp);
	if (ret) {
		dev_err(ipa->dev,
			"error %d sending DRIVER_INIT_COMPLETE rsp\n", ret);
		return;
	}

	mutex_lock(&ipa->state_lock);
	ipa->uc_loaded = true;
	ipa_v2_qmi_try_ready_locked(ipa);
	mutex_unlock(&ipa->state_lock);
}

static void ipa_v2_qmi_server_msg_handler(struct qmi_handle *qmi,
					  struct sockaddr_qrtr *sq,
					  const void *data, size_t count)
{
	struct ipa_v2_dev *ipa = container_of(qmi, struct ipa_v2_dev, qmi_server);
	const struct qmi_header *hdr = data;
	struct qmi_txn txn = { };
	u16 msg_id;

	if (count < sizeof(struct qmi_header))
		return;

	if (hdr->type != QMI_REQUEST)
		return;

	msg_id = le16_to_cpu(hdr->msg_id);
	txn.id = le16_to_cpu(hdr->txn_id);

	if (msg_id == IPA_QMI_INDICATION_REGISTER) {
		ipa_v2_qmi_indication_register(qmi, sq, &txn, NULL);
	} else if (msg_id == IPA_QMI_DRIVER_INIT_COMPLETE) {
		ipa_v2_qmi_driver_init_complete(qmi, sq, &txn, NULL);
	} else {
		struct ipa_indication_register_rsp rsp = {
			.rsp = {
				.result = QMI_RESULT_SUCCESS_V01,
				.error = QMI_ERR_NONE_V01,
			},
		};

		dev_info(ipa->dev,
			 "IPA QMI server req msg_id=%#x len=%zu (replying OK)\n",
			 msg_id, count);
		qmi_send_response(qmi, sq, &txn, msg_id,
				  IPA_QMI_INDICATION_REGISTER_RSP_SZ,
				  ipa_indication_register_rsp_ei, &rsp);
	}
}

static const struct qmi_ops ipa_v2_qmi_server_ops = {
	.bye = ipa_v2_qmi_server_bye,
	.msg_handler = ipa_v2_qmi_server_msg_handler,
};

static void ipa_v2_qmi_init_driver_rsp_cb(struct qmi_handle *qmi,
					  struct sockaddr_qrtr *sq,
					  struct qmi_txn *txn,
					  const void *decoded)
{
	const struct ipa_init_modem_driver_rsp *rsp = decoded;
	struct ipa_v2_dev *ipa = container_of(qmi, struct ipa_v2_dev, qmi_client);

	if (rsp && rsp->rsp.result != QMI_RESULT_SUCCESS_V01) {
		dev_err(ipa->dev, "IPA_QMI_INIT_DRIVER failed: res=%u err=%u\n",
			rsp->rsp.result, rsp->rsp.error);
		txn->result = -EPROTO;
	} else {
		txn->result = 0;
	}
	complete(&txn->completion);
}

static const struct qmi_msg_handler ipa_v2_qmi_client_handlers[] = {
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_INIT_DRIVER,
		.ei		= ipa_init_modem_driver_rsp_ei,
		.decoded_size	= IPA_QMI_INIT_DRIVER_RSP_SZ,
		.fn		= ipa_v2_qmi_init_driver_rsp_cb,
	},
	{ },
};

static void ipa_v2_init_driver_work(struct work_struct *work)
{
	struct ipa_v2_dev *ipa = container_of(work, struct ipa_v2_dev,
					      init_driver_work);
	struct ipa_init_modem_driver_req req = { };
	struct qmi_txn txn;
	u32 base = ipa->mem_offset;
	int ret;

	req.platform_type_valid = 1;
	req.platform_type = IPA_QMI_PLATFORM_TYPE_MSM_ANDROID;

	req.hdr_tbl_info_valid = 1;
	req.hdr_tbl_info.start = base + IPA_V2_MEM_MODEM_HDR_OFF;
	req.hdr_tbl_info.end = req.hdr_tbl_info.start + IPA_V2_MEM_MODEM_HDR_SZ - 1;

	req.v4_route_tbl_info_valid = 1;
	req.v4_route_tbl_info.start = base + IPA_V2_MEM_V4_RT_OFF;
	req.v4_route_tbl_info.end = IPA_V2_MEM_V4_MODEM_RT_MAX_IDX;

	req.v6_route_tbl_info_valid = 1;
	req.v6_route_tbl_info.start = base + IPA_V2_MEM_V6_RT_OFF;
	req.v6_route_tbl_info.end = IPA_V2_MEM_V6_MODEM_RT_MAX_IDX;

	req.v4_filter_tbl_start_valid = 1;
	req.v4_filter_tbl_start = base + IPA_V2_MEM_V4_FLT_OFF;

	req.v6_filter_tbl_start_valid = 1;
	req.v6_filter_tbl_start = base + IPA_V2_MEM_V6_FLT_OFF;

	req.modem_mem_info_valid = 1;
	req.modem_mem_info.start = base + IPA_V2_MEM_MODEM_OFF;
	req.modem_mem_info.size = IPA_V2_MEM_MODEM_SZ;

	req.ctrl_comm_dest_end_pt_valid = 1;
	req.ctrl_comm_dest_end_pt = IPA_V2_EP_AP_MODEM_RX;

	req.skip_uc_load_valid = ipa->uc_loaded ? 1 : 0;
	req.skip_uc_load = ipa->uc_loaded ? 1 : 0;

	req.hdr_proc_ctx_tbl_info_valid = 1;
	req.hdr_proc_ctx_tbl_info.start = base + IPA_V2_MEM_MODEM_PROC_CTX_OFF;
	req.hdr_proc_ctx_tbl_info.end =
		req.hdr_proc_ctx_tbl_info.start + IPA_V2_MEM_MODEM_PROC_CTX_SZ - 1;

	ret = qmi_txn_init(&ipa->qmi_client, &txn, NULL, NULL);
	if (ret < 0) {
		dev_err(ipa->dev, "qmi_txn_init failed: %d\n", ret);
		return;
	}

	ret = qmi_send_request(&ipa->qmi_client, &ipa->modem_sq, &txn,
			       IPA_QMI_INIT_DRIVER, IPA_QMI_INIT_DRIVER_REQ_SZ,
			       ipa_init_modem_driver_req_ei, &req);
	if (ret) {
		dev_err(ipa->dev, "error %d sending INIT_DRIVER\n", ret);
		qmi_txn_cancel(&txn);
		return;
	}

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(30000));
	if (ret < 0) {
		dev_err(ipa->dev, "error %d awaiting INIT_DRIVER rsp\n", ret);
		qmi_txn_cancel(&txn);
		return;
	}

	dev_info(ipa->dev, "QMI IPA_QMI_INIT_DRIVER succeeded\n");
	mutex_lock(&ipa->state_lock);
	ipa->modem_ready = true;
	ipa_v2_qmi_try_ready_locked(ipa);
	mutex_unlock(&ipa->state_lock);
}

static int ipa_v2_qmi_new_server(struct qmi_handle *qmi,
				 struct qmi_service *svc)
{
	struct ipa_v2_dev *ipa = container_of(qmi, struct ipa_v2_dev, qmi_client);

	ipa->modem_sq.sq_family = AF_QIPCRTR;
	ipa->modem_sq.sq_node = svc->node;
	ipa->modem_sq.sq_port = svc->port;

	dev_info(ipa->dev, "modem IPA QMI service appeared (%u:%u)\n",
		 svc->node, svc->port);
	schedule_work(&ipa->init_driver_work);
	return 0;
}

static const struct qmi_ops ipa_v2_qmi_client_ops = {
	.new_server = ipa_v2_qmi_new_server,
};

static irqreturn_t ipa_v2_isr_thread(int irq, void *dev_id)
{
	struct ipa_v2_dev *ipa = dev_id;
	struct ipa_v2_uc_mem_area *uc;
	u32 stts, pending, mask;

	uc = ipa->mem_virt + ipa->mem_offset + IPA_V2_MEM_UC_SHARED_OFF;
	stts = ipa_v2_read(ipa, IPA_V2_REG_IRQ_STTS);

	while ((mask = stts & ipa->irq_en)) {
		do {
			u32 bit = __ffs(mask);
			u32 clr = BIT(bit);

			mask ^= clr;
			ipa_v2_write(ipa, IPA_V2_REG_IRQ_CLR, clr);

			if (bit == IPA_V2_IRQ_UC_0) {
				if (uc->event == 1)
					dev_err(ipa->dev, "IPA uC error event\n");
			} else if (bit == IPA_V2_IRQ_UC_1) {
				if (uc->response == 1) {
					mutex_lock(&ipa->state_lock);
					ipa->uc_loaded = true;
					ipa_v2_qmi_try_ready_locked(ipa);
					mutex_unlock(&ipa->state_lock);
					dev_info(ipa->dev, "IPA uC init completed\n");
				}
			}
		} while (mask);

		stts = ipa_v2_read(ipa, IPA_V2_REG_IRQ_STTS);
	}

	pending = stts & ~ipa->irq_en;
	if (pending)
		ipa_v2_write(ipa, IPA_V2_REG_IRQ_CLR, pending);

	return IRQ_HANDLED;
}

static void ipa_v2_program_endpoints(struct ipa_v2_dev *ipa)
{
	/* Pipe 2: AP_LAN_RX (exception / command completions, 4B pad, generic aggr) */
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_CTRL(IPA_V2_EP_AP_LAN_RX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_CFG(IPA_V2_EP_AP_LAN_RX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR(IPA_V2_EP_AP_LAN_RX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR_EXT(IPA_V2_EP_AP_LAN_RX),
		     2U << 10);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR_MD_MASK(IPA_V2_EP_AP_LAN_RX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_AGGR(IPA_V2_EP_AP_LAN_RX),
		     1U | (0U << 2) | (2U << 5) | (1U << 10));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_STATUS(IPA_V2_EP_AP_LAN_RX), BIT(0));

	/* Pipe 3: AP_CMD_TX (immediate commands -> pipe 2) */
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_CTRL(IPA_V2_EP_AP_CMD_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_MODE(IPA_V2_EP_AP_CMD_TX),
		     3U | (IPA_V2_EP_AP_LAN_RX << 4));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_AGGR(IPA_V2_EP_AP_CMD_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_DEAGGR(IPA_V2_EP_AP_CMD_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_STATUS(IPA_V2_EP_AP_CMD_TX), 0);

	/* Pipe 4: AP_MODEM_TX (QMAP 4B header, no csum offload, status -> pipe 2) */
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_CTRL(IPA_V2_EP_AP_MODEM_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_CFG(IPA_V2_EP_AP_MODEM_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_NAT(IPA_V2_EP_AP_MODEM_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR(IPA_V2_EP_AP_MODEM_TX),
		     4U | BIT(6));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR_EXT(IPA_V2_EP_AP_MODEM_TX),
		     BIT(0));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_MODE(IPA_V2_EP_AP_MODEM_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_AGGR(IPA_V2_EP_AP_MODEM_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_DEAGGR(IPA_V2_EP_AP_MODEM_TX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_STATUS(IPA_V2_EP_AP_MODEM_TX),
		     BIT(0) | (IPA_V2_EP_AP_LAN_RX << 1));

	/* Pipe 5: AP_MODEM_RX (QMAP 4B header + QMAP aggr, no csum offload) */
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_CTRL(IPA_V2_EP_AP_MODEM_RX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_CFG(IPA_V2_EP_AP_MODEM_RX), 0);
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR(IPA_V2_EP_AP_MODEM_RX),
		     4U | BIT(6) | (1U << 7) | BIT(19) | (2U << 20));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR_EXT(IPA_V2_EP_AP_MODEM_RX),
		     BIT(0) | BIT(1) | BIT(3));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_HDR_MD_MASK(IPA_V2_EP_AP_MODEM_RX),
		     (__force u32)cpu_to_be32(0x000000ffU));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_INIT_AGGR(IPA_V2_EP_AP_MODEM_RX),
		     1U | (3U << 2) | (4U << 5) | (1U << 10) | BIT(21));
	ipa_v2_write(ipa, IPA_V2_REG_ENDP_STATUS(IPA_V2_EP_AP_MODEM_RX), 0);

	/* Default route -> AP_LAN_RX (pipe 2) */
	ipa_v2_write(ipa, IPA_V2_REG_ROUTE,
		     (IPA_V2_EP_AP_LAN_RX << 1) | BIT(6) |
		     (IPA_V2_EP_AP_LAN_RX << 17) | BIT(24));
}

static void ipa_v2_write_sram_canaries(struct ipa_v2_dev *ipa)
{
	static const u32 canaries[] = {
		0x0280, 0x0284,
		0x02e0, 0x02e4,
		0x0340, 0x0344,
		0x0384,
		0x03c4,
		0x0508, 0x050c,
		0x0910,
		0x1fbc,
	};
	void *base = ipa->mem_virt + ipa->mem_offset;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(canaries); i++) {
		__le32 *p = base + canaries[i];
		*p = IPA_V2_MEM_CANARY_VAL;
	}
	wmb();
}

static int ipa_v2_init_tables_and_mem(struct ipa_v2_dev *ipa)
{
	const u32 count = 21;
	dma_addr_t tbl_list_dma;
	dma_addr_t zero_rule_dma;
	unsigned int i;
	int ret;

	/* Zero rule (1 x __le32) + filter bitmap (1 x __le32) + 21 entries */
	ipa->table_size = (2 + count) * sizeof(__le32);
	ipa->table_virt = dma_alloc_coherent(ipa->dev, ipa->table_size,
					     &ipa->table_dma, GFP_KERNEL);
	if (!ipa->table_virt)
		return -ENOMEM;

	ipa->zero_size = 0x1800;
	ipa->zero_virt = dma_alloc_coherent(ipa->dev, ipa->zero_size,
					    &ipa->zero_dma, GFP_KERNEL);
	if (!ipa->zero_virt)
		return -ENOMEM;

	ipa->cmd_virt = dma_alloc_coherent(ipa->dev, 64, &ipa->cmd_dma,
					   GFP_KERNEL);
	if (!ipa->cmd_virt)
		return -ENOMEM;

	ipa->table_virt[0] = 0;
	ipa->table_virt[1] = cpu_to_le32((0xfffffU << 1) | 0x1U);
	for (i = 0; i < count; i++)
		ipa->table_virt[2 + i] = cpu_to_le32((u32)ipa->table_dma);

	zero_rule_dma = ipa->table_dma;
	tbl_list_dma = zero_rule_dma + 8;

	ret = ipa_v2_cmd_fltrt_init(ipa, IPA_V2_CMD_IP_V4_ROUTING_INIT,
				    tbl_list_dma, IPA_V2_MEM_V4_RT_SZ,
				    IPA_V2_MEM_V4_RT_OFF, true);
	if (ret)
		return ret;

	ret = ipa_v2_cmd_fltrt_init(ipa, IPA_V2_CMD_IP_V6_ROUTING_INIT,
				    tbl_list_dma, IPA_V2_MEM_V6_RT_SZ,
				    IPA_V2_MEM_V6_RT_OFF, false);
	if (ret)
		return ret;

	ret = ipa_v2_cmd_fltrt_init(ipa, IPA_V2_CMD_IP_V4_FILTER_INIT,
				    zero_rule_dma + 4, IPA_V2_MEM_V4_FLT_SZ,
				    IPA_V2_MEM_V4_FLT_OFF, true);
	if (ret)
		return ret;

	ret = ipa_v2_cmd_fltrt_init(ipa, IPA_V2_CMD_IP_V6_FILTER_INIT,
				    zero_rule_dma + 4, IPA_V2_MEM_V6_FLT_SZ,
				    IPA_V2_MEM_V6_FLT_OFF, false);
	if (ret)
		return ret;

	/* Initialize local header table */
	ret = ipa_v2_cmd_fltrt_init(ipa, IPA_V2_CMD_HDR_INIT_LOCAL,
				    ipa->zero_dma, IPA_V2_MEM_MODEM_HDR_SZ,
				    IPA_V2_MEM_MODEM_HDR_OFF, true);
	if (ret)
		return ret;

	ret = ipa_v2_cmd_dma_shared_mem(ipa, IPA_V2_MEM_MODEM_PROC_CTX_OFF,
					IPA_V2_MEM_MODEM_PROC_CTX_SZ,
					ipa->zero_dma, true);
	if (ret)
		return ret;

	ret = ipa_v2_cmd_dma_shared_mem(ipa, IPA_V2_MEM_AP_PROC_CTX_OFF,
					IPA_V2_MEM_AP_PROC_CTX_SZ,
					ipa->zero_dma, true);
	if (ret)
		return ret;

	ret = ipa_v2_cmd_dma_shared_mem(ipa, IPA_V2_MEM_MODEM_OFF,
					IPA_V2_MEM_MODEM_SZ,
					ipa->zero_dma, true);
	if (ret)
		return ret;

	ipa_v2_write(ipa, IPA_V2_REG_LOCAL_PKT_PROC_CNTXT_BASE,
		     ipa->mem_offset + IPA_V2_MEM_MODEM_PROC_CTX_OFF);

	return 0;
}

static int ipa_v2_alloc_rx_pools(struct ipa_v2_dev *ipa)
{
	unsigned int order = get_order(IPA_V2_RX_BUF_SIZE);
	unsigned int i;

	for (i = 0; i < IPA_V2_LAN_RX_COUNT; i++) {
		ipa->lan_rx_pool[i].ipa = ipa;
		ipa->lan_rx_pool[i].chan = ipa->ch_lan_rx;
		ipa->lan_rx_pool[i].is_modem_rx = false;
		ipa->lan_rx_pool[i].buf =
			(void *)__get_free_pages(GFP_KERNEL | GFP_DMA32, order);
		if (!ipa->lan_rx_pool[i].buf)
			return -ENOMEM;
	}

	for (i = 0; i < IPA_V2_MODEM_RX_COUNT; i++) {
		ipa->modem_rx_pool[i].ipa = ipa;
		ipa->modem_rx_pool[i].chan = ipa->ch_modem_rx;
		ipa->modem_rx_pool[i].is_modem_rx = true;
		ipa->modem_rx_pool[i].buf =
			(void *)__get_free_pages(GFP_KERNEL | GFP_DMA32, order);
		if (!ipa->modem_rx_pool[i].buf)
			return -ENOMEM;
	}

	return 0;
}

static void ipa_v2_free_rx_pools(struct ipa_v2_dev *ipa)
{
	unsigned int order = get_order(IPA_V2_RX_BUF_SIZE);
	unsigned int i;

	for (i = 0; i < IPA_V2_LAN_RX_COUNT; i++) {
		if (ipa->lan_rx_pool[i].buf) {
			free_pages((unsigned long)ipa->lan_rx_pool[i].buf,
				   order);
			ipa->lan_rx_pool[i].buf = NULL;
		}
	}
	for (i = 0; i < IPA_V2_MODEM_RX_COUNT; i++) {
		if (ipa->modem_rx_pool[i].buf) {
			free_pages((unsigned long)ipa->modem_rx_pool[i].buf,
				   order);
			ipa->modem_rx_pool[i].buf = NULL;
		}
	}
}

static int ipa_v2_request_dma(struct ipa_v2_dev *ipa)
{
	struct dma_slave_config cfg_rx = {
		.direction = DMA_DEV_TO_MEM,
		.src_maxburst = 16,
	};
	struct dma_slave_config cfg_tx = {
		.direction = DMA_MEM_TO_DEV,
		.dst_maxburst = 16,
	};

	ipa->ch_lan_rx = dma_request_chan(ipa->dev, "ap_lan_rx");
	if (IS_ERR(ipa->ch_lan_rx))
		return PTR_ERR(ipa->ch_lan_rx);

	ipa->ch_cmd_tx = dma_request_chan(ipa->dev, "cmd_tx");
	if (IS_ERR(ipa->ch_cmd_tx))
		return PTR_ERR(ipa->ch_cmd_tx);

	ipa->ch_modem_tx = dma_request_chan(ipa->dev, "ap_modem_tx");
	if (IS_ERR(ipa->ch_modem_tx))
		return PTR_ERR(ipa->ch_modem_tx);

	ipa->ch_modem_rx = dma_request_chan(ipa->dev, "ap_modem_rx");
	if (IS_ERR(ipa->ch_modem_rx))
		return PTR_ERR(ipa->ch_modem_rx);

	dmaengine_slave_config(ipa->ch_lan_rx, &cfg_rx);
	dmaengine_slave_config(ipa->ch_cmd_tx, &cfg_tx);
	dmaengine_slave_config(ipa->ch_modem_tx, &cfg_tx);
	dmaengine_slave_config(ipa->ch_modem_rx, &cfg_rx);

	return 0;
}

static void ipa_v2_release_dma(struct ipa_v2_dev *ipa)
{
	if (!IS_ERR_OR_NULL(ipa->ch_modem_rx)) {
		dmaengine_terminate_sync(ipa->ch_modem_rx);
		dma_release_channel(ipa->ch_modem_rx);
	}
	if (!IS_ERR_OR_NULL(ipa->ch_modem_tx)) {
		dmaengine_terminate_sync(ipa->ch_modem_tx);
		dma_release_channel(ipa->ch_modem_tx);
	}
	if (!IS_ERR_OR_NULL(ipa->ch_cmd_tx)) {
		dmaengine_terminate_sync(ipa->ch_cmd_tx);
		dma_release_channel(ipa->ch_cmd_tx);
	}
	if (!IS_ERR_OR_NULL(ipa->ch_lan_rx)) {
		dmaengine_terminate_sync(ipa->ch_lan_rx);
		dma_release_channel(ipa->ch_lan_rx);
	}
}

static int ipa_v2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ipa_v2_dev *ipa;
	struct resource *res;
	u32 hw_ver, smem_reg, smem_sz;
	unsigned int i;
	int ret;

	ipa = devm_kzalloc(dev, sizeof(*ipa), GFP_KERNEL);
	if (!ipa)
		return -ENOMEM;

	ipa->pdev = pdev;
	ipa->dev = dev;
	mutex_init(&ipa->state_lock);
	INIT_WORK(&ipa->init_driver_work, ipa_v2_init_driver_work);
	platform_set_drvdata(pdev, ipa);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "32-bit DMA mask failed: %d\n", ret);
		return ret;
	}

	ipa->reg_virt = devm_platform_ioremap_resource_byname(pdev, "ipa-reg");
	if (IS_ERR(ipa->reg_virt))
		return PTR_ERR(ipa->reg_virt);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ipa-shared");
	if (!res)
		return -ENODEV;

	ipa->mem_virt = memremap(res->start, resource_size(res), MEMREMAP_WC);
	if (!ipa->mem_virt)
		return -ENOMEM;
	ipa->mem_addr = res->start;
	ipa->mem_size = resource_size(res);

	ipa->core_clk = devm_clk_get(dev, "core");
	if (IS_ERR(ipa->core_clk)) {
		ret = PTR_ERR(ipa->core_clk);
		goto err_unmap_mem;
	}

	ret = clk_prepare_enable(ipa->core_clk);
	if (ret) {
		dev_err(dev, "clk_prepare_enable failed: %d\n", ret);
		goto err_unmap_mem;
	}

	ret = clk_set_rate(ipa->core_clk, 200 * 1000 * 1000);
	if (ret)
		dev_warn(dev, "clk_set_rate(200MHz) returned %d\n", ret);

	ipa->icc_mem = devm_of_icc_get(dev, "memory");
	if (IS_ERR(ipa->icc_mem)) {
		ret = PTR_ERR(ipa->icc_mem);
		if (ret == -EPROBE_DEFER)
			goto err_clk_disable;
		dev_warn(dev, "memory icc get failed (%d), continuing\n", ret);
		ipa->icc_mem = NULL;
	}

	ipa->icc_imem = devm_of_icc_get(dev, "imem");
	if (IS_ERR(ipa->icc_imem)) {
		ret = PTR_ERR(ipa->icc_imem);
		if (ret == -EPROBE_DEFER)
			goto err_clk_disable;
		dev_warn(dev, "imem icc get failed (%d), continuing\n", ret);
		ipa->icc_imem = NULL;
	}

	if (ipa->icc_mem)
		icc_set_bw(ipa->icc_mem, 100000, 1200000);
	if (ipa->icc_imem)
		icc_set_bw(ipa->icc_imem, 0, 350000);

	ipa->irq = platform_get_irq_byname(pdev, "ipa");
	if (ipa->irq < 0) {
		ret = ipa->irq;
		goto err_clk_disable;
	}

	ret = ipa_v2_request_dma(ipa);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "BAM DMA channel request failed: %d\n", ret);
		goto err_release_dma;
	}

	dev_info(dev, "probing IPA v2.5 (clk=%lu icc_mem=%d icc_imem=%d)\n",
		 clk_get_rate(ipa->core_clk), !!ipa->icc_mem, !!ipa->icc_imem);

	/* Soft-reset and enable IPA hardware */
	ipa_v2_write(ipa, IPA_V2_REG_COMP_SW_RESET, 1);
	wmb();
	ipa_v2_write(ipa, IPA_V2_REG_COMP_SW_RESET, 0);
	wmb();
	ipa_v2_write(ipa, IPA_V2_REG_COMP_CFG, 1);

	hw_ver = ipa_v2_read(ipa, IPA_V2_REG_VERSION);
	if (!hw_ver) {
		dev_err(dev, "IPA version register read 0\n");
		ret = -EFAULT;
		goto err_release_dma;
	}

	ipa_v2_write(ipa, IPA_V2_REG_BCR, IPA_V2_BCR_VAL);
	ipa_v2_write(ipa, IPA_V2_REG_COUNTER_CFG, 15U << 4);

	smem_reg = ipa_v2_read(ipa, IPA_V2_REG_SHARED_MEM_SIZE);
	ipa->mem_offset = FIELD_GET(IPA_V2_SHARED_MEM_BADDR_FMASK, smem_reg);
	smem_sz = FIELD_GET(IPA_V2_SHARED_MEM_SIZE_FMASK, smem_reg);

	dev_info(dev, "IPA v2.5 version=%#x smem_reg=%#x (off=%#x sz=%#x res=%#zx)\n",
		 hw_ver, smem_reg, ipa->mem_offset, smem_sz, ipa->mem_size);

	if (ipa->mem_offset + IPA_V2_MEM_END > ipa->mem_size) {
		dev_err(dev, "SRAM region (%#x + %#x) exceeds mapped size %#zx\n",
			ipa->mem_offset, IPA_V2_MEM_END, ipa->mem_size);
		ret = -EINVAL;
		goto err_release_dma;
	}

	ipa_v2_write_sram_canaries(ipa);
	ipa_v2_program_endpoints(ipa);

	ret = ipa_v2_alloc_rx_pools(ipa);
	if (ret)
		goto err_free_rx;

	ipa->rx_active = true;
	for (i = 0; i < IPA_V2_LAN_RX_COUNT; i++) {
		ret = ipa_v2_rx_post_with_cb(&ipa->lan_rx_pool[i]);
		if (ret)
			goto err_free_rx;
	}
	dma_async_issue_pending(ipa->ch_lan_rx);

	dev_info(dev, "IPA endpoints configured, submitting init commands\n");

	ret = ipa_v2_init_tables_and_mem(ipa);
	if (ret) {
		dev_err(dev, "IPA table/mem init failed: %d\n", ret);
		goto err_free_rx;
	}

	ipa->irq_en = BIT(IPA_V2_IRQ_UC_0) | BIT(IPA_V2_IRQ_UC_1);
	ipa_v2_write(ipa, IPA_V2_REG_IRQ_EN, 0);
	ret = devm_request_threaded_irq(dev, ipa->irq, NULL, ipa_v2_isr_thread,
					IRQF_ONESHOT, "ipa", ipa);
	if (ret) {
		dev_err(dev, "failed requesting IPA IRQ: %d\n", ret);
		goto err_free_rx;
	}
	ipa_v2_write(ipa, IPA_V2_REG_IRQ_EN, ipa->irq_en);

	ipa->netdev = alloc_netdev(sizeof(struct ipa_v2_netdev_priv),
				   "rmnet_ipa%d", NET_NAME_UNKNOWN,
				   ipa_v2_netdev_setup);
	if (!ipa->netdev) {
		ret = -ENOMEM;
		goto err_free_rx;
	}
	SET_NETDEV_DEV(ipa->netdev, dev);

	ret = qmi_handle_init(&ipa->qmi_server, IPA_V2_QMI_MAX_RCV_SZ,
			      &ipa_v2_qmi_server_ops,
			      NULL);
	if (ret)
		goto err_free_netdev;

	ret = qmi_add_server(&ipa->qmi_server, IPA_HOST_SERVICE_SVC_ID,
			     IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID);
	if (ret)
		goto err_qmi_server_rel;

	ret = qmi_handle_init(&ipa->qmi_client, IPA_V2_QMI_MAX_RCV_SZ,
			      &ipa_v2_qmi_client_ops,
			      ipa_v2_qmi_client_handlers);
	if (ret)
		goto err_qmi_server_rel;

	ret = qmi_add_lookup(&ipa->qmi_client, IPA_MODEM_SERVICE_SVC_ID,
			     IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);
	if (ret)
		goto err_qmi_client_rel;

	ipa->qmi_initialized = true;
	dev_info(dev, "MSM8996 IPA v2.5 BAM driver initialized\n");
	return 0;

err_qmi_client_rel:
	qmi_handle_release(&ipa->qmi_client);
err_qmi_server_rel:
	qmi_handle_release(&ipa->qmi_server);
err_free_netdev:
	free_netdev(ipa->netdev);
err_free_rx:
	ipa->rx_active = false;
	ipa_v2_release_dma(ipa);
	ipa_v2_free_rx_pools(ipa);
	if (ipa->cmd_virt)
		dma_free_coherent(dev, 64, ipa->cmd_virt, ipa->cmd_dma);
	if (ipa->zero_virt)
		dma_free_coherent(dev, ipa->zero_size, ipa->zero_virt,
				  ipa->zero_dma);
	if (ipa->table_virt)
		dma_free_coherent(dev, ipa->table_size, ipa->table_virt,
				  ipa->table_dma);
	goto err_clk_disable;
err_release_dma:
	ipa_v2_release_dma(ipa);
err_clk_disable:
	clk_disable_unprepare(ipa->core_clk);
err_unmap_mem:
	memunmap(ipa->mem_virt);
	return ret;
}

static void ipa_v2_remove(struct platform_device *pdev)
{
	struct ipa_v2_dev *ipa = platform_get_drvdata(pdev);

	ipa_v2_write(ipa, IPA_V2_REG_IRQ_EN, 0);
	cancel_work_sync(&ipa->init_driver_work);

	if (ipa->qmi_initialized) {
		qmi_handle_release(&ipa->qmi_client);
		qmi_handle_release(&ipa->qmi_server);
	}
	if (ipa->netdev_registered)
		unregister_netdev(ipa->netdev);
	if (ipa->netdev)
		free_netdev(ipa->netdev);

	ipa->rx_active = false;
	ipa_v2_release_dma(ipa);
	ipa_v2_free_rx_pools(ipa);

	if (ipa->cmd_virt)
		dma_free_coherent(ipa->dev, 64, ipa->cmd_virt, ipa->cmd_dma);
	if (ipa->zero_virt)
		dma_free_coherent(ipa->dev, ipa->zero_size, ipa->zero_virt,
				  ipa->zero_dma);
	if (ipa->table_virt)
		dma_free_coherent(ipa->dev, ipa->table_size, ipa->table_virt,
				  ipa->table_dma);

	clk_disable_unprepare(ipa->core_clk);
	memunmap(ipa->mem_virt);
}

static const struct of_device_id ipa_v2_match[] = {
	{ .compatible = "qcom,msm8996-ipa" },
	{ }
};
MODULE_DEVICE_TABLE(of, ipa_v2_match);

static struct platform_driver ipa_v2_driver = {
	.probe	= ipa_v2_probe,
	.remove	= ipa_v2_remove,
	.driver	= {
		.name		= "ipa_v2",
		.of_match_table	= ipa_v2_match,
	},
};

module_platform_driver(ipa_v2_driver);

MODULE_DESCRIPTION("Qualcomm IPA v2.5 (BAM) driver for MSM8996");
MODULE_LICENSE("GPL");
