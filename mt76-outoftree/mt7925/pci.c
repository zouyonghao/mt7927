// SPDX-License-Identifier: ISC
/* Copyright (C) 2023 MediaTek Inc. */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "mt7925.h"
#include "mac.h"
#include "mcu.h"
#include "../dma.h"
#include "mt7927_regs.h"

static const struct pci_device_id mt7925_pci_device_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7925),
		.driver_data = (kernel_ulong_t)MT7925_FIRMWARE_WM },
	{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x0717),
		.driver_data = (kernel_ulong_t)MT7925_FIRMWARE_WM },
	{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7927),
		.driver_data = (kernel_ulong_t)MT7925_FIRMWARE_WM },
	{ },
};

static bool mt7925_disable_aspm;
module_param_named(disable_aspm, mt7925_disable_aspm, bool, 0644);
MODULE_PARM_DESC(disable_aspm, "disable PCI ASPM support");

static int mt7925e_init_reset(struct mt792x_dev *dev)
{
	return mt792x_wpdma_reset(dev, true);
}

static void mt7925e_unregister_device(struct mt792x_dev *dev)
{
	int i;
	struct mt76_connac_pm *pm = &dev->pm;

	cancel_work_sync(&dev->init_work);
	mt76_unregister_device(&dev->mt76);
	mt76_for_each_q_rx(&dev->mt76, i)
		napi_disable(&dev->mt76.napi[i]);
	cancel_delayed_work_sync(&pm->ps_work);
	cancel_work_sync(&pm->wake_work);
	cancel_work_sync(&dev->reset_work);

	mt7925_tx_token_put(dev);
	__mt792x_mcu_drv_pmctrl(dev);
	mt792x_dma_cleanup(dev);
	mt792x_wfsys_reset(dev);
	skb_queue_purge(&dev->mt76.mcu.res_q);

	tasklet_disable(&dev->mt76.irq_tasklet);
}

static void mt7925_reg_remap_restore(struct mt792x_dev *dev)
{
	/* remap to ori status */
	if (unlikely(dev->backup_l1)) {
		dev->bus_ops->wr(&dev->mt76, MT_HIF_REMAP_L1, dev->backup_l1);
		dev->backup_l1 = 0;
	}

	if (dev->backup_l2) {
		dev->bus_ops->wr(&dev->mt76, MT_HIF_REMAP_L2, dev->backup_l2);
		dev->backup_l2 = 0;
	}
}

static u32 mt7925_reg_map_l1(struct mt792x_dev *dev, u32 addr)
{
	u32 offset = FIELD_GET(MT_HIF_REMAP_L1_OFFSET, addr);
	u32 base = FIELD_GET(MT_HIF_REMAP_L1_BASE, addr);

	dev->backup_l1 = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	dev->bus_ops->rmw(&dev->mt76, MT_HIF_REMAP_L1,
			  MT_HIF_REMAP_L1_MASK,
			  FIELD_PREP(MT_HIF_REMAP_L1_MASK, base));

	/* use read to push write */
	dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	return MT_HIF_REMAP_BASE_L1 + offset;
}

static u32 mt7925_reg_map_l2(struct mt792x_dev *dev, u32 addr)
{
	u32 base = FIELD_GET(MT_HIF_REMAP_L1_BASE, MT_HIF_REMAP_BASE_L2);

	dev->backup_l2 = dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	dev->bus_ops->rmw(&dev->mt76, MT_HIF_REMAP_L1,
			  MT_HIF_REMAP_L1_MASK,
			  FIELD_PREP(MT_HIF_REMAP_L1_MASK, base));

	dev->bus_ops->wr(&dev->mt76, MT_HIF_REMAP_L2, addr);
	/* use read to push write */
	dev->bus_ops->rr(&dev->mt76, MT_HIF_REMAP_L1);

	return MT_HIF_REMAP_BASE_L1;
}

