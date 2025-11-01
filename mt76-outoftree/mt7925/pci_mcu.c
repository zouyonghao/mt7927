// SPDX-License-Identifier: ISC
/* Copyright (C) 2023 MediaTek Inc. */

#include "mt7925.h"
#include "mcu.h"
#include "mt7927_regs.h"

static int
mt7925_mcu_send_message(struct mt76_dev *mdev, struct sk_buff *skb,
			int cmd, int *seq)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	enum mt76_mcuq_id txq = MT_MCUQ_WM;
	int ret;

	ret = mt7925_mcu_fill_message(mdev, skb, cmd, seq);
	if (ret)
		return ret;

	mdev->mcu.timeout = 3 * HZ;

	if (cmd == MCU_CMD(FW_SCATTER))
		txq = MT_MCUQ_FWDL;

	dev_dbg(mdev->dev, "[MCU_TX] Sending cmd=0x%08x, seq=%d, queue=%d, skb_len=%u\n",
		cmd, seq ? *seq : -1, txq, skb->len);
	
	/* For MT7927, dump critical registers before TX (only for non-scatter commands) */
	if (is_mt7927(mdev) && cmd != MCU_CMD(FW_SCATTER)) {
		u32 int_sta = mt76_rr(dev, MT_WFDMA0_HOST_INT_STA);
		u32 int_ena = mt76_rr(dev, MT_WFDMA0_HOST_INT_ENA);
		u32 glo_cfg = mt76_rr(dev, MT_WFDMA0_GLO_CFG);
		dev_dbg(mdev->dev, "[MT7927_TX] Before TX: INT_STA=0x%08x INT_ENA=0x%08x GLO_CFG=0x%08x\n",
			 int_sta, int_ena, glo_cfg);
	}

	return mt76_tx_queue_skb_raw(dev, mdev->q_mcu[txq], skb, 0);
}

int mt7925e_mcu_init(struct mt792x_dev *dev)
{
	static const struct mt76_mcu_ops mt7925_mcu_ops = {
		.headroom = sizeof(struct mt76_connac2_mcu_txd),
		.mcu_skb_send_msg = mt7925_mcu_send_message,
		.mcu_parse_response = mt7925_mcu_parse_response,
	};
	int err;

	dev->mt76.mcu_ops = &mt7925_mcu_ops;

	err = mt792xe_mcu_fw_pmctrl(dev);
	if (err)
		return err;

	err = __mt792xe_mcu_drv_pmctrl(dev);
	if (err)
		return err;

	mt76_rmw_field(dev, MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS, 1);

	err = mt7925_run_firmware(dev);

	mt76_queue_tx_cleanup(dev, dev->mt76.q_mcu[MT_MCUQ_FWDL], false);

	return err;
}

/* MT7927 requires special pre-hardware initialization */
/* This does ONLY the MCU idle check and early setup - NOT firmware loading */
void mt7927e_mcu_pre_init(struct mt792x_dev *dev)
{
	struct mt76_dev *mdev = &dev->mt76;
	u32 val;
	int i;

	dev_info(mdev->dev, "MT7927: Starting MCU pre-initialization\n");

	/* Step 1: Force conninfra wakeup */
	mt76_wr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_CFG_PWRCTRL0_ADDR, 0x1);
	msleep(1);

	/* Poll for conninfra version */
	for (i = 0; i < 10; i++) {
		val = mt76_rr(dev, CONN_INFRA_CFG_VERSION_ADDR);
		if (val == CONN_INFRA_CFG_CONN_HW_VER || val == 0x03010001) {
			dev_info(mdev->dev, "MT7927: CONN_INFRA VERSION = 0x%08x - OK\n", val);
			break;
		}
		msleep(1);
	}

	if (i >= 10) {
		val = mt76_rr(dev, CONN_INFRA_CFG_VERSION_ADDR);
		dev_warn(mdev->dev, "MT7927: CONN_INFRA VERSION = 0x%08x (unexpected, continuing)\n", val);
	}

	/* Step 2: WiFi subsystem reset (from MTK mt6639_mcu_reset) */
	dev_info(mdev->dev, "MT7927: Performing WiFi subsystem reset\n");
	val = mt76_rr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR);
	val |= BIT(0);  /* Assert reset */
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, val);
	msleep(1);
	val &= ~BIT(0);  /* Deassert reset */
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, val);
	dev_info(mdev->dev, "MT7927: WiFi subsystem reset complete\n");

	/* Step 3: Set Crypto MCU ownership */
	mt76_wr(dev, CB_INFRA_SLP_CTRL_CB_INFRA_CRYPTO_TOP_MCU_OWN_SET_ADDR, BIT(0));
	msleep(1);

	/* Step 4: Wait for MCU IDLE */
	dev_info(mdev->dev, "MT7927: Waiting for MCU IDLE state\n");
	for (i = 0; i < 1000; i++) {
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		if (val == MCU_IDLE) {
			dev_info(mdev->dev, "MT7927: MCU IDLE (0x%08x)\n", val);
			return;
		}
		if ((i % 100) == 0 && i > 0) {
			dev_info(mdev->dev, "MT7927: MCU state = 0x%08x (waiting for 0x%04x)\n",
				 val, MCU_IDLE);
		}
		msleep(1);
	}

	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_err(mdev->dev, "MT7927: MCU timeout! State=0x%08x\n", val);
}

