// SPDX-License-Identifier: ISC
/* Copyright (C) 2023 MediaTek Inc. */

#include "mt7925.h"
#include "mcu.h"
#include "mt7927_regs.h"

static int mt7927_ioremap_read(struct mt792x_dev *dev, phys_addr_t addr, u32 *val)
{
	int ret;

	ret = wf_ioremap_read(addr, val);
	if (ret)
		dev_err(dev->mt76.dev,
			"mt7927: ioremap read failed addr=%pa (%d)\n",
			&addr, ret);

	return ret;
}

static int mt7927_ioremap_write(struct mt792x_dev *dev, phys_addr_t addr, u32 val)
{
	int ret;

	ret = wf_ioremap_write(addr, val);
	if (ret)
		dev_err(dev->mt76.dev,
			"mt7927: ioremap write failed addr=%pa (%d)\n",
			&addr, ret);

	return ret;
}

static int mt7927_setup_wfsys_bus_debug(struct mt792x_dev *dev)
{
	u32 val;
	int ret;

	ret = mt7927_ioremap_read(dev, WF_MCU_CONFG_LS_BUSHANGCR_ADDR, &val);
	if (ret)
		return ret;

	val &= ~WF_MCU_CONFG_LS_BUSHANGCR_BUS_HANG_TIME_LIMIT_MASK;
	val |= MT7927_BUS_HANG_TIMEOUT_VALUE;
	ret = mt7927_ioremap_write(dev, WF_MCU_CONFG_LS_BUSHANGCR_ADDR, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, WF_MCU_CONFG_LS_BUSHANGCR_ADDR, &val);
	if (ret)
		return ret;

	val |= WF_MCU_CONFG_LS_BUSHANGCR_BUS_HANG_DEBUG_EN_MASK |
	       WF_MCU_CONFG_LS_BUSHANGCR_BUS_HANG_DEBUG_CG_EN_MASK;
	ret = mt7927_ioremap_write(dev, WF_MCU_CONFG_LS_BUSHANGCR_ADDR, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_write(dev, WF_MCU_BUS_CR_AP2WF_REMAP_1_ADDR,
					WF_MCUSYS_INFRA_BUS_FULL_U_DEBUG_CTRL_AO_BASE);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, &val);
	if (ret)
		return ret;

	val |= WF_DEBUG_CTRL0_DEBUG_CKEN_MASK;
	ret = mt7927_ioremap_write(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, &val);
	if (ret)
		return ret;

	val |= WF_DEBUG_CTRL0_TIMEOUT_CLR_MASK;
	ret = mt7927_ioremap_write(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, &val);
	if (ret)
		return ret;

	val &= ~WF_DEBUG_CTRL0_TIMEOUT_CLR_MASK;
	ret = mt7927_ioremap_write(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, &val);
	if (ret)
		return ret;

	val &= ~WF_DEBUG_CTRL0_TIMEOUT_THRES_MASK;
	val |= WF_DEBUG_CTRL0_TIMEOUT_THRES_VALUE;
	ret = mt7927_ioremap_write(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL3, &val);
	if (ret)
		return ret;

	val |= WF_DEBUG_CTRL3_WFDMA_UMAC_BUSY_MASK;
	ret = mt7927_ioremap_write(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL3, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, &val);
	if (ret)
		return ret;

	val |= WF_DEBUG_CTRL0_DEBUG_EN_MASK |
	       WF_DEBUG_CTRL0_DEBUG_CKEN_MASK |
	       WF_DEBUG_CTRL0_DEBUG_TOP_EN_MASK;

	return mt7927_ioremap_write(dev, DEBUG_CTRL_AO_WFMCU_PWA_CTRL0, val);
}