static u32 __mt7925_reg_addr(struct mt792x_dev *dev, u32 addr)
{
	static const struct mt76_connac_reg_map fixed_map[] = {
		{ 0x830c0000, 0x000000, 0x0001000 }, /* WF_MCU_BUS_CR_REMAP */
		{ 0x54000000, 0x002000, 0x0001000 }, /* WFDMA PCIE0 MCU DMA0 */
		{ 0x55000000, 0x003000, 0x0001000 }, /* WFDMA PCIE0 MCU DMA1 */
		{ 0x56000000, 0x004000, 0x0001000 }, /* WFDMA reserved */
		{ 0x57000000, 0x005000, 0x0001000 }, /* WFDMA MCU wrap CR */
		{ 0x58000000, 0x006000, 0x0001000 }, /* WFDMA PCIE1 MCU DMA0 (MEM_DMA) */
		{ 0x59000000, 0x007000, 0x0001000 }, /* WFDMA PCIE1 MCU DMA1 */
		{ 0x820c0000, 0x008000, 0x0004000 }, /* WF_UMAC_TOP (PLE) */
		{ 0x820c8000, 0x00c000, 0x0002000 }, /* WF_UMAC_TOP (PSE) */
		{ 0x820cc000, 0x00e000, 0x0002000 }, /* WF_UMAC_TOP (PP) */
		{ 0x820e0000, 0x020000, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_CFG) */
		{ 0x820e1000, 0x020400, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_TRB) */
		{ 0x820e2000, 0x020800, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_AGG) */
		{ 0x820e3000, 0x020c00, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_ARB) */
		{ 0x820e4000, 0x021000, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_TMAC) */
		{ 0x820e5000, 0x021400, 0x0000800 }, /* WF_LMAC_TOP BN0 (WF_RMAC) */
		{ 0x820ce000, 0x021c00, 0x0000200 }, /* WF_LMAC_TOP (WF_SEC) */
		{ 0x820e7000, 0x021e00, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_DMA) */
		{ 0x820cf000, 0x022000, 0x0001000 }, /* WF_LMAC_TOP (WF_PF) */
		{ 0x820e9000, 0x023400, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
		{ 0x820ea000, 0x024000, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_ETBF) */
		{ 0x820eb000, 0x024200, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_LPON) */
		{ 0x820ec000, 0x024600, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_INT) */
		{ 0x820ed000, 0x024800, 0x0000800 }, /* WF_LMAC_TOP BN0 (WF_MIB) */
		{ 0x820ca000, 0x026000, 0x0002000 }, /* WF_LMAC_TOP BN0 (WF_MUCOP) */
		{ 0x820d0000, 0x030000, 0x0010000 }, /* WF_LMAC_TOP (WF_WTBLON) */
		{ 0x40000000, 0x070000, 0x0010000 }, /* WF_UMAC_SYSRAM */
		{ 0x00400000, 0x080000, 0x0010000 }, /* WF_MCU_SYSRAM */
		{ 0x00410000, 0x090000, 0x0010000 }, /* WF_MCU_SYSRAM (configure register) */
		{ 0x820f0000, 0x0a0000, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_CFG) */
		{ 0x820f1000, 0x0a0600, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_TRB) */
		{ 0x820f2000, 0x0a0800, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_AGG) */
		{ 0x820f3000, 0x0a0c00, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_ARB) */
		{ 0x820f4000, 0x0a1000, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_TMAC) */
		{ 0x820f5000, 0x0a1400, 0x0000800 }, /* WF_LMAC_TOP BN1 (WF_RMAC) */
		{ 0x820f7000, 0x0a1e00, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_DMA) */
		{ 0x820f9000, 0x0a3400, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
		{ 0x820fa000, 0x0a4000, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_ETBF) */
		{ 0x820fb000, 0x0a4200, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_LPON) */
		{ 0x820fc000, 0x0a4600, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_INT) */
		{ 0x820fd000, 0x0a4800, 0x0000800 }, /* WF_LMAC_TOP BN1 (WF_MIB) */
		{ 0x820c4000, 0x0a8000, 0x0004000 }, /* WF_LMAC_TOP BN1 (WF_MUCOP) */
		{ 0x820b0000, 0x0ae000, 0x0001000 }, /* [APB2] WFSYS_ON */
		{ 0x80020000, 0x0b0000, 0x0010000 }, /* WF_TOP_MISC_OFF */
		{ 0x81020000, 0x0c0000, 0x0010000 }, /* WF_TOP_MISC_ON */
		{ 0x7c020000, 0x0d0000, 0x0010000 }, /* CONN_INFRA, wfdma */
		{ 0x7c060000, 0x0e0000, 0x0010000 }, /* CONN_INFRA, conn_host_csr_top */
		{ 0x7c000000, 0x0f0000, 0x0010000 }, /* CONN_INFRA */
		{ 0x7c010000, 0x100000, 0x0010000 }, /* CONN_INFRA (includes CONN_CFG at 0x7C011000) */
		{ 0x7c030000, 0x1a0000, 0x0010000 }, /* CONN_INFRA_ON_CCIF (for PCCIF/mailbox) */
		{ 0x70000000, 0x1e0000, 0x0009000 }, /* MT6639/MT7927: CBTOP low range (includes 0x70010200 chip ID) */
		{ 0x70020000, 0x1f0000, 0x0010000 }, /* Reserved for CBTOP, can't switch */
		{ 0x7c500000, 0x060000, 0x2000000 }, /* remap */
		{ 0x0, 0x0, 0x0 } /* End */
	};
	static const struct mt76_connac_reg_map fixed_map_mt7927[] = {
		{ 0x830c0000, 0x000000, 0x0001000 }, /* WF_MCU_BUS_CR_REMAP */
		{ 0x54000000, 0x002000, 0x0001000 }, /* WFDMA PCIE0 MCU DMA0 */
		{ 0x55000000, 0x003000, 0x0001000 }, /* WFDMA PCIE0 MCU DMA1 */
		{ 0x56000000, 0x004000, 0x0001000 }, /* WFDMA reserved */
		{ 0x57000000, 0x005000, 0x0001000 }, /* WFDMA MCU wrap CR */
		{ 0x58000000, 0x006000, 0x0001000 }, /* WFDMA PCIE1 MCU DMA0 (MEM_DMA) */
		{ 0x59000000, 0x007000, 0x0001000 }, /* WFDMA PCIE1 MCU DMA1 */
		{ 0x820c0000, 0x008000, 0x0004000 }, /* WF_UMAC_TOP (PLE) */
		{ 0x820c8000, 0x00c000, 0x0002000 }, /* WF_UMAC_TOP (PSE) */
		{ 0x820cc000, 0x00e000, 0x0002000 }, /* WF_UMAC_TOP (PP) */
		{ 0x820e0000, 0x020000, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_CFG) */
		{ 0x820e1000, 0x020400, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_TRB) */
		{ 0x820e2000, 0x020800, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_AGG) */
		{ 0x820e3000, 0x020c00, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_ARB) */
		{ 0x820e4000, 0x021000, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_TMAC) */
		{ 0x820e5000, 0x021400, 0x0000800 }, /* WF_LMAC_TOP BN0 (WF_RMAC) */
		{ 0x820ce000, 0x021c00, 0x0000200 }, /* WF_LMAC_TOP (WF_SEC) */
		{ 0x820e7000, 0x021e00, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_DMA) */
		{ 0x820cf000, 0x022000, 0x0001000 }, /* WF_LMAC_TOP (WF_PF) */
		{ 0x820e9000, 0x023400, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
		{ 0x820ea000, 0x024000, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_ETBF) */
		{ 0x820eb000, 0x024200, 0x0000400 }, /* WF_LMAC_TOP BN0 (WF_LPON) */
		{ 0x820ec000, 0x024600, 0x0000200 }, /* WF_LMAC_TOP BN0 (WF_INT) */
		{ 0x820ed000, 0x024800, 0x0000800 }, /* WF_LMAC_TOP BN0 (WF_MIB) */
		{ 0x820ca000, 0x026000, 0x0002000 }, /* WF_LMAC_TOP BN0 (WF_MUCOP) */
		{ 0x820d0000, 0x030000, 0x0010000 }, /* WF_LMAC_TOP (WF_WTBLON) */
		{ 0x40000000, 0x070000, 0x0010000 }, /* WF_UMAC_SYSRAM */
		{ 0x00400000, 0x080000, 0x0010000 }, /* WF_MCU_SYSRAM */
		{ 0x00410000, 0x090000, 0x0010000 }, /* WF_MCU_SYSRAM (configure register) */
		{ 0x820f0000, 0x0a0000, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_CFG) */
		{ 0x820f1000, 0x0a0600, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_TRB) */
		{ 0x820f2000, 0x0a0800, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_AGG) */
		{ 0x820f3000, 0x0a0c00, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_ARB) */
		{ 0x820f4000, 0x0a1000, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_TMAC) */
		{ 0x820f5000, 0x0a1400, 0x0000800 }, /* WF_LMAC_TOP BN1 (WF_RMAC) */
		{ 0x820f7000, 0x0a1e00, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_DMA) */
		{ 0x820f9000, 0x0a3400, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
		{ 0x820fa000, 0x0a4000, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_ETBF) */
		{ 0x820fb000, 0x0a4200, 0x0000400 }, /* WF_LMAC_TOP BN1 (WF_LPON) */
		{ 0x820fc000, 0x0a4600, 0x0000200 }, /* WF_LMAC_TOP BN1 (WF_INT) */
		{ 0x820fd000, 0x0a4800, 0x0000800 }, /* WF_LMAC_TOP BN1 (WF_MIB) */
		{ 0x820c4000, 0x0a8000, 0x0004000 }, /* WF_LMAC_TOP BN1 (WF_MUCOP) */
		{ 0x820b0000, 0x0ae000, 0x0001000 }, /* [APB2] WFSYS_ON */
		{ 0x80020000, 0x0b0000, 0x0010000 }, /* WF_TOP_MISC_OFF */
		{ 0x81020000, 0x0c0000, 0x0010000 }, /* WF_TOP_MISC_ON */
		{ 0x7c020000, 0x0d0000, 0x0010000 }, /* CONN_INFRA, wfdma */
		{ 0x7c060000, 0x0e0000, 0x0010000 }, /* CONN_INFRA, conn_host_csr_top */
		{ 0x7c000000, 0x0f0000, 0x0010000 }, /* CONN_INFRA */
		{ 0x7c010000, 0x100000, 0x0010000 }, /* CONN_INFRA (includes CONN_CFG at 0x7C011000) */
		{ 0x7c030000, 0x1a0000, 0x0010000 }, /* CONN_INFRA_ON_CCIF (for PCCIF/mailbox) */
		{ 0x74030000, 0x010000, 0x0001000 }, /* PCIe MAC (MT6639/MT7927 CE/Linux, not mobile) */
		{ 0x70000000, 0x1e0000, 0x0009000 }, /* MT6639/MT7927: CBTOP low range (includes 0x70010200 chip ID) */
		{ 0x70020000, 0x1f0000, 0x0010000 }, /* Reserved for CBTOP, can't switch */
		{ 0x7c500000, 0x060000, 0x2000000 }, /* remap */
		{0x70000000, 0x1e0000, 0x9000},
		{ 0x0, 0x0, 0x0 } /* End */
	};
	const struct mt76_connac_reg_map *map;
	int i, map_size;

	/* MT7927 needs different PCIe MAC mapping */
	if (is_mt7927(&dev->mt76)) {
		map = fixed_map_mt7927;
		map_size = ARRAY_SIZE(fixed_map_mt7927);
	} else {
		map = fixed_map;
		map_size = ARRAY_SIZE(fixed_map);
	}

	if (addr < 0x200000)
		return addr;

	mt7925_reg_remap_restore(dev);

	for (i = 0; i < map_size; i++) {
		u32 ofs;

		if (addr < map[i].phys)
			continue;

		ofs = addr - map[i].phys;
		if (ofs > map[i].size)
			continue;

		return map[i].maps + ofs;
	}

	if ((addr >= 0x18000000 && addr < 0x18c00000) ||
	    (addr >= 0x70000000 && addr < 0x78000000) ||
	    (addr >= 0x7c000000 && addr < 0x7c400000))
		return mt7925_reg_map_l1(dev, addr);

	return mt7925_reg_map_l2(dev, addr);
}

static u32 mt7925_rr(struct mt76_dev *mdev, u32 offset)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	u32 addr = __mt7925_reg_addr(dev, offset);

	return dev->bus_ops->rr(mdev, addr);
}

static void mt7925_wr(struct mt76_dev *mdev, u32 offset, u32 val)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	u32 addr = __mt7925_reg_addr(dev, offset);

	dev->bus_ops->wr(mdev, addr, val);
}

static u32 mt7925_rmw(struct mt76_dev *mdev, u32 offset, u32 mask, u32 val)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	u32 addr = __mt7925_reg_addr(dev, offset);

	return dev->bus_ops->rmw(mdev, addr, mask, val);
}

static int mt7927_wfsys_reset(struct mt792x_dev *dev)
{
	struct mt76_dev *mdev = &dev->mt76;
	u32 val;
	int i;

	dev_info(mdev->dev, "MT7927: Performing WF/BT subsystem reset (MTK sequence)\n");

	/* GPIO mode configuration */
	mt76_wr(dev, CBTOP_GPIO_MODE5_MOD_ADDR, MT7927_GPIO_MODE5_VALUE);
	mt76_wr(dev, CBTOP_GPIO_MODE6_MOD_ADDR, MT7927_GPIO_MODE6_VALUE);
	usleep_range(100, 200);

	/* BT subsystem reset */
	mt76_wr(dev, CB_INFRA_RGU_BT_SUBSYS_RST_ADDR, MT7927_BT_SUBSYS_RST_ASSERT);
	msleep(10);
	mt76_wr(dev, CB_INFRA_RGU_BT_SUBSYS_RST_ADDR, MT7927_BT_SUBSYS_RST_DEASSERT);
	msleep(10);

	/* WF subsystem reset */
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, MT7927_WF_SUBSYS_RST_ASSERT);
	msleep(10);
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, MT7927_WF_SUBSYS_RST_DEASSERT);
	msleep(50);

	/* Step 3: Second WF reset - exact MTK mt6639_mcu_reset sequence */
	/* RMW on WF_SUBSYS_RST bit only, preserve all other bits */
	dev_info(mdev->dev, "MT7927: Performing second WF reset (MTK RMW on bit 4)\n");

	/* Read current value */
	val = mt76_rr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR);
	dev_info(mdev->dev, "MT7927: WF_SUBSYS_RST read = 0x%08x\n", val);

	/* Assert reset: clear mask, then set bit */
	val &= ~CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_MASK;
	val |= (1 << CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_SHFT);
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, val);
	dev_info(mdev->dev, "MT7927: WF_SUBSYS_RST wrote 0x%08x (reset asserted)\n", val);
	msleep(1);

	/* Read again - hardware may modify register during reset */
	val = mt76_rr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR);
	dev_info(mdev->dev, "MT7927: WF_SUBSYS_RST read after 1ms = 0x%08x\n", val);

	/* De-assert reset: clear mask, then set to 0 */
	val &= ~CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_MASK;
	val |= (0 << CB_INFRA_RGU_WF_SUBSYS_RST_WF_SUBSYS_RST_SHFT);
	mt76_wr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR, val);
	dev_info(mdev->dev, "MT7927: WF_SUBSYS_RST wrote 0x%08x (reset de-asserted)\n", val);
	msleep(10);

	dev_info(mdev->dev, "MT7927: WF_SUBSYS_RST final value = 0x%08x\n",
		 mt76_rr(dev, CB_INFRA_RGU_WF_SUBSYS_RST_ADDR));

	/* Verify CONN_SEMAPHORE after reset (should be 0x0 per MTK code) */
	val = mt76_rr(dev, CONN_SEMAPHORE_CONN_SEMA_OWN_BY_M0_STA_REP_1_ADDR);
	dev_info(mdev->dev, "MT7927: CONN_SEMAPHORE = 0x%08x (should be 0x0)\n", val);
	if (val & CONN_SEMAPHORE_CONN_SEMA_OWN_BY_M0_STA_REP_1_CONN_SEMA00_OWN_BY_M0_STA_REP_MASK)
		dev_warn(mdev->dev, "MT7927: L0.5 reset incomplete - semaphore owned by MCU, will retry\n");

	/* CRITICAL: MCU state after reset
	 * After WF subsystem reset, the MCU may not automatically start the ROM bootloader.
	 * The semaphore being 0x1 (owned by MCU) is actually NORMAL - it means the hardware
	 * reset completed and MCU domain is alive. The driver_own sequence will clear it.
	 * 
	 * We DON'T need to wait for MCU IDLE here - that happens after firmware is loaded!
	 */
	dev_info(mdev->dev, "MT7927: WF subsystem reset complete, semaphore=0x%08x\n", val);

	/* Wait for WF init done */
	for (i = 0; i < 500; i++) {
		val = mt76_rr(dev, CONN_INFRA_CFG_ON_CONN_INFRA_CFG_AP2WF_BUS_ADDR);
		if (val & CONN_INFRA_CFG_ON_CONN_INFRA_CFG_AP2WF_BUS_WFSYS_SW_INIT_DONE) {
			dev_info(mdev->dev, "MT7927: WF subsystem init done (reg=0x%08x)\n", val);
			return 0;
		}
		msleep(1);
	}

	dev_err(mdev->dev, "MT7927: WF subsystem init timeout (reg=0x%08x)\n", val);
	return -ETIMEDOUT;
}

