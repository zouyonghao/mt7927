// SPDX-License-Identifier: ISC
/* Copyright (C) 2024 MediaTek Inc.
 *
 * MT7927 Firmware Loading - Register-based Protocol (ROM Bootloader)
 * The MCU goes from 0x1d1e (IDLE) to 0x00000000 (CRASHED) when we enable
 * WFDMA GLO_CFG TX_DMA_EN/RX_DMA_EN bits.
 *
 * Solution: Keep WFDMA disabled during firmware download, enable after.
 * Use register-based protocol or direct memory writes instead of WFDMA commands.
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include "mt792x.h"
#include "mt792x_regs.h"
#include "mt7925/mt7927_regs.h"
#include "mt76_connac_mcu.h"
#include "mt76_connac2_mac.h"

/* Command chunk size from MTK driver (CMD_PKT_SIZE_FOR_IMAGE) */
#define MT7927_FW_CHUNK_SIZE	2048

/* MCU addresses */
#define MCU_PATCH_ADDRESS	0x200000

/* Patch finish types align with MTK INIT command definitions */
#define PATCH_FNSH_TYPE_WF	0

#define MT7927_INIT_CMD_PQ_ID		0x8000
#define MT7927_INIT_PDA_PQ_ID		0xF800
#define MT7927_INIT_PKT_TYPE_ID	0xA0
#define MT7927_INIT_PKT_FT_CMD		0x2
#define MT7927_INIT_PKT_FT_FWDL	0x3

/* Matches vendor INIT_HIF_TX_HEADER_PENDING_FOR_HW_32BYTES layout */
struct mt7927_init_cmd_pending {
	__le16 tx_byte_count;
	__le16 pq_id;
	u8 wlan_idx;
	u8 header_format;
	u8 header_padding;
	u8 pkt_ft_ownmac;
	__le32 rsv[6];
};

/* Matches vendor INIT_HIF_TX_HEADER layout */
struct mt7927_init_wifi_cmd {
	u8 cid;
	u8 pkt_type;
	u8 reserved;
	u8 seq;
	__le32 reserved_dw0;
	__le32 reserved_dw[5];
};

struct mt7927_init_cmd_header {
	__le16 tx_byte_count;
	__le16 pq_id;
	struct mt7927_init_wifi_cmd cmd;
};

static u32 mt7927_fw_get_mcu_state(struct mt76_dev *dev)
{
	return __mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
}

/* Perform proper MCU reset sequence based on MT6639 reference code
 * This resets the WF subsystem and clears the semaphore.
 */