static int mt7927_enable_wf_monflg(struct mt792x_dev *dev)
{
	u32 val;
	int ret;

	ret = mt7927_ioremap_read(dev, CONN_HOST_CSR_TOP_WF_ON_MONFLG_EN_FR_HIF_ADDR, &val);
	if (ret)
		return ret;

	val |= CONN_HOST_CSR_TOP_WF_ON_MONFLG_EN_FR_HIF_MASK;
	ret = mt7927_ioremap_write(dev, CONN_HOST_CSR_TOP_WF_ON_MONFLG_EN_FR_HIF_ADDR, val);
	if (ret)
		return ret;

	ret = mt7927_ioremap_read(dev, CONN_HOST_CSR_TOP_WF_ON_MONFLG_SEL_FR_HIF_ADDR, &val);
	if (ret)
		return ret;

	val &= ~CONN_HOST_CSR_TOP_WF_ON_MONFLG_SEL_FR_HIF_MASK;
	val |= CONN_HOST_CSR_TOP_WF_ON_MONFLG_MAILBOX_SEL;

	return mt7927_ioremap_write(dev, CONN_HOST_CSR_TOP_WF_ON_MONFLG_SEL_FR_HIF_ADDR, val);
}

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
/* Implements full mt6639_mcu_reinit sequence from MTK reference driver */
void mt7927e_mcu_pre_init(struct mt792x_dev *dev)
{
#define CONNINFRA_ID_MAX_POLLING_COUNT 10
#define CONNINFRA_RDY_MAX_POLLING_COUNT 10

	struct mt76_dev *mdev = &dev->mt76;
	u32 val;
	int i, err;
	bool need_recovery = false;

	dev_info(mdev->dev, "MT7927: Starting MCU pre-initialization\n");

	/* Check if recovery is needed (from mt6639_check_recovery_needed) */
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: Initial ROM CODE INDEX = 0x%08x\n", val);
	
	if ((val & 0xFFFF0000) == 0xDEAD0000) {
		dev_info(mdev->dev, "MT7927: MCU crashed (ROM CODE=0xDEADxxxx), recovery needed\n");
		need_recovery = true;
	} else if ((val & 0xFFFF) == MCU_IDLE) {
		dev_info(mdev->dev, "MT7927: MCU already in IDLE state (0x%04x), skipping recovery\n", val & 0xFFFF);
		need_recovery = false;
	} else {
		dev_info(mdev->dev, "MT7927: MCU in state 0x%08x, attempting recovery\n", val);
		need_recovery = true;
	}

	if (!need_recovery) {
		/* MCU is already in good state, just do minimal setup */
		dev_info(mdev->dev, "MT7927: MCU healthy, performing minimal init\n");
		
		/* Set CBINFRA remap (CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF) */
		mt76_wr(dev, CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF_ADDR, 0x74037001);
		
		/* Set Crypto MCU ownership */
		mt76_wr(dev, CB_INFRA_SLP_CTRL_CB_INFRA_CRYPTO_TOP_MCU_OWN_SET_ADDR, BIT(0));
		msleep(1);
		
		/* CRITICAL: Configure WFDMA MSI BEFORE any DMA operations!
		 * This must be done even when MCU is healthy, before DMA init.
		 */
		dev_info(mdev->dev, "MT7927: Configuring WFDMA MSI (pre-DMA setup)\n");
		{
			u32 msi_val;

			/* Configure MSI number - use single MSI mode (value = 0) */
			msi_val = (MT7927_MSI_NUM_SINGLE <<
				   WF_WFDMA_EXT_WRAP_CSR_WFDMA_HOST_CONFIG_PCIE0_MSI_NUM_SHFT) &
				  WF_WFDMA_EXT_WRAP_CSR_WFDMA_HOST_CONFIG_PCIE0_MSI_NUM_MASK;
			mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_WFDMA_HOST_CONFIG_ADDR, msi_val);

			/* Configure MSI ring mappings - these map DMA rings to MSI vectors */
			mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG0_ADDR, MT7927_MSI_INT_CFG0_VALUE);
			mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG1_ADDR, MT7927_MSI_INT_CFG1_VALUE);
			mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG2_ADDR, MT7927_MSI_INT_CFG2_VALUE);
			mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_MSI_INT_CFG3_ADDR, MT7927_MSI_INT_CFG3_VALUE);
			
			dev_info(mdev->dev, "MT7927: WFDMA MSI configured, now safe for DMA init\n");
		}

		err = mt7927_setup_wfsys_bus_debug(dev);
		if (err)
			dev_warn(mdev->dev,
				 "MT7927: WFSYS bus debug setup failed (%d)\n", err);

		err = mt7927_enable_wf_monflg(dev);
		if (err)
			dev_warn(mdev->dev,
				 "MT7927: WF mailbox monitor setup failed (%d)\n", err);
		
		dev_info(mdev->dev, "MT7927: Minimal init complete, MCU ready for firmware\n");
		return;
	}

	/* Full recovery sequence (mt6639_mcu_reinit) */
	dev_info(mdev->dev, "MT7927: Performing full MCU reinit/recovery\n");

	/* Step 1: Force conninfra wakeup */
	dev_info(mdev->dev, "MT7927: Force conninfra wakeup\n");
	val = mt76_rr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_WF_ADDR);
	val |= CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_WF_MASK;
	mt76_wr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_WF_ADDR, val);
	usleep_range(200, 400);
	mt76_wr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_TOP_ADDR, 0x1);
	
	/* Wait for conninfra version ID */
	for (i = 0; i < CONNINFRA_ID_MAX_POLLING_COUNT; i++) {
		val = mt76_rr(dev, CONN_CFG_IP_VERSION_ADDR);
		if (val == MT6639_CONNINFRA_VERSION_ID ||
		    val == MT6639_CONNINFRA_VERSION_ID_E2) {
			dev_info(mdev->dev, "MT7927: Conninfra woke up, version=0x%08x\n", val);
			goto conninfra_ok;
		}
		usleep_range(1000, 2000);
	}
	
	val = mt76_rr(dev, CONN_CFG_IP_VERSION_ADDR);
	dev_err(mdev->dev, "MT7927: Conninfra ID polling failed, value=0x%08x\n", val);
	return;