static int mt7925_dma_init(struct mt792x_dev *dev)
{
	int ret;

	mt76_dma_attach(&dev->mt76);

	ret = mt792x_dma_disable(dev, true);
	if (ret)
		return ret;

	/* init tx queue */
	ret = mt76_connac_init_tx_queues(dev->phy.mt76, MT7925_TXQ_BAND0,
					 MT7925_TX_RING_SIZE,
					 MT_TX_RING_BASE, NULL, 0);
	if (ret)
		return ret;

	mt76_wr(dev, MT_WFDMA0_TX_RING0_EXT_CTRL, 0x4);

	/* command to WM */
	ret = mt76_init_mcu_queue(&dev->mt76, MT_MCUQ_WM, MT7925_TXQ_MCU_WM,
				  MT7925_TX_MCU_RING_SIZE, MT_TX_RING_BASE);
	if (ret)
		return ret;

	/* firmware download */
	ret = mt76_init_mcu_queue(&dev->mt76, MT_MCUQ_FWDL, MT7925_TXQ_FWDL,
				  MT7925_TX_FWDL_RING_SIZE, MT_TX_RING_BASE);
	if (ret)
		return ret;

	/* rx event */
	ret = mt76_queue_alloc(dev, &dev->mt76.q_rx[MT_RXQ_MCU],
			       MT7925_RXQ_MCU_WM, MT7925_RX_MCU_RING_SIZE,
			       MT_RX_BUF_SIZE, MT_RX_EVENT_RING_BASE);
	if (ret)
		return ret;

	/* rx data */
	ret = mt76_queue_alloc(dev, &dev->mt76.q_rx[MT_RXQ_MAIN],
			       MT7925_RXQ_BAND0, MT7925_RX_RING_SIZE,
			       MT_RX_BUF_SIZE, MT_RX_DATA_RING_BASE);
	if (ret)
		return ret;

	ret = mt76_init_queues(dev, mt792x_poll_rx);
	if (ret < 0)
		return ret;

	netif_napi_add_tx(dev->mt76.tx_napi_dev, &dev->mt76.tx_napi,
			  mt792x_poll_tx);
	napi_enable(&dev->mt76.tx_napi);

	return mt792x_dma_enable(dev);
}