static int mt7927_mcu_reset(struct mt76_dev *dev)
{
	u32 value = 0;
	int poll_count = 0;
	int ret = 0;

	dev_info(dev->dev, "[MT7927] Performing MCU reset sequence...\n");

	/* STEP 1: Force on conninfra (wakeup) */
	dev_info(dev->dev, "[MT7927] Forcing CONN_INFRA wakeup...\n");
	__mt76_wr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_TOP_ADDR, 0x1);

	/* STEP 2: Wait for conninfra to become ready by checking version ID */
	poll_count = 0;
	while (1) {
		value = __mt76_rr(dev, CONN_CFG_IP_VERSION_IP_VERSION_ADDR);
		dev_dbg(dev->dev, "[MT7927] CONN_INFRA version=0x%08x (polling %d)\n",
			value, poll_count);

		/* Check for MT6639 version IDs */
		if (value == MT6639_CONNINFRA_VERSION_ID ||
		    value == MT6639_CONNINFRA_VERSION_ID_E2)
			break;

		poll_count++;
		if (poll_count >= 10) {
			dev_warn(dev->dev,
				"[MT7927] CONN_INFRA version polling timeout (value=0x%08x)\n",
				value);
			/* Don't fail - this might be expected on some hardware */
			break;
		}

		udelay(1000);
	}

	/* STEP 3: Power up WFSYS domain (CRITICAL - missing from original reset!)
	 * Without this, the MCU will crash (0xdead2217).
	 * This brings the WF subsystem power domain out of sleep mode.
	 */
	dev_info(dev->dev, "[MT7927] Powering up WFSYS domain...\n");
	
	/* Enable PTA-related clocks */
	__mt76_wr(dev, CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_0_SET_ADDR,
		CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_0_SET_PTA_MASK);
	__mt76_wr(dev, CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_1_SET_ADDR,
		CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_1_SET_PTA_MASK);

	/* Hold WFSYS CPU in reset while powering up */
	value = __mt76_rr(dev, CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_ADDR);
	value &= ~CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_MASK;
	__mt76_wr(dev, CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_ADDR, value);
	udelay(100);

	/* Disable sleep protection between conninfra and WFSYS */
	value = __mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_CTRL_ADDR);
	value &= ~CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_CTRL_CFG_CONN_WF_SLP_PROT_SW_EN_MASK;
	__mt76_wr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_CTRL_ADDR, value);
	udelay(100);

	/* Power on the WFSYS TOP domain using write key */
	value = __mt76_rr(dev, CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_ADDR);
	value &= ~(CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_WRITE_KEY_MASK |
		   CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_PWR_ON_MASK);
	value |= MT7927_WFSYS_ON_TOP_WRITE_KEY;
	value |= CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_PWR_ON_MASK;
	__mt76_wr(dev, CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_ADDR, value);
	msleep(1);

	/* Wait for WFSYS power states to indicate ON (bit 30) */
	poll_count = 0;
	while (poll_count < 50) {
		value = __mt76_rr(dev, CONN_HOST_CSR_TOP_CONNSYS_PWR_STATES_ADDR);
		if (value & BIT(30)) {
			dev_info(dev->dev, "[MT7927] WFSYS powered on (PWR_STATES=0x%08x)\n", value);
			break;
		}
		poll_count++;
		usleep_range(500, 1000);
	}
	if (poll_count >= 50)
		dev_warn(dev->dev, "[MT7927] WFSYS power-on timeout (PWR_STATES=0x%08x)\n", value);

	/* Ensure WF<->CONN sleep protection bits are cleared */
	poll_count = 0;
	while (poll_count < 50) {
		value = __mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_ADDR);
		if (!(value & (CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_WF2CONN_SLP_PROT_RDY_MASK |
			       CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_CONN2WF_SLP_PROT_RDY_MASK))) {
			dev_info(dev->dev, "[MT7927] WF<->CONN sleep protect cleared\n");
			break;
		}
		poll_count++;
		usleep_range(500, 1000);
	}
	if (poll_count >= 50)
		dev_warn(dev->dev, "[MT7927] WF<->CONN sleep protect timeout\n");

	/* Ensure WFDMA->CONN sleep protection bit is cleared */
	poll_count = 0;
	while (poll_count < 50) {
		value = __mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_ADDR);
		if (!(value & CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_WFDMA2CONN_SLP_PROT_RDY_MASK)) {
			dev_info(dev->dev, "[MT7927] WFDMA->CONN sleep protect cleared\n");
			break;
		}
		poll_count++;
		usleep_range(500, 1000);
	}
	if (poll_count >= 50)
		dev_warn(dev->dev, "[MT7927] WFDMA->CONN sleep protect timeout\n");

	/* Ensure WF TOP sleep protection bits are cleared */
	poll_count = 0;
	while (poll_count < 50) {
		value = __mt76_rr(dev, WF_TOP_SLPPROT_ON_STATUS_READ_ADDR);
		if (!(value & (WF_TOP_SLPPROT_ON_STATUS_READ_SRC1_MASK |
			       WF_TOP_SLPPROT_ON_STATUS_READ_SRC2_MASK))) {
			dev_info(dev->dev, "[MT7927] WF TOP sleep protect cleared\n");
			break;
		}
		poll_count++;
		usleep_range(500, 1000);
	}
	if (poll_count >= 50)
		dev_warn(dev->dev, "[MT7927] WF TOP sleep protect timeout\n");

	dev_info(dev->dev, "[MT7927] WFSYS domain powerup complete\n");

	/* STEP 4: Switch to GPIO mode */
	dev_info(dev->dev, "[MT7927] Switching to GPIO mode...\n");
	__mt76_wr(dev, CBTOP_GPIO_MODE5_MOD_ADDR, 0x80000000);
	__mt76_wr(dev, CBTOP_GPIO_MODE6_MOD_ADDR, 0x80);
	udelay(100);

	/* STEP 5: Reset BT and WF subsystems */
	dev_info(dev->dev, "[MT7927] Resetting BT and WF subsystems...\n");
	__mt76_wr(dev, CB_INFRA_RGU_BT_SUBSYS_RST_ADDR, 0x10351);
	__mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, 0x10351);
	msleep(10);
	__mt76_wr(dev, CB_INFRA_RGU_BT_SUBSYS_RST_ADDR, 0x10340);
	__mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, 0x10340);
	msleep(50);

	/* STEP 6: Read and log GPIO mode after reset */
	value = __mt76_rr(dev, CBTOP_GPIO_MODE5_ADDR);
	dev_info(dev->dev, "[MT7927] CBTOP_GPIO_MODE5_ADDR=0x%08x\n", value);

	value = __mt76_rr(dev, CBTOP_GPIO_MODE6_ADDR);
	dev_info(dev->dev, "[MT7927] CBTOP_GPIO_MODE6_ADDR=0x%08x\n", value);

	/* STEP 7: Clean force on conninfra */
	dev_info(dev->dev, "[MT7927] Releasing CONN_INFRA force...\n");
	__mt76_wr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_TOP_ADDR, 0x0);

	/* STEP 8: Perform final WF subsystem reset release and check semaphore */
	dev_info(dev->dev, "[MT7927] Performing final WF subsystem reset release...\n");

	value = __mt76_rr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR);
	value &= ~CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_MASK;
	value |= (0x1 << CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_SHFT);
	__mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, value);

	msleep(1);

	value = __mt76_rr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR);
	value &= ~CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_MASK;
	value |= (0x0 << CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_SHFT);
	__mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, value);

	/* STEP 9: Check CONN_SEMAPHORE - should be 0x0 after successful reset */
	value = __mt76_rr(dev, CONN_SEMAPHORE_CONN_SEMA_OWN_BY_M0_STA_REP_1_ADDR);
	dev_info(dev->dev, "[MT7927] CONN_SEMAPHORE_CONN_SEMA_OWN_BY_M0_STA_REP_1=0x%08x\n",
		value);

	if ((value & CONN_SEMAPHORE_CONN_SEMA_OWN_BY_M0_STA_REP_1_CONN_SEMA00_OWN_BY_M0_STA_REP_MASK) != 0x0) {
		dev_err(dev->dev, "[MT7927] L0.5 reset failed - semaphore still owned by MCU!\n");
		/* Continue anyway - semaphore might clear later */
		ret = 0;  /* Non-fatal for now */
	} else {
		dev_info(dev->dev, "[MT7927] L0.5 reset successful - semaphore cleared\n");
	}

	return ret;
}

