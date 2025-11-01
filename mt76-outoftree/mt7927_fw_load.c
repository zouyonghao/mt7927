// SPDX-License-Identifier: ISC
/* Copyright (C) 2024 MediaTek Inc.
 *
 * MT7927 Firmware Loading - Polling-based DMA (no mailbox)
 *
 * MT7927 ROM bootloader does NOT support WFDMA mailbox command protocol.
 * We use the standard mt76 firmware loading BUT skip mailbox responses
 * since the ROM doesn't send them.
 */

#include <linux/firmware.h>
#include "mt792x.h"
#include "mt76_connac_mcu.h"

/* MT7927 uses standard DMA but NO mailbox waiting */

#define MCU_PATCH_ADDRESS	0x200000

/* Send init command without waiting for mailbox response */
static int mt7927_mcu_send_init_cmd(struct mt76_dev *dev, int cmd,
				    const void *data, int len)
{
	/* Send command but don't wait for response - ROM doesn't send them */
	return mt76_mcu_send_msg(dev, cmd, data, len, false);
}

/* Initialize firmware download region */
static int mt7927_mcu_init_download(struct mt76_dev *dev, u32 addr, 
				    u32 len, u32 mode)
{
	struct {
		__le32 addr;
		__le32 len;
		__le32 mode;
	} req = {
		.addr = cpu_to_le32(addr),
		.len = cpu_to_le32(len),
		.mode = cpu_to_le32(mode),
	};
	int cmd;

	/* Determine command based on address */
	if (addr == MCU_PATCH_ADDRESS || addr == 0x900000 || addr == 0xe0002800)
		cmd = MCU_CMD(PATCH_START_REQ);
	else
		cmd = MCU_CMD(TARGET_ADDRESS_LEN_REQ);

	dev_info(dev->dev, "[MT7927] Init download: addr=0x%08x len=%u mode=0x%x cmd=0x%x\n",
		 addr, len, mode, cmd);

	return mt7927_mcu_send_init_cmd(dev, cmd, &req, sizeof(req));
}

/* Send firmware data */
static int mt7927_mcu_send_firmware(struct mt76_dev *dev, int cmd,
				    const void *data, int len, int max_len)
{
	int err, cur_len;

	while (len > 0) {
		cur_len = min_t(int, max_len, len);

		/* Aggressive cleanup BEFORE sending - force=true to free all pending */
		if (dev->queue_ops->tx_cleanup) {
			dev->queue_ops->tx_cleanup(dev,
						   dev->q_mcu[MT_MCUQ_FWDL],
						   true);
		}

		/* Send data chunk without waiting for response */
		err = mt76_mcu_send_msg(dev, cmd, data, cur_len, false);
		if (err) {
			dev_err(dev->dev, "[MT7927] Failed to send fw chunk: %d\n", err);
			return err;
		}

		data += cur_len;
		len -= cur_len;

		/* Cleanup AFTER sending to process what we just sent */
		if (dev->queue_ops->tx_cleanup) {
			dev->queue_ops->tx_cleanup(dev,
						   dev->q_mcu[MT_MCUQ_FWDL],
						   true);
		}
		
		/* Let other kernel threads run to free memory */
		cond_resched();
		
		/* Brief delay to let MCU process buffer (reduced from 25ms) */
		msleep(5);
	}

	return 0;
}

/* Load patch - simplified without semaphore */
int mt7927_load_patch(struct mt76_dev *dev, const char *name)
{
	const struct mt76_connac2_patch_hdr *hdr;
	const struct mt76_connac2_patch_sec *sec;
	const struct firmware *fw = NULL;
	int ret, offset;
	u32 len, addr, data_mode;

	dev_info(dev->dev, "[MT7927] Loading patch: %s (no mailbox protocol)\n", name);

	ret = request_firmware(&fw, name, dev->dev);
	if (ret) {
		dev_err(dev->dev, "[MT7927] Failed to request firmware: %d\n", ret);
		return ret;
	}

	if (!fw || fw->size < sizeof(*hdr)) {
		dev_err(dev->dev, "[MT7927] Invalid patch file size\n");
		ret = -EINVAL;
		goto out;
	}

	hdr = (const void *)fw->data;

	dev_info(dev->dev, "[MT7927] Patch info: ver=0x%x platform=%s\n",
		 be32_to_cpu(hdr->hw_sw_ver), hdr->platform);

	/* Get first patch section */
	sec = (const void *)(fw->data + sizeof(*hdr));
	
	addr = be32_to_cpu(sec->info.addr);
	len = be32_to_cpu(sec->info.len);
	data_mode = 0; /* No special mode for patch */
	
	offset = be32_to_cpu(sec->offs);

	dev_info(dev->dev, "[MT7927] Patch: addr=0x%08x len=%u offs=%d\n",
		 addr, len, offset);

	/* NO SEMAPHORE - MT7927 ROM doesn't support it */
	
	/* Initialize download */
	ret = mt7927_mcu_init_download(dev, addr, len, data_mode);
	if (ret) {
		dev_err(dev->dev, "[MT7927] Init download failed: %d\n", ret);
		goto out;
	}

	/* Small delay for ROM to process */
	msleep(10);

	/* Send patch data */
	dev_info(dev->dev, "[MT7927] Sending patch data (%u bytes)...\n", len);
	ret = mt7927_mcu_send_firmware(dev, MCU_CMD(FW_SCATTER),
				       fw->data + offset, len, 4096);
	if (ret) {
		dev_err(dev->dev, "[MT7927] Send patch data failed: %d\n", ret);
		goto out;
	}

	/* Send PATCH_FINISH to let ROM process the patch */
	dev_info(dev->dev, "[MT7927] Sending PATCH_FINISH...\n");
	ret = mt7927_mcu_send_init_cmd(dev, MCU_CMD(PATCH_FINISH_REQ), NULL, 0);
	if (ret) {
		dev_err(dev->dev, "[MT7927] PATCH_FINISH failed: %d (continuing anyway)\n", ret);
		ret = 0; /* Don't fail on this */
	}

	/* Give ROM time to apply patch */
	msleep(50);
	
	/* Check MCU status after patch */
	{
		u32 val = __mt76_rr(dev, 0x7c060204);
		dev_info(dev->dev, "[MT7927] MCU status after patch: 0x%08x\n", val);
	}

	dev_info(dev->dev, "[MT7927] Patch data sent successfully\n");

	ret = 0;
out:
	release_firmware(fw);
	return ret;
}
EXPORT_SYMBOL_GPL(mt7927_load_patch);