static int mt7925_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	static const struct mt76_driver_ops drv_ops = {
		/* txwi_size = txd size + txp size */
		.txwi_size = MT_TXD_SIZE + sizeof(struct mt76_connac_hw_txp),
		.drv_flags = MT_DRV_TXWI_NO_FREE | MT_DRV_HW_MGMT_TXQ |
			     MT_DRV_AMSDU_OFFLOAD,
		.survey_flags = SURVEY_INFO_TIME_TX |
				SURVEY_INFO_TIME_RX |
				SURVEY_INFO_TIME_BSS_RX,
		.token_size = MT7925_TOKEN_SIZE,
		.tx_prepare_skb = mt7925e_tx_prepare_skb,
		.tx_complete_skb = mt76_connac_tx_complete_skb,
		.rx_check = mt7925_rx_check,
		.rx_skb = mt7925_queue_rx_skb,
		.rx_poll_complete = mt792x_rx_poll_complete,
		.sta_add = mt7925_mac_sta_add,
		.sta_event = mt7925_mac_sta_event,
		.sta_remove = mt7925_mac_sta_remove,
		.update_survey = mt792x_update_channel,
		.set_channel = mt7925_set_channel,
		.link_data_size = sizeof(struct mt76_vif_link),
		.vif_link_add = mt7925_vif_link_add,
		.vif_link_remove = mt7925_vif_link_remove,
	};
	static const struct mt792x_hif_ops mt7925_pcie_ops = {
		.init_reset = mt7925e_init_reset,
		.reset = mt7925e_mac_reset,
		.mcu_init = mt7925e_mcu_init,
		.drv_own = mt792xe_mcu_drv_pmctrl,
		.fw_own = mt792xe_mcu_fw_pmctrl,
	};
	static const struct mt792x_hif_ops mt7927_pcie_ops = {
		.init_reset = mt7925e_init_reset,
		.reset = mt7925e_mac_reset,
		.mcu_init = mt7927e_mcu_init,  /* MT7927 uses custom mcu_init */
		.drv_own = mt792xe_mcu_drv_pmctrl,
		.fw_own = mt792xe_mcu_fw_pmctrl,
	};
	static const struct mt792x_irq_map irq_map = {
		.host_irq_enable = MT_WFDMA0_HOST_INT_ENA,
		.tx = {
			.all_complete_mask = MT_INT_TX_DONE_ALL,
			.mcu_complete_mask = MT_INT_TX_DONE_MCU,
		},
		.rx = {
			.data_complete_mask = HOST_RX_DONE_INT_ENA2,
			.wm_complete_mask = HOST_RX_DONE_INT_ENA0,
		},
	};
	struct ieee80211_ops *ops;
	struct mt76_bus_ops *bus_ops;
	struct mt792x_dev *dev;
	struct mt76_dev *mdev;
	u8 features;
	int ret;
	u16 cmd;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(pdev, BIT(0), pci_name(pdev));
	if (ret)
		return ret;

	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	if (!(cmd & PCI_COMMAND_MEMORY)) {
		cmd |= PCI_COMMAND_MEMORY;
		pci_write_config_word(pdev, PCI_COMMAND, cmd);
	}
	pci_set_master(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
	if (ret < 0)
		return ret;

	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free_pci_vec;

	if (mt7925_disable_aspm)
		mt76_pci_disable_aspm(pdev);

	ops = mt792x_get_mac80211_ops(&pdev->dev, &mt7925_ops,
				      (void *)id->driver_data, &features);
	if (!ops) {
		ret = -ENOMEM;
		goto err_free_pci_vec;
	}

	mdev = mt76_alloc_device(&pdev->dev, sizeof(*dev), ops, &drv_ops);
	if (!mdev) {
		ret = -ENOMEM;
		goto err_free_pci_vec;
	}

	pci_set_drvdata(pdev, mdev);

	dev = container_of(mdev, struct mt792x_dev, mt76);
	dev->fw_features = features;
	/* Use MT7927-specific ops for device 0x7927 */
	dev->hif_ops = (pdev->device == 0x7927) ? &mt7927_pcie_ops : &mt7925_pcie_ops;
	dev->irq_map = &irq_map;
	mt76_mmio_init(&dev->mt76, pcim_iomap_table(pdev)[0]);

	/* Set bus_ops to use original (direct MMIO) operations */
	dev->bus_ops = dev->mt76.bus;

	/* Now set up tasklet and phy structures */
	tasklet_init(&mdev->irq_tasklet, mt792x_irq_tasklet, (unsigned long)dev);

	dev->phy.dev = dev;
	dev->phy.mt76 = &dev->mt76.phy;
	dev->mt76.phy.priv = &dev->phy;

	/* Install custom bus operations with address remapping FIRST */
	/* CRITICAL: Must be done BEFORE MT7927 init so register accesses use L1/L2 windowing */
	bus_ops = devm_kmemdup(dev->mt76.dev, dev->bus_ops, sizeof(*bus_ops),
			       GFP_KERNEL);
	if (!bus_ops) {
		ret = -ENOMEM;
		goto err_free_dev;
	}

	bus_ops->rr = mt7925_rr;
	bus_ops->wr = mt7925_wr;
	bus_ops->rmw = mt7925_rmw;
	dev->mt76.bus = bus_ops;

	/* Initialize PCIe remap registers for MT7927/MT6639 */
	/* CRITICAL: Must be done BEFORE any 0x7xxxx register access! */
	if (pdev->device == 0x7927) {
		u32 val;

		dev_info(mdev->dev, "MT7927 detected, initializing CBInfra remap registers\n");

		/* Set CBInfra PCIe remap using custom bus ops (with L1 windowing) */
		mt76_wr(dev, CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF_ADDR,
			MT7927_CBTOP_PCIE_REMAP_WF_VALUE);
		mt76_wr(dev, CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF_BT_ADDR,
			MT7927_CBTOP_PCIE_REMAP_WF_BT_VALUE);

		/* Verify the remap was set correctly */
		val = mt76_rr(dev, CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF_ADDR);
		dev_info(mdev->dev, "MT7927: CBTOP_PCIE_REMAP_WF    = 0x%08x\n", val);
		val = mt76_rr(dev, CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF_BT_ADDR);
		dev_info(mdev->dev, "MT7927: CBTOP_PCIE_REMAP_WF_BT = 0x%08x\n", val);

		/* PCIE2AP remap will be set in mcu_init AFTER MCU reaches IDLE */
		/* This is critical - MTK sets this AFTER reset but BEFORE firmware load */
	}

	if (!mt7925_disable_aspm && mt76_pci_aspm_supported(pdev))
		dev->aspm_supported = true;

	/* Power control - order matters!
	 * MT7927: Must do WF reset BEFORE taking driver ownership
	 * MT7925: Standard power control sequence
	 */
	if (pdev->device == 0x7927) {
		/* MT7927: Skip initial power control, do reset first */
		mdev->rev = (mt76_rr(dev, MT_HW_CHIPID) << 16) |
			    (mt76_rr(dev, MT_HW_REV) & 0xff);
		dev_info(mdev->dev, "MT7927: ASIC revision: %04x\n", mdev->rev);

		/* Reset WF subsystem first */
		ret = mt7927_wfsys_reset(dev);
		if (ret)
			goto err_free_dev;

		/* NOW take driver ownership after reset */
		ret = __mt792x_mcu_fw_pmctrl(dev);
		if (ret)
			goto err_free_dev;

		ret = __mt792xe_mcu_drv_pmctrl(dev);
		if (ret)
			goto err_free_dev;

		dev_info(mdev->dev, "MT7927: Driver ownership acquired after reset\n");
	} else {
		/* MT7925: Standard power control before reset */
		ret = __mt792x_mcu_fw_pmctrl(dev);
		if (ret)
			goto err_free_dev;

		ret = __mt792xe_mcu_drv_pmctrl(dev);
		if (ret)
			goto err_free_dev;

		mdev->rev = (mt76_rr(dev, MT_HW_CHIPID) << 16) |
			    (mt76_rr(dev, MT_HW_REV) & 0xff);
		dev_info(mdev->dev, "ASIC revision: %04x\n", mdev->rev);

		/* MT_HW_EMI_CTL is MT7925-specific */
		mt76_rmw_field(dev, MT_HW_EMI_CTL, MT_HW_EMI_CTL_SLPPROT_EN, 1);

		/* Standard MT7925 reset */
		ret = mt792x_wfsys_reset(dev);
		if (ret)
			goto err_free_dev;
	}

	mt76_wr(dev, irq_map.host_irq_enable, 0);

	mt76_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xff);

	ret = devm_request_irq(mdev->dev, pdev->irq, mt792x_irq_handler,
			       IRQF_SHARED, KBUILD_MODNAME, dev);
	if (ret)
		goto err_free_dev;

	/* MT7927: Pre-initialize MCU (check IDLE) before DMA setup */
	if (pdev->device == 0x7927) {
		dev_info(mdev->dev, "MT7927: Running pre-initialization sequence\n");
		mt7927e_mcu_pre_init(dev);
		dev_info(mdev->dev, "MT7927: Pre-initialization complete, proceeding with DMA setup\n");
	}

	/* MT7927: Configure PCIe MAC interrupt routing after IRQ setup (mt6639InitPcieInt) */
	if (pdev->device == 0x7927) {
		dev_info(mdev->dev, "MT7927: Configuring PCIe MAC interrupt routing (post-IRQ)\n");
		mt76_wr(dev, MT7927_PCIE_MAC_INT_CONFIG_ADDR, MT7927_PCIE_MAC_INT_CONFIG_VALUE);
		dev_info(mdev->dev, "  PCIE_MAC[0x%x] = 0x%08x\n",
			 MT7927_PCIE_MAC_INT_CONFIG_ADDR, MT7927_PCIE_MAC_INT_CONFIG_VALUE);
	}

	ret = mt7925_dma_init(dev);
	if (ret)
		goto err_free_irq;

	ret = mt7925_register_device(dev);
	if (ret)
		goto err_free_irq;

	return 0;