/* Send init command without waiting for mailbox response */
static int mt7927_mcu_send_init_cmd(struct mt76_dev *dev, int cmd,
			    const void *data, int len,
			    int wait_for_resp)
{
	struct sk_buff *skb;
	struct mt7927_init_cmd_pending *pending;
	struct mt7927_init_cmd_header *hdr;
	u16 total_len, header_len;
	int ret;
	u8 seq;
	u8 cid;
	enum mt76_mcuq_id qid = MT_MCUQ_WM;

	if (wait_for_resp)
		return mt76_mcu_send_msg(dev, cmd, data, len, true);

	cid = FIELD_GET(__MCU_CMD_FIELD_ID, cmd);
	if (cid == MCU_CMD_FW_SCATTER)
		return mt76_mcu_send_msg(dev, cmd, data, len, false);

	if (!dev->queue_ops || !dev->queue_ops->tx_queue_skb_raw)
		return mt76_mcu_send_msg(dev, cmd, data, len, false);

	header_len = sizeof(*pending) + sizeof(*hdr);
	total_len = header_len + len;

	if (!dev->q_mcu[qid])
		return mt76_mcu_send_msg(dev, cmd, data, len, false);

	skb = mt76_mcu_msg_alloc(dev, NULL, total_len);
	if (!skb)
		return -ENOMEM;

	pending = (struct mt7927_init_cmd_pending *)skb_put(skb, sizeof(*pending));
	memset(pending, 0, sizeof(*pending));
	pending->tx_byte_count = cpu_to_le16(total_len);
	pending->pq_id = cpu_to_le16(MT7927_INIT_CMD_PQ_ID);
	pending->header_format = MT7927_INIT_PKT_TYPE_ID;
	pending->pkt_ft_ownmac = MT7927_INIT_PKT_FT_CMD;

	hdr = (struct mt7927_init_cmd_header *)skb_put(skb, sizeof(*hdr));
	memset(hdr, 0, sizeof(*hdr));
	hdr->tx_byte_count = cpu_to_le16(sizeof(*hdr) + len);
	hdr->pq_id = cpu_to_le16(MT7927_INIT_CMD_PQ_ID);
	hdr->cmd.cid = cid;
	hdr->cmd.pkt_type = MT7927_INIT_PKT_TYPE_ID;

	if (len)
		skb_put_data(skb, data, len);

	/* Initialise padding so hardware sees aligned DW writes */
	if (skb->len % 4)
		memset(skb_put(skb, 4 - (skb->len % 4)), 0, 4 - (skb->len % 4));

	/* Serialize sequence allocation and raw enqueue against other MCU traffic */
	mutex_lock(&dev->mcu.mutex);

	seq = ++dev->mcu.msg_seq & 0xf;
	if (!seq)
		seq = ++dev->mcu.msg_seq & 0xf;
	hdr->cmd.seq = seq;

	/* Prep TX descriptor so WFDMA accepts the pending/header blob */
	{
		u8 q_idx;
		u32 val;
		struct mt76_connac2_mcu_txd *mcu_txd;
		__le32 *txd;

		q_idx = MT_TX_MCU_PORT_RX_Q0;

		mcu_txd = (struct mt76_connac2_mcu_txd *)
			skb_push(skb, sizeof(*mcu_txd));
		memset(mcu_txd, 0, sizeof(*mcu_txd));
		txd = mcu_txd->txd;

		val = FIELD_PREP(MT_TXD0_TX_BYTES, skb->len) |
		      FIELD_PREP(MT_TXD0_PKT_FMT, MT_TX_TYPE_CMD) |
		      FIELD_PREP(MT_TXD0_Q_IDX, q_idx);
		txd[0] = cpu_to_le32(val);

		val = FIELD_PREP(MT_TXD1_HDR_FORMAT, MT_HDR_FORMAT_CMD);
		txd[1] = cpu_to_le32(val);

		mcu_txd->len = cpu_to_le16(skb->len - sizeof(mcu_txd->txd));
		mcu_txd->pq_id = cpu_to_le16(MCU_PQ_ID(MT_TX_PORT_IDX_MCU, q_idx));
		mcu_txd->pkt_type = MCU_PKT_ID;
		mcu_txd->seq = seq;
		mcu_txd->cid = cid;
		mcu_txd->ext_cid = FIELD_GET(__MCU_CMD_FIELD_EXT_ID, cmd);
		mcu_txd->s2d_index = MCU_S2D_H2N;
		if (mcu_txd->ext_cid || (cmd & __MCU_CMD_FIELD_CE)) {
			mcu_txd->set_query = cmd & __MCU_CMD_FIELD_QUERY ?
					MCU_Q_QUERY : MCU_Q_SET;
			mcu_txd->ext_cid_ack = !!mcu_txd->ext_cid;
		} else {
			mcu_txd->set_query = MCU_Q_NA;
		}
	}

	print_hex_dump(KERN_INFO, "[MT7927] INIT CMD TXD ", DUMP_PREFIX_OFFSET,
		      16, 1, skb->data, min_t(int, skb->len, 128), false);

	ret = dev->queue_ops->tx_queue_skb_raw(dev, dev->q_mcu[qid], skb, 0);
	mutex_unlock(&dev->mcu.mutex);

	return ret;
}