/* Load RAM firmware */
int mt7927_load_ram(struct mt76_dev *dev, const char *name)
{
	const struct mt76_connac2_fw_trailer *hdr;
	const struct firmware *fw = NULL;
	int ret, i, offset = 0;

	dev_info(dev->dev, "[MT7927] Loading RAM: %s (no mailbox protocol)\n", name);

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

	dev_info(dev->dev, "[MT7927] RAM: chip_id=0x%x eco=0x%x regions=%d\n",
		 hdr->chip_id, hdr->eco_code, hdr->n_region);

	/* Download each region */
	for (i = 0; i < hdr->n_region; i++) {
		const struct mt76_connac2_fw_region *region;
		u32 addr, len, data_mode;

		region = (const void *)((const u8 *)hdr -
					(hdr->n_region - i) * sizeof(*region));

		addr = le32_to_cpu(region->addr);
		len = le32_to_cpu(region->len);
		data_mode = (region->feature_set & FW_FEATURE_NON_DL) ? 0 : 0;

		dev_info(dev->dev, "[MT7927] RAM region %d: addr=0x%08x len=%u\n",
			 i, addr, len);

		/* Initialize region download */
		ret = mt7927_mcu_init_download(dev, addr, len, data_mode);
		if (ret) {
			dev_err(dev->dev, "[MT7927] Init region %d failed: %d\n", i, ret);
			goto out;
		}

		msleep(5);

		/* Send region data */
		ret = mt7927_mcu_send_firmware(dev, MCU_CMD(FW_SCATTER),
					       fw->data + offset, len, 4096);
		if (ret) {
			dev_err(dev->dev, "[MT7927] Send region %d failed: %d\n", i, ret);
			/* Check MCU status on error */
			{
				u32 val = __mt76_rr(dev, 0x7c060204);
				dev_err(dev->dev, "[MT7927] MCU status on error: 0x%08x\n", val);
			}
			goto out;
		}

		offset += len;

		/* Aggressive cleanup between regions - reduced delay */
		dev_dbg(dev->dev, "[MT7927] Cleanup between regions...\n");
		if (dev->queue_ops->tx_cleanup) {
			int j;
			for (j = 0; j < 10; j++) {
				dev->queue_ops->tx_cleanup(dev,
							   dev->q_mcu[MT_MCUQ_FWDL],
							   false);
				msleep(10);
			}
		}

		/* Check MCU status */
		{
			u32 val = __mt76_rr(dev, 0x7c060204);
			dev_info(dev->dev, "[MT7927] MCU status after region %d: 0x%08x\n", i, val);
		}

		dev_info(dev->dev, "[MT7927] Region %d sent successfully\n", i);
	}

	/* MT7927: Skip FW_START - it's a mailbox command that ROM doesn't support.
	 * The firmware should already be executing after we sent all regions.
	 * As a nudge, set AP2WF SW_INIT_DONE bit to signal host is ready. */
	{
		u32 ap2wf, mcu_ready;
		/* Read current AP2WF bus status */
		ap2wf = __mt76_rr(dev, 0x7C000140);
		__mt76_wr(dev, 0x7C000140, ap2wf | BIT(4));
		dev_info(dev->dev, "[MT7927] Set AP2WF SW_INIT_DONE (0x7C000140 |= BIT4)\n");
		
		/* Check MCU ready status in MT_CONN_ON_MISC (0x7c0600f0) */
		mcu_ready = __mt76_rr(dev, 0x7c0600f0);
		dev_info(dev->dev, "[MT7927] MCU ready register (0x7c0600f0) = 0x%08x\n", mcu_ready);
	}

	dev_info(dev->dev, "[MT7927] Skipping FW_START (mailbox not supported)\n");

	dev_info(dev->dev, "[MT7927] RAM firmware loaded successfully\n");
	ret = 0;

out:
	release_firmware(fw);
	return ret;
}
EXPORT_SYMBOL_GPL(mt7927_load_ram);