err_free_irq:
	devm_free_irq(&pdev->dev, pdev->irq, dev);
err_free_dev:
	mt76_free_device(&dev->mt76);
err_free_pci_vec:
	pci_free_irq_vectors(pdev);

	return ret;
}

static void mt7925_pci_remove(struct pci_dev *pdev)
{
	struct mt76_dev *mdev = pci_get_drvdata(pdev);
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);

	mt7925e_unregister_device(dev);
	set_bit(MT76_REMOVED, &mdev->phy.state);
	devm_free_irq(&pdev->dev, pdev->irq, dev);
	mt76_free_device(&dev->mt76);
	pci_free_irq_vectors(pdev);
}

static int mt7925_pci_suspend(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct mt76_dev *mdev = pci_get_drvdata(pdev);
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt76_connac_pm *pm = &dev->pm;
	int i, err, ret;

	pm->suspended = true;
	dev->hif_resumed = false;
	flush_work(&dev->reset_work);
	cancel_delayed_work_sync(&pm->ps_work);
	cancel_work_sync(&pm->wake_work);

	mt7925_roc_abort_sync(dev);

	err = mt792x_mcu_drv_pmctrl(dev);
	if (err < 0)
		goto restore_suspend;

	wait_event_timeout(dev->wait,
			   !dev->regd_in_progress, 5 * HZ);

	/* always enable deep sleep during suspend to reduce
	 * power consumption
	 */
	mt7925_mcu_set_deep_sleep(dev, true);

	mt76_connac_mcu_set_hif_suspend(mdev, true, false);
	ret = wait_event_timeout(dev->wait,
				 dev->hif_idle, 3 * HZ);
	if (!ret) {
		err = -ETIMEDOUT;
		goto restore_suspend;
	}

	napi_disable(&mdev->tx_napi);
	mt76_worker_disable(&mdev->tx_worker);

	mt76_for_each_q_rx(mdev, i) {
		napi_disable(&mdev->napi[i]);
	}

	/* wait until dma is idle  */
	mt76_poll(dev, MT_WFDMA0_GLO_CFG,
		  MT_WFDMA0_GLO_CFG_TX_DMA_BUSY |
		  MT_WFDMA0_GLO_CFG_RX_DMA_BUSY, 0, 1000);

	/* put dma disabled */
	mt76_clear(dev, MT_WFDMA0_GLO_CFG,
		   MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	/* disable interrupt */
	mt76_wr(dev, dev->irq_map->host_irq_enable, 0);

	mt76_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0x0);

	synchronize_irq(pdev->irq);
	tasklet_kill(&mdev->irq_tasklet);

	err = mt792x_mcu_fw_pmctrl(dev);
	if (err)
		goto restore_napi;

	return 0;

restore_napi:
	mt76_for_each_q_rx(mdev, i) {
		napi_enable(&mdev->napi[i]);
	}
	napi_enable(&mdev->tx_napi);

	if (!pm->ds_enable)
		mt7925_mcu_set_deep_sleep(dev, false);

	mt76_connac_mcu_set_hif_suspend(mdev, false, false);
	ret = wait_event_timeout(dev->wait,
				 dev->hif_resumed, 3 * HZ);
	if (!ret)
		err = -ETIMEDOUT;
restore_suspend:
	pm->suspended = false;

	if (err < 0)
		mt792x_reset(&dev->mt76);

	return err;
}

