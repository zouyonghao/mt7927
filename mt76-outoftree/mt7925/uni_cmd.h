/* SPDX-License-Identifier: ISC */
/* MT7927 UNI_CMD definitions based on MTK driver */

#ifndef __MT7925_UNI_CMD_H
#define __MT7925_UNI_CMD_H

#include <linux/types.h>

/* UNI_CMD IDs */
#define UNI_CMD_ID_DEVINFO		0x01
#define UNI_CMD_ID_BSSINFO		0x02
#define UNI_CMD_ID_STAREC_INFO		0x03
#define UNI_CMD_ID_BAND_CONFIG		0x08
#define UNI_CMD_ID_SCAN_REQ		0x16

/* UNI_CMD_BSSINFO Tags */
#define UNI_CMD_BSSINFO_TAG_BASIC	0
#define UNI_CMD_BSSINFO_TAG_RLM		2

/* Band definitions */
#define CMD_BAND_2G4			1
#define CMD_BAND_5G			2
#define CMD_BAND_6G			3

struct uni_cmd_bssinfo_rlm {
	__le16 tag;		/* Tag = 0x02 */
	__le16 len;
	u8 primary_channel;
	u8 center_chan_seg0;
	u8 center_chan_seg1;
	u8 bandwidth;
	u8 tx_streams;
	u8 rx_streams;
	u8 ht_op_info1;
	u8 sco;			/* Secondary Channel Offset */
	u8 rf_band;		/* CMD_BAND_2G4/5G/6G */
	u8 pad[3];
} __packed;

#endif /* __MT7925_UNI_CMD_H */