/* MT7927 MCU initialization - firmware loading only after DMA is ready */
int mt7927e_mcu_init(struct mt792x_dev *dev)
{
	static const struct mt76_mcu_ops mt7925_mcu_ops = {
		.headroom = sizeof(struct mt76_connac2_mcu_txd),
		.mcu_skb_send_msg = mt7925_mcu_send_message,
		.mcu_parse_response = mt7925_mcu_parse_response,
	};
	struct mt76_dev *mdev = &dev->mt76;
	u32 val, cfg_val, addr;
	int err;

	dev->mt76.mcu_ops = &mt7925_mcu_ops;

	dev_info(mdev->dev, "MT7927: MCU initialization (post-DMA)\n");

	/* CRITICAL: Set PCIE2AP remap for MCU mailbox - enables MCU to respond to PCIe commands */
	dev_info(mdev->dev, "MT7927: Setting PCIE2AP remap for MCU mailbox\n");
	mt76_wr(dev, CONN_BUS_CR_VON_CONN_INFRA_PCIE2AP_REMAP_WF_1_BA_ADDR, 0x18051803);
	val = mt76_rr(dev, CONN_BUS_CR_VON_CONN_INFRA_PCIE2AP_REMAP_WF_1_BA_ADDR);
	dev_info(mdev->dev, "MT7927: PCIE2AP_REMAP_WF_1_BA = 0x%08x\n", val);

	/* Skip CCIF and time sync for CE/Windows segment - not needed for PCIe-only mode */
	dev_info(mdev->dev, "MT7927: Skipping CCIF/time sync (CE segment mode)\n");

	/* Check MCU status - should be in IDLE state */
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU ROMCODE status = 0x%08x\n", val);

	/* MT7927-specific: Configure WFDMA MSI (Message Signaled Interrupts) */
	dev_info(mdev->dev, "MT7927: Configuring WFDMA MSI for interrupt routing\n");
	{
		u32 msi_val;

		/* Configure MSI number - use single MSI mode (value = 0) */
		msi_val = (MT7927_MSI_NUM_SINGLE <<
			   WF_WFDMA_EXT_WRAP_CSR_WFDMA_HOST_CONFIG_PCIE0_MSI_NUM_SHFT) &
			  WF_WFDMA_EXT_WRAP_CSR_WFDMA_HOST_CONFIG_PCIE0_MSI_NUM_MASK;
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_WFDMA_HOST_CONFIG_ADDR, msi_val);
		dev_info(mdev->dev, "  WFDMA_HOST_CONFIG = 0x%08x\n", msi_val);

		/* Configure MSI ring mappings - these map DMA rings to MSI vectors */
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG0_ADDR, MT7927_MSI_INT_CFG0_VALUE);
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG1_ADDR, MT7927_MSI_INT_CFG1_VALUE);
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG2_ADDR, MT7927_MSI_INT_CFG2_VALUE);
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG3_ADDR, MT7927_MSI_INT_CFG3_VALUE);
	}

	/* MT7927-specific: Configure WFDMA extension registers */
	dev_info(mdev->dev, "MT7927: Configuring WFDMA extensions\n");
	{
		/* WPDMA_GLO_CFG_EXT1: Packet-based TX flow control */
		cfg_val = MT7927_WPDMA_GLO_CFG_EXT1_VALUE | MT7927_WPDMA_GLO_CFG_EXT1_TX_FCTRL;
		mt76_wr(dev, WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_EXT1_ADDR, cfg_val);

		/* WPDMA_GLO_CFG_EXT2: Enable performance monitor */
		mt76_wr(dev, WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_EXT2_ADDR,
			MT7927_WPDMA_GLO_CFG_EXT2_VALUE);

		/* WFDMA_HIF_PERF_MAVG_DIV: Moving average divisor */
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_WFDMA_HIF_PERF_MAVG_DIV_ADDR,
			MT7927_WFDMA_HIF_PERF_MAVG_DIV_VALUE);

		/* Configure RX ring thresholds */
		for (addr = WF_WFDMA_HOST_DMA0_WPDMA_PAUSE_RX_Q_TH10_ADDR;
		     addr <= WF_WFDMA_HOST_DMA0_WPDMA_PAUSE_RX_Q_TH1110_ADDR;
		     addr += 0x4) {
			mt76_wr(dev, addr, MT7927_RX_RING_THRESHOLD_DEFAULT);
		}

		/* Enable periodic delayed interrupt */
		mt76_wr(dev, WF_WFDMA_HOST_DMA0_HOST_PER_DLY_INT_CFG_ADDR,
			MT7927_PER_DLY_INT_CFG_VALUE);

		/* Setup ring 4-7 delay interrupt configuration */
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_WFDMA_DLY_IDX_CFG_0_ADDR,
			MT7927_DLY_IDX_CFG_RING4_7_VALUE);
	}

	/* Power control for hardware readiness */
	err = mt792xe_mcu_fw_pmctrl(dev);
	if (err)
		return err;

	err = __mt792xe_mcu_drv_pmctrl(dev);
	if (err)
		return err;

	mt76_rmw_field(dev, MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS, 1);

	/* MT7927: Firmware was already loaded via custom polling loader during probe.
	 * Skip calling mt7925_run_firmware() which would try to reload via mailbox protocol.
	 * The MT7927 MCU doesn't support mailbox commands, so we can't do the usual post-init
	 * (get NIC capability, load CLC, enable logging). Just mark MCU as running. */
	
	dev_info(mdev->dev, "MT7927: Firmware already loaded via polling loader\n");
	
	/* Mark MCU as running - allows driver to proceed with hardware initialization */
	set_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);
	dev_info(mdev->dev, "MT7927: MCU marked as running (mailbox commands not supported)\n");
	
	/* Clean up firmware download queue */
	mt76_queue_tx_cleanup(dev, dev->mt76.q_mcu[MT_MCUQ_FWDL], false);
	
	dev_info(mdev->dev, "MT7927: mcu_init complete\n");

	return 0;
}