static int mt7925_pci_resume(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	struct mt76_dev *mdev = pci_get_drvdata(pdev);
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	struct mt76_connac_pm *pm = &dev->pm;
	int i, err, ret;

	dev->hif_idle = false;
	err = mt792x_mcu_drv_pmctrl(dev);
	if (err < 0)
		goto failed;

	mt792x_wpdma_reinit_cond(dev);

	/* MT7927: Reconfigure PCIe MAC interrupt routing after resume */
	if (pdev->device == 0x7927) {
		dev_info(mdev->dev, "MT7927: Reconfiguring PCIe MAC interrupt routing (resume)\n");
		mt76_wr(dev, MT7927_PCIE_MAC_INT_CONFIG_ADDR, MT7927_PCIE_MAC_INT_CONFIG_VALUE);
	}

	/* enable interrupt */
	mt76_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xff);
	mt76_connac_irq_enable(&dev->mt76,
			       dev->irq_map->tx.all_complete_mask |
			       MT_INT_RX_DONE_ALL | MT_INT_MCU_CMD);
	mt76_set(dev, MT_MCU2HOST_SW_INT_ENA, MT_MCU_CMD_WAKE_RX_PCIE);

	/* put dma enabled */
	mt76_set(dev, MT_WFDMA0_GLO_CFG,
		 MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	mt76_worker_enable(&mdev->tx_worker);

	mt76_for_each_q_rx(mdev, i) {
		napi_enable(&mdev->napi[i]);
	}
	napi_enable(&mdev->tx_napi);

	local_bh_disable();
	mt76_for_each_q_rx(mdev, i) {
		napi_schedule(&mdev->napi[i]);
	}
	napi_schedule(&mdev->tx_napi);
	local_bh_enable();

	mt76_connac_mcu_set_hif_suspend(mdev, false, false);
	ret = wait_event_timeout(dev->wait,
				 dev->hif_resumed, 3 * HZ);
	if (!ret) {
		err = -ETIMEDOUT;
		goto failed;
	}

	/* restore previous ds setting */
	if (!pm->ds_enable)
		mt7925_mcu_set_deep_sleep(dev, false);

	mt7925_regd_update(dev);
failed:
	pm->suspended = false;

	if (err < 0)
		mt792x_reset(&dev->mt76);

	return err;
}

static void mt7925_pci_shutdown(struct pci_dev *pdev)
{
	mt7925_pci_remove(pdev);
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt7925_pm_ops, mt7925_pci_suspend, mt7925_pci_resume);

static struct pci_driver mt7925_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= mt7925_pci_device_table,
	.probe		= mt7925_pci_probe,
	.remove		= mt7925_pci_remove,
	.shutdown       = mt7925_pci_shutdown,
	.driver.pm	= pm_sleep_ptr(&mt7925_pm_ops),
};

module_pci_driver(mt7925_pci_driver);

MODULE_DEVICE_TABLE(pci, mt7925_pci_device_table);
MODULE_FIRMWARE(MT7925_FIRMWARE_WM);
MODULE_FIRMWARE(MT7925_ROM_PATCH);
MODULE_FIRMWARE(MT7927_FIRMWARE_WM);
MODULE_FIRMWARE(MT7927_ROM_PATCH);
MODULE_AUTHOR("Deren Wu <deren.wu@mediatek.com>");
MODULE_AUTHOR("Lorenzo Bianconi <lorenzo@kernel.org>");
MODULE_DESCRIPTION("MediaTek MT7925E (PCIe) wireless driver");
MODULE_LICENSE("Dual BSD/GPL");
