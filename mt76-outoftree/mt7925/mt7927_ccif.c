// SPDX-License-Identifier: ISC
/* Copyright (C) 2024 MediaTek Inc. */

#include "mt7925.h"
#include "mcu.h"
#include "mt7927_regs.h"

/* MT7927/MT6639 CCIF (Cross-Core Interface) minimal initialization
 * 
 * MT7927 is designed for mobile platforms with modem integration.
 * The MCU expects CCIF to be initialized for inter-core communication.
 * 
 * This is a minimal stub that sets up CCIF registers to allow MCU
 * communication without actual modem shared memory.
 */

#define MT7927_CCIF_BASE		0x1a0000
#define MT7927_PCIE2AP_BASE		0x1b0000

/* CCIF register offsets */
#define CCIF_RCHNUM			0x0000
#define CCIF_ACK			0x0014
#define CCIF_RCHNUM_ACK			0x001C
#define CCIF_TCHNUM			0x0100
#define CCIF_IRQ0_MASK			0x0180
#define CCIF_IRQ1_MASK			0x0184

/* Try to initialize CCIF without modem shared memory */
int mt7927_ccif_init(struct mt792x_dev *dev)
{
	struct mt76_dev *mdev = &dev->mt76;
	u32 ccif_base, pcie2ap_base;

	dev_info(mdev->dev, "MT7927: Initializing CCIF (minimal stub)\n");

	/* CCIF base addresses with bus offset */
	ccif_base = MT7927_CCIF_BASE;
	pcie2ap_base = MT7927_PCIE2AP_BASE;

	/* Note: The MTK driver uses ioremap for shared memory with modem.
	 * Since we're on a PC without modem, we'll try to just initialize
	 * the CCIF control registers to make MCU think CCIF is ready.
	 */

	/* Setup PCIE2AP remap for CCIF (already done in pci_mcu.c but ensure it's set) */
	mt76_wr(dev, 0x1b0000 + 0x5180, 0x18051803);
	dev_info(mdev->dev, "MT7927: PCIE2AP_REMAP set for CCIF\n");

	/* Try to initialize CCIF interrupt masks - allow all interrupts */
	/* These addresses need to be accessed through the proper remap window */
	
	/* Clear CCIF channel status - attempt to reset CCIF state */
	mt76_wr(dev, ccif_base + CCIF_ACK, 0xFFFFFFFF);
	mt76_wr(dev, ccif_base + CCIF_RCHNUM_ACK, 0xFFFFFFFF);
	
	/* Enable CCIF interrupts */
	mt76_wr(dev, ccif_base + CCIF_IRQ0_MASK, 0xFFFFFFFF);
	mt76_wr(dev, ccif_base + CCIF_IRQ1_MASK, 0xFFFFFFFF);
	
	dev_info(mdev->dev, "MT7927: CCIF control registers initialized\n");

	/* The MCU might still expect shared memory setup which we can't provide
	 * on a PC. This is a best-effort attempt. */
	dev_info(mdev->dev, "MT7927: CCIF initialization complete (no shared memory)\n");
	dev_info(mdev->dev, "MT7927: Note - MCU may still require full CCIF with modem\n");

	return 0;
}

/* Alternative: Try to use direct register-based MCU communication */
int mt7927_setup_direct_mcu_comm(struct mt792x_dev *dev)
{
	struct mt76_dev *mdev = &dev->mt76;
	u32 val;

	dev_info(mdev->dev, "MT7927: Checking MCU status before firmware load\n");

	/* Read MCU status to verify it's in IDLE state */
	val = mt76_rr(dev, WF_TOP_CFG_ON_ROMCODE_INDEX_ADDR);
	dev_info(mdev->dev, "MT7927: MCU ROMCODE status = 0x%08x\n", val);

	/* Don't write to any registers here - let the standard firmware
	 * loading process handle communication. The experimental writes
	 * were causing MCU crashes. */

	if (val == 0x00001d1e) {
		dev_info(mdev->dev, "MT7927: MCU in IDLE state, ready for firmware\n");
	} else {
		dev_warn(mdev->dev, "MT7927: MCU not in expected IDLE state!\n");
	}

	return 0;
}