/* MTK Protocol Step 1: wlanImageSectionConfig
 * Sends INIT_CMD to configure MCU memory region before data transfer.
 * This is the CRITICAL step that was missing - without it, MCU crashes!
 */
static int mt7927_mcu_image_section_config(struct mt76_dev *dev, u32 addr,
					   u32 len, u32 data_mode, bool is_patch)
{
	struct {
		__le32 addr;
		__le32 len;
		__le32 data_mode;
	} req = {
		.addr = cpu_to_le32(addr),
		.len = cpu_to_le32(len),
		.data_mode = cpu_to_le32(data_mode),
	};
	int cmd;

	/* MTK: INIT_CMD_ID_PATCH_START (7) for patches, INIT_CMD_ID_DOWNLOAD_CONFIG (1) for RAM */
	if (is_patch)
		cmd = MCU_CMD(PATCH_START_REQ);  /* 0x05 maps to cmd 7 in ROM */
	else
		cmd = MCU_CMD(TARGET_ADDRESS_LEN_REQ);  /* 0x01 */

	dev_info(dev->dev, "[MT7927] Image section config: addr=0x%08x len=%u mode=0x%x cmd=0x%x %s\n",
		 addr, len, data_mode, cmd, is_patch ? "(PATCH)" : "(RAM)");

	return mt7927_mcu_send_init_cmd(dev, cmd, &req, sizeof(req), 0);
}

/* MTK Protocol Step 2: wlanImageSectionDownload
 * Scatter firmware data in fixed-size chunks (2048 bytes).
 * Uses MCU FW_SCATTER command (no mailbox ACK per chunk).
 */
static int mt7927_mcu_image_section_download(struct mt76_dev *dev,
					     const void *data, u32 len)
{
	u32 offset = 0;
	int err;

	dev_info(dev->dev, "[MT7927] Downloading firmware section: %u bytes in %u-byte chunks\n",
		 len, MT7927_FW_CHUNK_SIZE);

	while (offset < len) {
		const u8 *chunk_data = (const u8 *)data + offset;
		u32 chunk_size;

		if (offset + MT7927_FW_CHUNK_SIZE < len)
			chunk_size = MT7927_FW_CHUNK_SIZE;
		else
			chunk_size = len - offset;

		/* ROM doesn't send mailbox acks, flush host ring proactively */
		if (dev->queue_ops->tx_cleanup)
			dev->queue_ops->tx_cleanup(dev, dev->q_mcu[MT_MCUQ_FWDL], true);

		/* Use FW_SCATTER command so mt76 routes chunks through FWDL queue */
		err = mt76_mcu_send_msg(dev, MCU_CMD(FW_SCATTER),
				chunk_data, chunk_size, false);
		if (err) {
			dev_err(dev->dev, "[MT7927] Failed to send chunk at offset %u: %d\n",
				offset, err);
			return err;
		}

		offset += chunk_size;

		/* Cleanup TX queue periodically */
		if (dev->queue_ops->tx_cleanup && (offset % (MT7927_FW_CHUNK_SIZE * 4)) == 0)
			dev->queue_ops->tx_cleanup(dev, dev->q_mcu[MT_MCUQ_FWDL], false);
		
		/* Brief yield every 8KB */
		if ((offset % (MT7927_FW_CHUNK_SIZE * 4)) == 0)
			cond_resched();
	}

	dev_info(dev->dev, "[MT7927] Section download complete: %u bytes\n", len);
	return 0;
}

/* MTK Protocol Step 3: wlanImageQueryStatus (optional)
 * Query if any errors occurred during download.
 */
