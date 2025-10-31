// SPDX-License-Identifier: ISC
/* Copyright (C) 2023 MediaTek Inc. */

#include "mt7925.h"
#include "mcu.h"

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

/* MT7927 requires special initialization before firmware loading */
int mt7927e_mcu_init(struct mt792x_dev *dev)
{
	static const struct mt76_mcu_ops mt7925_mcu_ops = {
		.headroom = sizeof(struct mt76_connac2_mcu_txd),
		.mcu_skb_send_msg = mt7925_mcu_send_message,
		.mcu_parse_response = mt7925_mcu_parse_response,
	};
	struct mt76_dev *mdev = &dev->mt76;
	u32 val;
	int i, err;

	dev->mt76.mcu_ops = &mt7925_mcu_ops;

	dev_info(mdev->dev, "MT7927: Starting MCU initialization sequence\n");

	/* Step 1: Force conninfra wakeup */
	mt76_wr(dev, 0x7C0601A0, 0x1);
	msleep(1);

	/* Poll for conninfra version */
	for (i = 0; i < 10; i++) {
		val = mt76_rr(dev, 0x7C011000);
		if (val == 0x03010001 || val == 0x03010002) {
			dev_info(mdev->dev, "MT7927: CONN_INFRA VERSION = 0x%08x - OK\n", val);
			break;
		}
		msleep(1);
	}

	if (i >= 10) {
		val = mt76_rr(dev, 0x7C011000);
		dev_warn(mdev->dev, "MT7927: CONN_INFRA VERSION = 0x%08x (unexpected, continuing)\n", val);
	}

	/* Step 2: WF/BT Subsystem Reset */
	dev_info(mdev->dev, "MT7927: Performing WF/BT subsystem reset\n");

	/* GPIO mode */
	mt76_wr(dev, 0x7000535c, 0x80000000);
	mt76_wr(dev, 0x7000536c, 0x80);
	usleep_range(100, 200);

	/* Reset both subsystems */
	mt76_wr(dev, 0x70028630, 0x10351);  /* BT */
	mt76_wr(dev, 0x70028600, 0x10351);  /* WF */
	msleep(10);

	mt76_wr(dev, 0x70028630, 0x10340);
	mt76_wr(dev, 0x70028600, 0x10340);
	msleep(50);

	dev_info(mdev->dev, "MT7927: WF/BT subsystem reset completed\n");

	/* After WF reset, DMA might need reinitialization */
	dev_info(mdev->dev, "MT7927: Reinitializing WPDMA after subsystem reset\n");
	err = mt792x_wpdma_reinit_cond(dev);
	if (err) {
		dev_err(mdev->dev, "MT7927: WPDMA reinit failed: %d\n", err);
		goto cleanup;
	}

	/* Step 3: Set Crypto MCU ownership */
	mt76_wr(dev, 0x70025380, 0x1);
	msleep(1);

	/* Step 4: Wait for MCU IDLE */
	dev_info(mdev->dev, "MT7927: Waiting for MCU IDLE state\n");
	for (i = 0; i < 1000; i++) {
		val = mt76_rr(dev, 0x81021604);
		if (val == 0x1D1E) {
			dev_info(mdev->dev, "MT7927: MCU IDLE (0x%08x)\n", val);
			break;
		}
		if ((i % 100) == 0 && i > 0) {
			dev_info(mdev->dev, "MT7927: MCU state = 0x%08x (waiting for 0x1D1E)\n", val);
		}
		msleep(1);
	}

	if (i >= 1000) {
		val = mt76_rr(dev, 0x81021604);
		dev_err(mdev->dev, "MT7927: MCU timeout! State=0x%08x\n", val);
		err = -ETIMEDOUT;
		goto cleanup;
	}

	/* Keep conninfra wakeup enabled for firmware loading */
	dev_info(mdev->dev, "MT7927: Hardware initialization completed\n");

	/* Power control for hardware readiness */
	err = mt792xe_mcu_fw_pmctrl(dev);
	if (err)
		goto cleanup;

	err = __mt792xe_mcu_drv_pmctrl(dev);
	if (err)
		goto cleanup;

	mt76_rmw_field(dev, MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS, 1);

	/* Now load firmware - MCU is ready */
	dev_info(mdev->dev, "MT7927: Loading firmware\n");
	err = mt7925_run_firmware(dev);
	if (err)
		goto cleanup;

	mt76_queue_tx_cleanup(dev, dev->mt76.q_mcu[MT_MCUQ_FWDL], false);

	dev_info(mdev->dev, "MT7927: Firmware loaded successfully\n");

	/* DON'T clear conninfra wakeup yet - keep it enabled for normal operation
	 * It will be cleared when entering power save mode
	 */

	return 0;

cleanup:
	mt76_wr(dev, 0x7C0601A0, 0x0);
	return err;
}