conninfra_ok:
	/* Ensure conninfra ready bit is set before proceeding */
	for (i = 0; i < CONNINFRA_RDY_MAX_POLLING_COUNT; i++) {
		val = mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_CFG_PWRCTRL1_ADDR);
		if (val & CONN_INFRA_CFG_ON_CONN_INFRA_CFG_PWRCTRL1_RDY_MASK) {
			dev_info(mdev->dev, "MT7927: Conninfra ready flag set (0x%08x)\n", val);
			goto conninfra_ready;
		}
		usleep_range(500, 1000);
	}

	val = mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_CFG_PWRCTRL1_ADDR);
	dev_err(mdev->dev, "MT7927: Conninfra ready polling failed, value=0x%08x\n", val);
	return;

conninfra_ready:
	/* Step 2: Bring WFSYS power domain out of sleep */
	dev_info(mdev->dev, "MT7927: Powering on WFSYS domain\n");

	/* Enable PTA-related clocks to match MTK bring-up */
	mt76_wr(dev, CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_0_SET_ADDR,
		CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_0_SET_PTA_MASK);
	mt76_wr(dev, CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_1_SET_ADDR,
		CONN_INFRA_CLKGEN_TOP_CKGEN_COEX_1_SET_PTA_MASK);

	/* Hold WFSYS CPU in reset while we power up the domain */
	val = mt76_rr(dev, CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_ADDR);
	val &= ~CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_MASK;
	mt76_wr(dev, CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_ADDR, val);

	/* Disable sleep protection between conninfra and WFSYS */
	val = mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_CTRL_ADDR);
	val &= ~CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_CTRL_CFG_CONN_WF_SLP_PROT_SW_EN_MASK;
	mt76_wr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_CTRL_ADDR, val);

	/* Power on the WFSYS TOP domain */
	val = mt76_rr(dev, CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_ADDR);
	val &= ~(CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_WRITE_KEY_MASK |
		 CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_PWR_ON_MASK);
	val |= MT7927_WFSYS_ON_TOP_WRITE_KEY;
	val |= CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_PWR_ON_MASK;
	mt76_wr(dev, CONN_INFRA_RGU_ON_WFSYS_ON_TOP_PWR_CTL_ADDR, val);

	/* Wait for WFSYS power states to indicate ON */
	for (i = 0; i < 20; i++) {
		val = mt76_rr(dev, CONN_HOST_CSR_TOP_CONNSYS_PWR_STATES_ADDR);
		if (val & BIT(30))
			break;
		usleep_range(500, 1000);
	}
	if (i == 20)
		dev_err(mdev->dev, "MT7927: WFSYS power-on timeout (CONNSYS_PWR_STATES=0x%08x)\n",
			val);

	/* Ensure sleep protection bits are cleared */
	for (i = 0; i < 120; i++) {
		val = mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_ADDR);
		if (!(val & (CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_WF2CONN_SLP_PROT_RDY_MASK |
			      CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_CONN2WF_SLP_PROT_RDY_MASK)))
			break;
		usleep_range(500, 1000);
	}
	if (i == 120)
		dev_err(mdev->dev, "MT7927: WF<->CONN sleep protect stuck (status=0x%08x)\n", val);

	for (i = 0; i < 120; i++) {
		val = mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_ADDR);
		if (!(val & CONN_INFRA_CFG_ON_CONN_INFRA_WF_SLP_STATUS_WFDMA2CONN_SLP_PROT_RDY_MASK))
			break;
		usleep_range(500, 1000);
	}
	if (i == 120)
		dev_err(mdev->dev, "MT7927: WFDMA->CONN sleep protect stuck (status=0x%08x)\n", val);

	for (i = 0; i < 120; i++) {
		val = mt76_rr(dev, WF_TOP_SLPPROT_ON_STATUS_READ_ADDR);
		if (!(val & (WF_TOP_SLPPROT_ON_STATUS_READ_SRC1_MASK |
			      WF_TOP_SLPPROT_ON_STATUS_READ_SRC2_MASK)))
			break;
		usleep_range(500, 1000);
	}
	if (i == 120)
		dev_err(mdev->dev, "MT7927: WF TOP sleep protect stuck (0x%08x)\n", val);

	val = mt76_rr(dev, WF_TOP_CFG_IP_VERSION_ADDR);
	dev_info(mdev->dev, "MT7927: WFSYS IP version 0x%08x\n", val);

	err = mt7927_setup_wfsys_bus_debug(dev);
	if (err)
		dev_warn(mdev->dev,
			 "MT7927: WFSYS bus debug setup failed (%d)\n", err);

	err = mt7927_enable_wf_monflg(dev);
	if (err)
		dev_warn(mdev->dev,
			 "MT7927: WF mailbox monitor setup failed (%d)\n", err);

	/* Release WFSYS CPU reset so ROM can run */
	val = mt76_rr(dev, CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_ADDR);
	val |= CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_MASK;
	mt76_wr(dev, CONN_INFRA_RGU_ON_WFSYS_CPU_SW_RST_B_ADDR, val);

	/* Step 3: Switch to GPIO mode (critical for MT7927!) */
	dev_info(mdev->dev, "MT7927: Switching GPIO modes\n");
	mt76_wr(dev, CBTOP_GPIO_MODE5_MOD_ADDR, 0x80000000);
	mt76_wr(dev, CBTOP_GPIO_MODE6_MOD_ADDR, 0x80);
	usleep_range(100, 200);

	/* Step 4: Reset BT and WF subsystems (proper sequence from MTK) */
	dev_info(mdev->dev, "MT7927: Resetting BT and WF subsystems\n");
	mt76_wr(dev, CB_INFRA_RGU_BT_SUBSYS_RST_ADDR, 0x10351);
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, 0x10351);
	msleep(10);
	mt76_wr(dev, CB_INFRA_RGU_BT_SUBSYS_RST_ADDR, 0x10340);
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, 0x10340);
	msleep(50);

	/* Verify GPIO modes were set */
	val = mt76_rr(dev, CBTOP_GPIO_MODE5_ADDR);
	dev_info(mdev->dev, "MT7927: GPIO_MODE5=0x%08x\n", val);
	val = mt76_rr(dev, CBTOP_GPIO_MODE6_ADDR);
	dev_info(mdev->dev, "MT7927: GPIO_MODE6=0x%08x\n", val);

	/* Step 5: Clean up conninfra force wakeup */
	dev_info(mdev->dev, "MT7927: Clean conninfra force\n");
	val = mt76_rr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_WF_ADDR);
	val &= ~CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_WF_MASK;
	mt76_wr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_WF_ADDR, val);
	mt76_wr(dev, CONN_HOST_CSR_TOP_CONN_INFRA_WAKEPU_TOP_ADDR, 0x0);

	/* Step 6: Set CBINFRA remap (from mt6639_mcu_init) */
	dev_info(mdev->dev, "MT7927: Set CBINFRA remap\n");
	mt76_wr(dev, CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF_ADDR, 0x74037001);

	/* Step 7: Set Crypto MCU ownership */
	mt76_wr(dev, CB_INFRA_SLP_CTRL_CB_INFRA_CRYPTO_TOP_MCU_OWN_SET_ADDR, BIT(0));
	msleep(1);

	/* Step 8: Configure WFDMA MSI BEFORE any DMA operations (CRITICAL!)
	 * This must be done before DMA is enabled or MCU will crash!
	 * MTK driver calls mt6639WpdmaMsiConfig() before enabling TX/RX DMA.
	 */
	dev_info(mdev->dev, "MT7927: Configuring WFDMA MSI (pre-DMA setup)\n");
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
		
		dev_info(mdev->dev, "MT7927: WFDMA MSI configured, now safe for DMA init\n");
	}

	/* Step 9: Wait for MCU IDLE */
	dev_info(mdev->dev, "MT7927: Waiting for MCU IDLE state\n");
	for (i = 0; i < 1000; i++) {
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		if (val == MCU_IDLE) {
			dev_info(mdev->dev, "MT7927: MCU IDLE (0x%08x) - ready for firmware\n", val);
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

	/* CRITICAL: Wait for MCU IDLE before accessing WFDMA registers!
	 * MT6639 waits for MCU_IDLE in mcu_init BEFORE setting PCIE2AP remap.
	 * If we write to these registers while MCU is still initializing, it crashes!
	 */
	dev_info(mdev->dev, "MT7927: Verifying MCU is in IDLE state before configuration\n");
	{
		int retry;
		for (retry = 0; retry < 1000; retry++) {
			val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
			if ((val & 0xFFFF) == MCU_IDLE) {
				dev_info(mdev->dev, "MT7927: MCU confirmed in IDLE (0x%08x) after %d ms\n", 
					 val, retry);
				break;
			}
			if (retry > 0 && (retry % 100) == 0) {
				dev_info(mdev->dev, "MT7927: Waiting for MCU IDLE, current state=0x%08x\n", val);
			}
			msleep(1);
		}
		if (retry >= 1000) {
			dev_err(mdev->dev, "MT7927: MCU timeout! State=0x%08x (expected 0x1D1E)\n", val);
			return -ETIMEDOUT;
		}
	}

	/* Now it's safe to configure PCIE2AP remap and WFDMA registers */
	dev_info(mdev->dev, "MT7927: Setting PCIE2AP remap for MCU mailbox\n");
	mt76_wr(dev, CONN_BUS_CR_VON_CONN_INFRA_PCIE2AP_REMAP_WF_1_BA_ADDR, 0x18051803);
	val = mt76_rr(dev, CONN_BUS_CR_VON_CONN_INFRA_PCIE2AP_REMAP_WF_1_BA_ADDR);
	dev_info(mdev->dev, "MT7927: PCIE2AP_REMAP_WF_1_BA = 0x%08x\n", val);

	/* Skip CCIF and time sync for CE/Windows segment - not needed for PCIe-only mode */
	dev_info(mdev->dev, "MT7927: Skipping CCIF/time sync (CE segment mode)\n");

	/* Verify MCU still in IDLE after remap */
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU ROMCODE status = 0x%08x\n", val);

	/* MT7927-specific: Configure WFDMA extension registers
	 * MSI was already configured in pre_init, now configure other WFDMA settings */
	dev_info(mdev->dev, "MT7927: Configuring WFDMA extensions\n");
	{
		/* WPDMA_GLO_CFG_EXT1: Packet-based TX flow control */
		cfg_val = MT7927_WPDMA_GLO_CFG_EXT1_VALUE | MT7927_WPDMA_GLO_CFG_EXT1_TX_FCTRL;
		mt76_wr(dev, WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_EXT1_ADDR, cfg_val);
		
		/* Check MCU after EXT1 */
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		dev_info(mdev->dev, "MT7927: MCU after GLO_CFG_EXT1: 0x%08x\n", val);

		/* WPDMA_GLO_CFG_EXT2: Enable performance monitor */
		mt76_wr(dev, WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_EXT2_ADDR,
			MT7927_WPDMA_GLO_CFG_EXT2_VALUE);
		
		/* Check MCU after EXT2 */
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		dev_info(mdev->dev, "MT7927: MCU after GLO_CFG_EXT2: 0x%08x\n", val);

		/* WFDMA_HIF_PERF_MAVG_DIV: Moving average divisor */
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_WFDMA_HIF_PERF_MAVG_DIV_ADDR,
			MT7927_WFDMA_HIF_PERF_MAVG_DIV_VALUE);
		
		/* Check MCU after MAVG_DIV */
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		dev_info(mdev->dev, "MT7927: MCU after MAVG_DIV: 0x%08x\n", val);

		/* Configure RX ring thresholds */
		for (addr = WF_WFDMA_HOST_DMA0_WPDMA_PAUSE_RX_Q_TH10_ADDR;
		     addr <= WF_WFDMA_HOST_DMA0_WPDMA_PAUSE_RX_Q_TH1110_ADDR;
		     addr += 0x4) {
			mt76_wr(dev, addr, MT7927_RX_RING_THRESHOLD_DEFAULT);
		}
		
		/* Check MCU after RX thresholds */
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		dev_info(mdev->dev, "MT7927: MCU after RX thresholds: 0x%08x\n", val);

		/* Enable periodic delayed interrupt */
		mt76_wr(dev, WF_WFDMA_HOST_DMA0_HOST_PER_DLY_INT_CFG_ADDR,
			MT7927_PER_DLY_INT_CFG_VALUE);
		
		/* Check MCU after PER_DLY_INT */
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		dev_info(mdev->dev, "MT7927: MCU after PER_DLY_INT: 0x%08x\n", val);

		/* Setup ring 4-7 delay interrupt configuration */
		mt76_wr(dev, WF_WFDMA_EXT_WRAP_CSR_WFDMA_DLY_IDX_CFG_0_ADDR,
			MT7927_DLY_IDX_CFG_RING4_7_VALUE);
		
		/* Check MCU after DLY_IDX */
		val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
		dev_info(mdev->dev, "MT7927: MCU after DLY_IDX: 0x%08x\n", val);
	}
	
	dev_info(mdev->dev, "MT7927: WFDMA extensions configured, checking MCU state\n");
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU after WFDMA config: 0x%08x\n", val);

	/* Power control for hardware readiness */
	dev_info(mdev->dev, "MT7927: Starting power management setup\n");
	err = mt792xe_mcu_fw_pmctrl(dev);
	if (err) {
		dev_err(mdev->dev, "MT7927: fw_pmctrl failed: %d\n", err);
		return err;
	}
	
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU after fw_pmctrl: 0x%08x\n", val);

	err = __mt792xe_mcu_drv_pmctrl(dev);
	if (err) {
		dev_err(mdev->dev, "MT7927: drv_pmctrl failed: %d\n", err);
		return err;
	}
	
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU after drv_pmctrl: 0x%08x\n", val);

	mt76_rmw_field(dev, MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS, 1);
	
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU after PCIE_MAC_PM: 0x%08x\n", val);

	/* MT7927: Load firmware using polling-based loader (no mailbox protocol)
	 * Unlike MT7925 which uses mailbox commands, MT7927 requires direct DMA writes
	 * and polling for completion status. */
	
	dev_info(mdev->dev, "MT7927: Loading firmware via polling loader...\n");
	
	/* Check MCU state RIGHT before firmware loading */
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU state RIGHT before mt7927_load_patch call: 0x%08x\n", val);
	
	/* Load patch file */
	err = mt7927_load_patch(mdev, mt792x_patch_name(dev));
	if (err) {
		dev_err(mdev->dev, "MT7927: Patch load failed: %d\n", err);
		/* Continue anyway - maybe patch is already loaded */
	}
	
	/* Load RAM firmware */
	err = mt7927_load_ram(mdev, mt792x_ram_name(dev));
	if (err) {
		dev_err(mdev->dev, "MT7927: RAM load failed: %d\n", err);
		// return err;  /* RAM is critical, fail if it doesn't load */
		// ignore for now
	}
	
	dev_info(mdev->dev, "MT7927: Firmware loaded successfully\n");
	
	/* WIFI_START command is now sent inside mt7927_load_ram() */
	
	/* Mark MCU as running - allows driver to proceed with hardware initialization */
	set_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);
	dev_info(mdev->dev, "MT7927: MCU marked as running (mailbox commands not supported)\n");
	
	/* MT7927: Enable interrupts since we skip mt7925_mac_reset() path
	 * Based on MT6639 mt6639ConfigIntMask(), use SET/CLR registers instead of direct write
	 */
	dev_info(mdev->dev, "MT7927: Enabling interrupts via SET register\n");
	
	{
		u32 int_mask = dev->irq_map->tx.all_complete_mask |
			       MT_INT_RX_DONE_ALL | MT_INT_MCU_CMD;
		
		/* MT7927: Use HOST_INT_ENA_SET register (like MT6639)
		 * Write to SET register enables interrupts atomically */
		mt76_wr(dev, WF_WFDMA_HOST_DMA0_HOST_INT_ENA_SET_ADDR, int_mask);
		dev_info(mdev->dev, "MT7927: Wrote 0x%08x to HOST_INT_ENA_SET (0x%08x)\n",
			 int_mask, WF_WFDMA_HOST_DMA0_HOST_INT_ENA_SET_ADDR);
		
		/* Also enable PCIE MAC interrupts */
		mt76_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xff);
		dev_info(mdev->dev, "MT7927: PCIE_MAC_INT_ENABLE = 0xff\n");
	}
	
	/* Dump critical DMA/interrupt registers for debugging */
	{
		u32 glo_cfg = mt76_rr(dev, MT_WFDMA0_GLO_CFG);
		u32 int_sta = mt76_rr(dev, MT_WFDMA0_HOST_INT_STA);
		u32 int_ena = mt76_rr(dev, MT_WFDMA0_HOST_INT_ENA);
		dev_info(mdev->dev, "MT7927: After init: GLO_CFG=0x%08x INT_STA=0x%08x INT_ENA=0x%08x\n",
			 glo_cfg, int_sta, int_ena);
	}
	
	/* Clean up firmware download queue */
	mt76_queue_tx_cleanup(dev, dev->mt76.q_mcu[MT_MCUQ_FWDL], false);
	
	dev_info(mdev->dev, "MT7927: mcu_init complete\n");

	return 0;
}