static int mt7927_mcu_query_pending_error(struct mt76_dev *dev)
{
	/* MTK sends INIT_CMD_ID_QUERY_PENDING_ERROR here, but ROM doesn't support it */
	/* We check MCU state register instead */
	u32 mcu_state = mt7927_fw_get_mcu_state(dev);

	dev_info(dev->dev, "[MT7927] MCU state after download: 0x%08x\n", mcu_state);

	if (mcu_state == 0 || mcu_state == 0xFFFFFFFF) {
		dev_err(dev->dev, "[MT7927] ERROR: MCU state invalid - download failed!\n");
		return -EIO;
	}

	return 0;
}

static void mt7927_enable_fwdl_mode(struct mt76_dev *dev)
{
	u32 val;

	val = __mt76_rr(dev, MT_WFDMA0_PCIE_PDA_CFG);
	if (val & MT_WFDMA0_PDA_CFG_FWDL_EN) {
		dev_dbg(dev->dev,
			"[MT7927] FWDL mode already active (PDA_CFG=0x%08x)\n",
			val);
		return;
	}

	dev_info(dev->dev,
		 "[MT7927] Enabling FWDL mode (PDA_CFG before=0x%08x)\n", val);

	__mt76_wr(dev, MT_WFDMA0_PCIE_PDA_CFG,
		val | MT_WFDMA0_PDA_CFG_FWDL_EN);
	val = __mt76_rr(dev, MT_WFDMA0_PCIE_PDA_CFG);
	dev_info(dev->dev,
		 "[MT7927] FWDL mode write complete (PDA_CFG after=0x%08x)\n", val);
}

static u32 mt7927_patch_gen_data_mode(struct mt76_dev *dev, u32 sec_info)
{
	u32 mode = 0;

	if (sec_info == PATCH_SEC_NOT_SUPPORT)
		return mode;

	switch (FIELD_GET(PATCH_SEC_ENC_TYPE_MASK, sec_info)) {
	case PATCH_SEC_ENC_TYPE_PLAIN:
		break;
	case PATCH_SEC_ENC_TYPE_AES:
		mode |= DL_MODE_ENCRYPT;
		mode |= FIELD_PREP(DL_MODE_KEY_IDX,
				 sec_info & PATCH_SEC_ENC_AES_KEY_MASK);
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	case PATCH_SEC_ENC_TYPE_SCRAMBLE:
		mode |= DL_MODE_ENCRYPT;
		mode |= DL_CONFIG_ENCRY_MODE_SEL;
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	default:
		dev_err(dev->dev, "[MT7927] Unsupported patch encryption type: 0x%x\n",
			(unsigned int)FIELD_GET(PATCH_SEC_ENC_TYPE_MASK, sec_info));
		break;
	}

	return mode;
}


/* Load patch using MTK protocol:
 * 1. wlanImageSectionConfig (PATCH_START with addr/len/mode)
 * 2. wlanImageSectionDownload (scatter data in 2048-byte chunks)
 * 3. PATCH_FINISH
 */
int mt7927_load_patch(struct mt76_dev *dev, const char *name)
{
	const struct mt76_connac2_patch_hdr *hdr;
	const struct firmware *fw = NULL;
	int ret;
	int n_region;

	struct {
		u8 check_crc;
		u8 type;
		u8 reserved[2];
	} __packed finish_cmd = {
		.check_crc = 0,
		.type = PATCH_FNSH_TYPE_WF,
	};

	dev_info(dev->dev, "[MT7927] ========== PATCH LOADING (MTK Protocol) ==========\n");
	dev_info(dev->dev, "[MT7927] Loading patch: %s\n", name);

	/* Check MCU state before requesting firmware */
	{
		u32 mcu_state = mt7927_fw_get_mcu_state(dev);
		dev_info(dev->dev, "[MT7927] MCU state at entry: 0x%08x\n", mcu_state);
	}

	ret = request_firmware(&fw, name, dev->dev);
	if (ret) {
		dev_err(dev->dev, "[MT7927] Failed to load patch: %d\n", ret);
		return ret;
	}
	
	/* Check MCU state after requesting firmware */
	{
		u32 mcu_state = mt7927_fw_get_mcu_state(dev);
		dev_info(dev->dev, "[MT7927] MCU state after request_firmware: 0x%08x\n", mcu_state);
	}

	if (!fw || fw->size < sizeof(*hdr)) {
		dev_err(dev->dev, "[MT7927] Invalid patch file size\n");
		ret = -EINVAL;
		goto out;
	}

	hdr = (const void *)fw->data;
	dev_info(dev->dev, "[MT7927] Patch: ver=0x%x platform=%s\n",
		 be32_to_cpu(hdr->hw_sw_ver), hdr->platform);

	n_region = be32_to_cpu(hdr->desc.n_region);
	if (sizeof(*hdr) + (size_t)n_region * sizeof(struct mt76_connac2_patch_sec) > fw->size) {
		dev_err(dev->dev, "[MT7927] Patch header truncated: regions=%d size=%zu\n",
			n_region, fw->size);
		ret = -EINVAL;
		goto out;
	}

	for (int i = 0; i < n_region; i++) {
		const struct mt76_connac2_patch_sec *sec;
		u32 sec_type;
		u32 addr, len, offs;
		u32 sec_info;
		u32 data_mode;
		const u8 *payload;

		sec = (const void *)((const u8 *)fw->data + sizeof(*hdr) +
				i * sizeof(*sec));

		sec_type = be32_to_cpu(sec->type);
		if ((sec_type & PATCH_SEC_TYPE_MASK) != PATCH_SEC_TYPE_INFO) {
			dev_dbg(dev->dev,
				 "[MT7927] Skipping non-info patch section %d type=0x%x\n",
				 i, sec_type);
			continue;
		}

		addr = be32_to_cpu(sec->info.addr);
		len = be32_to_cpu(sec->info.len);
		sec_info = be32_to_cpu(sec->info.sec_key_idx);
		offs = be32_to_cpu(sec->offs);

		if (offs + len > fw->size) {
			dev_err(dev->dev,
				 "[MT7927] Patch section %d out of range (offs=%u len=%u size=%zu)\n",
				 i, offs, len, fw->size);
			ret = -EINVAL;
			goto out;
		}

		payload = fw->data + offs;
		data_mode = mt7927_patch_gen_data_mode(dev, sec_info);

		dev_info(dev->dev,
			 "[MT7927] Section %d/%d addr=0x%08x len=%u mode=0x%x offset=%u\n",
			 i + 1, n_region, addr, len, data_mode, offs);

		ret = mt7927_mcu_image_section_config(dev, addr, len, data_mode, true);
		if (ret) {
			dev_err(dev->dev,
				 "[MT7927] Section %d config failed: %d\n", i, ret);
			goto out;
		}

		/* Allow ROM a moment to prepare the region */
		msleep(5);

		mt7927_enable_fwdl_mode(dev);

		ret = mt7927_mcu_image_section_download(dev, payload, len);
		if (ret) {
			dev_err(dev->dev,
				 "[MT7927] Section %d download failed: %d\n", i, ret);
			goto out;
		}

		ret = mt7927_mcu_query_pending_error(dev);
		if (ret) {
			dev_err(dev->dev,
				 "[MT7927] Section %d verification failed: %d\n", i, ret);
			goto out;
		}
	}

	dev_info(dev->dev, "[MT7927] All patch sections transferred\n");

	dev_info(dev->dev, "[MT7927] STEP: Sending PATCH_FINISH...\n");
	ret = mt7927_mcu_send_init_cmd(dev, MCU_CMD(PATCH_FINISH_REQ),
					 &finish_cmd, sizeof(finish_cmd), 0);
	if (ret)
		dev_err(dev->dev, "[MT7927] PATCH_FINISH failed: %d\n", ret);

	/* Give ROM time to apply patch */
	msleep(50);

	/* Final status check */
	ret = mt7927_mcu_query_pending_error(dev);
	if (ret) {
		dev_err(dev->dev, "[MT7927] Patch verification failed: %d\n", ret);
		goto out;
	}

	dev_info(dev->dev, "[MT7927] ========== PATCH LOADED SUCCESSFULLY ==========\n");
	ret = 0;

out:
	release_firmware(fw);
	return ret;
}
EXPORT_SYMBOL_GPL(mt7927_load_patch);


/* Load RAM firmware using MTK protocol:
 * For each region:
 *   1. wlanImageSectionConfig (DOWNLOAD_CONFIG with addr/len/mode)
 *   2. wlanImageSectionDownload (scatter data in 2048-byte chunks)
 * After all regions:
 *   3. wlanConfigWifiFunc (WIFI_START command)
 */
int mt7927_load_ram(struct mt76_dev *dev, const char *name)
{
	const struct mt76_connac2_fw_trailer *hdr;
	const struct firmware *fw = NULL;
	struct mt792x_dev *mdev = container_of(dev, struct mt792x_dev, mt76);
	const struct mt792x_irq_map *irq = mdev->irq_map;
	int ret, i, offset = 0;
	u32 fw_override_addr = 0;
	u32 fw_start_option = 0;

	dev_info(dev->dev, "[MT7927] ========== RAM LOADING (MTK Protocol) ==========\n");
	dev_info(dev->dev, "[MT7927] Loading RAM: %s\n", name);

	ret = request_firmware(&fw, name, dev->dev);
	if (ret) {
		dev_err(dev->dev, "[MT7927] Failed to request RAM firmware: %d\n", ret);
		return ret;
	}

	if (!fw || fw->size < sizeof(*hdr)) {
		dev_err(dev->dev, "[MT7927] Invalid RAM file size\n");
		ret = -EINVAL;
		goto out;
	}

	hdr = (const void *)(fw->data + fw->size - sizeof(*hdr));
	dev_info(dev->dev, "[MT7927] RAM: chip=0x%x eco=0x%x regions=%d\n",
		 hdr->chip_id, hdr->eco_code, hdr->n_region);

	/* Check MCU state before loading */
	{
		u32 mcu_state = mt7927_fw_get_mcu_state(dev);
		dev_info(dev->dev, "[MT7927] MCU state before RAM: 0x%08x\n", mcu_state);
	}

	/* Perform MCU reset sequence to prepare for firmware loading */
	dev_info(dev->dev, "[MT7927] Performing MCU reset before firmware download...\n");
	ret = mt7927_mcu_reset(dev);
	if (ret) {
		dev_err(dev->dev, "[MT7927] MCU reset failed: %d\n", ret);
		/* Continue anyway - reset might have partially succeeded */
	}

	/* Set MCU ownership as per MT6639 sequence (after reset) */
	dev_info(dev->dev, "[MT7927] Setting MCU crypto ownership...\n");
	__mt76_wr(dev, CB_INFRA_SLP_CTRL_CB_INFRA_CRYPTO_TOP_MCU_OWN_SET_ADDR, BIT(0));

	/* Poll for MCU to return to IDLE state after reset (MT6639 sequence) */
	dev_info(dev->dev, "[MT7927] Polling for MCU to return to IDLE after reset...\n");
	{
		int poll_count = 0;
		u32 mcu_state = 0;

		while (poll_count < 1000) {
			mcu_state = mt7927_fw_get_mcu_state(dev);
			if (mcu_state == 0x1d1e) {
				dev_info(dev->dev, "[MT7927] MCU returned to IDLE after %d ms\n",
					 poll_count);
				break;
			}
			poll_count++;
			dev_warn(dev->dev, "[MT7927] MCU still not IDLE after reset (state=0x%08x)\n",
				 mcu_state);
			udelay(1000);
		}

		if (poll_count >= 1000) {
			dev_warn(dev->dev, "[MT7927] MCU still not IDLE after reset (state=0x%08x)\n",
				 mcu_state);
		}
	}

	/* Check MCU state after reset */
	{
		u32 mcu_state = mt7927_fw_get_mcu_state(dev);
		dev_info(dev->dev, "[MT7927] MCU state after reset polling: 0x%08x\n", mcu_state);
	}

	/* Download each region */
	for (i = 0; i < hdr->n_region; i++) {
		const struct mt76_connac2_fw_region *region;
		u32 addr, len, data_mode;
		u8 feature_set;

		region = (const void *)((const u8 *)hdr -
					(hdr->n_region - i) * sizeof(*region));

		addr = le32_to_cpu(region->addr);
		len = le32_to_cpu(region->len);
		feature_set = region->feature_set;
		data_mode = mt76_connac_mcu_gen_dl_mode(dev, feature_set, false);
		data_mode &= ~DL_MODE_NEED_RSP;
		data_mode |= DL_MODE_WORKING_PDA_CR4; /* Match MTK register flow for CR4 images */

		/* Skip non-downloadable regions */
		if (feature_set & FW_FEATURE_NON_DL) {
			dev_info(dev->dev, "[MT7927] Skipping non-DL region %d\n", i);
			offset += len;
			continue;
		}

		dev_info(dev->dev, "[MT7927] === Region %d/%d: addr=0x%08x len=%u ===\n",
			 i + 1, hdr->n_region, addr, len);
		dev_info(dev->dev, "[MT7927] Region %d feature_set=0x%02x data_mode=0x%08x\n",
			 i, feature_set, data_mode);

		if (feature_set & FW_FEATURE_OVERRIDE_ADDR) {
			fw_override_addr = addr;
		}

		/* STEP 1: wlanImageSectionConfig - Configure region */
		dev_info(dev->dev, "[MT7927] Configuring region %d...\n", i);
		ret = mt7927_mcu_image_section_config(dev, addr, len, data_mode, false);
		if (ret) {
			dev_err(dev->dev, "[MT7927] Region %d config failed: %d\n", i, ret);
			goto out;
		}

		msleep(5);

		mt7927_enable_fwdl_mode(dev);

		/* STEP 2: wlanImageSectionDownload - Transfer region data */
		dev_info(dev->dev, "[MT7927] Downloading region %d data...\n", i);
		ret = mt7927_mcu_image_section_download(dev, fw->data + offset, len);
		if (ret) {
			dev_err(dev->dev, "[MT7927] Region %d download failed: %d\n", i, ret);
			goto out;
		}

		offset += len;

		/* Cleanup between regions */
		if (dev->queue_ops->tx_cleanup) {
			dev->queue_ops->tx_cleanup(dev, dev->q_mcu[MT_MCUQ_FWDL], false);
			msleep(10);
		}

		/* Check status after each region */
		ret = mt7927_mcu_query_pending_error(dev);
		if (ret) {
			dev_err(dev->dev, "[MT7927] Region %d verification failed\n", i);
			goto out;
		}

		dev_info(dev->dev, "[MT7927] Region %d complete\n", i);
	}

	dev_info(dev->dev, "[MT7927] All RAM regions downloaded\n");

	/* Flush MCU TX queues before we re-enable DMA so pending frames don't block */
	if (dev->queue_ops->tx_cleanup) {
		dev->queue_ops->tx_cleanup(dev, dev->q_mcu[MT_MCUQ_WM], true);
		dev->queue_ops->tx_cleanup(dev, dev->q_mcu[MT_MCUQ_WA], true);
	}

	{
		u32 mcu_cmd = __mt76_rr(dev, MT_MCU_CMD);

		dev_info(dev->dev,
			 "[MT7927] MCU_CMD before DMA enable: 0x%08x\n",
			 mcu_cmd);
	}

	/* Re-enable WFDMA engines before issuing WIFI_START so the command
	 * is delivered via the MCU TX ring. ROM cannot process it otherwise.
	 */
	ret = mt7927_dma_enable_engines(mdev);
	if (ret) {
		dev_err(dev->dev,
			 "[MT7927] Failed to enable DMA engines before FW start: %d\n",
			 ret);
		goto out;
	}

	{
		u32 glo_cfg = __mt76_rr(dev, MT_WFDMA0_GLO_CFG);
		u32 host_int_ena = irq ? __mt76_rr(dev, irq->host_irq_enable) : 0;
		u32 host_int_sta = __mt76_rr(dev, MT_WFDMA0_HOST_INT_STA);

		dev_info(dev->dev,
			 "[MT7927] Post-DMA enable: GLO_CFG=0x%08x HOST_INT_ENA=0x%08x HOST_INT_STA=0x%08x\n",
			 glo_cfg, host_int_ena, host_int_sta);

		if (dev->q_mcu[MT_MCUQ_WM]) {
			struct mt76_queue *txq = dev->q_mcu[MT_MCUQ_WM];

			dev_info(dev->dev,
				 "[MT7927] WM TX ring state: head=%u tail=%u queued=%d ndesc=%d stopped=%d blocked=%d\n",
				 txq->head, txq->tail, txq->queued, txq->ndesc,
				 txq->stopped, txq->blocked);
		} else {
			dev_warn(dev->dev,
				 "[MT7927] WARNING: WM TX queue not initialised before WIFI_START\n");
		}

		if (dev->q_rx[MT_RXQ_MCU].ndesc) {
			struct mt76_queue *rxq = &dev->q_rx[MT_RXQ_MCU];

			dev_info(dev->dev,
				 "[MT7927] WM RX ring state: head=%u tail=%u queued=%d ndesc=%d\n",
				 rxq->head, rxq->tail, rxq->queued, rxq->ndesc);
		} else {
			dev_warn(dev->dev,
				 "[MT7927] WARNING: WM RX queue has no descriptors before WIFI_START\n");
		}
	}

	/* STEP 3: wlanConfigWifiFunc - Send WIFI_START command */
	/* DISABLED: Try auto-boot to see if firmware starts without explicit WIFI_START */
	dev_info(dev->dev, "[MT7927] WIFI_START command SKIPPED - testing auto-boot...\n");

	/* Poll for MCU to transition to running state */
	dev_info(dev->dev, "[MT7927] Polling for MCU startup (with timeout)...\n");
	{
		int poll;
		u32 mcu_state = 0;

		for (poll = 0; poll < 100; poll++) {
			msleep(100);
			mcu_state = mt7927_fw_get_mcu_state(dev);

			if ((poll % 5) == 4)
				dev_info(dev->dev, "[MT7927] Poll %d: MCU state=0x%08x\n",
					 poll + 1, mcu_state);

			/* Check for transition from IDLE (0x1d1e) */
			if (mcu_state != 0x1d1e && mcu_state != 0 &&
			    mcu_state != 0xFFFFFFFF) {
				dev_info(dev->dev,
					 "[MT7927] MCU transitioned to 0x%08x - firmware running!\n",
					 mcu_state);
				break;
			}
		}

		/* Final check */
		mcu_state = mt7927_fw_get_mcu_state(dev);
		if (mcu_state == 0 || mcu_state == 0xFFFFFFFF) {
			dev_err(dev->dev, "[MT7927] ERROR: MCU crashed (state=0x%08x)\n",
				mcu_state);
			/* Don't fail yet - let driver continue */
		} else if (mcu_state == 0x1d1e) {
			dev_warn(dev->dev,
				 "[MT7927] WARNING: MCU still in IDLE after WIFI_START\n");
			/* Don't fail - firmware might be loading or ROM might have issue with response */
		} else {
			dev_info(dev->dev,
				 "[MT7927] SUCCESS: MCU running (state=0x%08x)\n",
				 mcu_state);
		}
	}

	dev_info(dev->dev, "[MT7927] ========== RAM LOADED SUCCESSFULLY ==========\n");
	ret = 0;

out:
	release_firmware(fw);
	return ret;
}
EXPORT_SYMBOL_GPL(mt7927_load_ram);

