/*
 * SiI8620 Linux Driver
 *
 * Copyright (C) 2013-2014 Silicon Image, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 * This program is distributed AS-IS WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; INCLUDING without the implied warranty
 * of MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE or NON-INFRINGEMENT.
 * See the GNU General Public License for more details at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/fs.h>
#include <linux/semaphore.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/stringify.h>
#include <linux/uaccess.h>

#include "si_fw_macros.h"
#include "si_infoframe.h"
#include "si_edid.h"
#include "si_mhl_defs.h"
#include "si_mhl2_edid_3d_api.h"
#include "si_mhl_tx_hw_drv_api.h"
#ifdef MEDIA_DATA_TUNNEL_SUPPORT
#include "si_mdt_inputdev.h"
#endif
#include "mhl_rcp_inputdev.h"
#if (INCLUDE_RBP == 1)
#include "mhl_rbp_inputdev.h"
#endif
#include "mhl_linux_tx.h"
#include "mhl_supp.h"
#include "platform.h"
#include "si_mhl_callback_api.h"
#include "si_8620_drv.h"

#define MHL_DRIVER_MINOR_MAX 1

/* Convert a value specified in milliseconds to nanoseconds */
#define MSEC_TO_NSEC(x)	(x * 1000000UL)




static struct class *mhl_class;

static void mhl_tx_destroy_timer_support(struct mhl_dev_context *dev_context);

#define SYS_ATTR_NAME_CONN			connection_state
#define SYS_ATTR_NAME_SPAD			spad
#define		SYS_ATTR_NAME_RAP_IN		SYS_ATTR_NAME_IN
#define MAX_EVENT_STRING_LEN 128
/*
 * Handler for event notifications from the MhlTx layer.
 *
 */
void mhl_event_notify(struct mhl_dev_context *dev_context, u32 event,
	u32 event_param, void *data)
{
	char event_string[MAX_EVENT_STRING_LEN];
	char *envp[] = { event_string, NULL };
	char *buf;
	u32 length;
	u32 count;
	int idx;

	MHL_TX_DBG_INFO("called, event: 0x%08x event_param: 0x%08x\n",
		event, event_param);

	switch (event) {

	case MHL_TX_EVENT_CONNECTION:
		dev_context->mhl_flags |= MHL_STATE_FLAG_CONNECTED;
		init_rcp_input_dev(dev_context);
		sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
			__stringify(SYS_ATTR_NAME_CONN));

		strncpy(event_string, "MHLEVENT=connected",
			MAX_EVENT_STRING_LEN);
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
			envp);
#if (INCLUDE_SII6031 == 1)
		mhl_tx_notify_otg(dev_context, true);
#endif
		break;

	case MHL_TX_EVENT_DISCONNECTION:
		mhl_platform_disconnected();
		dev_context->mhl_flags = 0;
#if (INCLUDE_RBP == 1)
		dev_context->rbp_in_button_code = 0;
		dev_context->rbp_out_button_code = 0;
		dev_context->rbp_err_code = 0;
		dev_context->rbp_send_status = 0;
#endif
		dev_context->rcp_in_key_code = 0;
		dev_context->rcp_out_key_code = 0;
		dev_context->rcp_err_code = 0;
		dev_context->rcp_send_status = 0;
		dev_context->ucp_in_key_code = 0;
		dev_context->ucp_out_key_code = 0;
		dev_context->ucp_err_code = 0;
		dev_context->spad_send_status = 0;

#if (INCLUDE_HID == 1)
		mhl3_hid_remove_all(dev_context);
#endif
#ifdef MEDIA_DATA_TUNNEL_SUPPORT
		mdt_destroy(dev_context);
#endif
#if (INCLUDE_SII6031 == 1)
		mhl_tx_notify_otg(dev_context, false);
#endif
		destroy_rcp_input_dev(dev_context);
		sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
			__stringify(SYS_ATTR_NAME_CONN));

		strncpy(event_string, "MHLEVENT=disconnected",
			MAX_EVENT_STRING_LEN);
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
			envp);
		break;

	case MHL_TX_EVENT_RCP_RECEIVED:
		if (input_dev_rcp) {
			int result;

			result = generate_rcp_input_event(dev_context,
				(uint8_t) event_param);
			if (0 == result)
				si_mhl_tx_rcpk_send(dev_context,
					(uint8_t) event_param);
			else
				si_mhl_tx_rcpe_send(dev_context,
					MHL_RCPE_STATUS_INEFFECTIVE_KEY_CODE);
		} else {
			dev_context->mhl_flags &= ~MHL_STATE_FLAG_RCP_SENT;
			dev_context->mhl_flags |= MHL_STATE_FLAG_RCP_RECEIVED;
			dev_context->rcp_in_key_code = event_param;

			sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
				__stringify(SYS_ATTR_NAME_RCP));

			snprintf(event_string, MAX_EVENT_STRING_LEN,
				"MHLEVENT=received_RCP key code=0x%02x",
				event_param);
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		}
		break;

	case MHL_TX_EVENT_RCPK_RECEIVED:
		if ((dev_context->mhl_flags & MHL_STATE_FLAG_RCP_SENT)
		    && (dev_context->rcp_out_key_code == event_param)) {

			if (!(dev_context->mhl_flags & MHL_STATE_FLAG_RCP_NAK))
					dev_context->rcp_err_code = 0;
			dev_context->mhl_flags |= MHL_STATE_FLAG_RCP_ACK;
			dev_context->mhl_flags &= ~MHL_STATE_FLAG_RCP_SENT;
			MHL_TX_DBG_INFO(
				"Generating RCPK rcvd event, keycode: 0x%02x\n",
				event_param);

			sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
				__stringify(SYS_ATTR_NAME_RCPK));

			snprintf(event_string, MAX_EVENT_STRING_LEN,
				"MHLEVENT=received_RCPK key code=0x%02x",
				event_param);
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		} else {
			MHL_TX_DBG_ERR(
				"Unexpected RCPK rcvd event, keycode: 0x%02x\n",
				event_param);
		}
		break;

	case MHL_TX_EVENT_RCPE_RECEIVED:
		if (input_dev_rcp) {
			/* do nothing */
		} else {
			if (dev_context->mhl_flags & MHL_STATE_FLAG_RCP_SENT) {

				dev_context->rcp_err_code = event_param;
				dev_context->mhl_flags |=
					MHL_STATE_FLAG_RCP_NAK;

				MHL_TX_DBG_INFO(
					"Generating RCPE received event, "
					"error code: 0x%02x\n", event_param);

				sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
					__stringify(SYS_ATTR_NAME_RCPK));

				snprintf(event_string, MAX_EVENT_STRING_LEN,
					"MHLEVENT=rcvd_RCPE error code=0x%02x",
					event_param);
				kobject_uevent_env(&dev_context->mhl_dev->kobj,
					KOBJ_CHANGE, envp);
			} else {
				MHL_TX_DBG_ERR(
					"Ignoring unexpected RCPE received "
					"event, error code: 0x%02x\n",
					event_param);
			}
		}
		break;

	case MHL_TX_EVENT_UCP_RECEIVED:
		if (input_dev_ucp) {
			if (event_param <= 0xFF)
				si_mhl_tx_ucpk_send(dev_context,
					(uint8_t) event_param);
			else
				si_mhl_tx_ucpe_send(dev_context,
					MHL_UCPE_STATUS_INEFFECTIVE_KEY_CODE);
		} else {
			dev_context->mhl_flags &= ~MHL_STATE_FLAG_UCP_SENT;
			dev_context->mhl_flags |= MHL_STATE_FLAG_UCP_RECEIVED;
			dev_context->ucp_in_key_code = event_param;
			sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
				__stringify(SYS_ATTR_NAME_UCP));

			snprintf(event_string, MAX_EVENT_STRING_LEN,
				"MHLEVENT=received_UCP key code=0x%02x",
				event_param);
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		}
		break;

	case MHL_TX_EVENT_UCPK_RECEIVED:
		if ((dev_context->mhl_flags & MHL_STATE_FLAG_UCP_SENT) &&
			(dev_context->ucp_out_key_code == event_param)) {

			dev_context->mhl_flags |= MHL_STATE_FLAG_UCP_ACK;

			MHL_TX_DBG_INFO("Generating UCPK received event, "
				"keycode: 0x%02x\n", event_param);

			sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
				__stringify(SYS_ATTR_NAME_UCPK));

			snprintf(event_string, MAX_EVENT_STRING_LEN,
				 "MHLEVENT=received_UCPK key code=0x%02x",
				 event_param);
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		} else {
			MHL_TX_DBG_ERR("Ignoring unexpected UCPK received "
				"event, keycode: 0x%02x\n", event_param);
		}
		break;

	case MHL_TX_EVENT_UCPE_RECEIVED:
		if (dev_context->mhl_flags & MHL_STATE_FLAG_UCP_SENT) {

			dev_context->ucp_err_code = event_param;
			dev_context->mhl_flags |= MHL_STATE_FLAG_UCP_NAK;

			MHL_TX_DBG_INFO("Generating UCPE received event, "
				"error code: 0x%02x\n", event_param);

			sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
				__stringify(SYS_ATTR_NAME_UCPK));

			snprintf(event_string, MAX_EVENT_STRING_LEN,
				"MHLEVENT=received_UCPE error code=0x%02x",
				event_param);
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		} else {
			MHL_TX_DBG_ERR("Ignoring unexpected UCPE received "
				"event, error code: 0x%02x\n",
				event_param);
		}
		break;
#if (INCLUDE_RBP == 1)
	case MHL_TX_EVENT_RBP_RECEIVED:
		if (input_dev_rbp) {
			if (((event_param >= 0x01) && (event_param <= 0x07)) ||
			    ((event_param >= 0x20) && (event_param <= 0x21)) ||
			    ((event_param >= 0x30) && (event_param <= 0x35)))
				si_mhl_tx_rbpk_send(dev_context,
					(uint8_t) event_param);
			else
				si_mhl_tx_rbpe_send(dev_context,
					MHL_RBPE_STATUS_INEFFECTIVE_BUTTON_CODE
					);
		} else {
			dev_context->mhl_flags |= MHL_STATE_FLAG_RBP_RECEIVED;
			dev_context->rbp_in_button_code = event_param;

			sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
				__stringify(SYS_ATTR_NAME_RBP));

			snprintf(event_string, MAX_EVENT_STRING_LEN,
				"MHLEVENT=received_RBP button code=0x%02x",
				event_param);
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		}
		break;

	case MHL_TX_EVENT_RBPK_RECEIVED:
		MHL_TX_DBG_INFO("RBPK received\n");
		if ((dev_context->mhl_flags & MHL_STATE_FLAG_RBP_SENT)
		    && (dev_context->rbp_out_button_code == event_param)) {

			dev_context->mhl_flags |= MHL_STATE_FLAG_RBP_ACK;

			MHL_TX_DBG_INFO(
				"Generating RBPK rcvd event, button code: 0x%02x\n",
				event_param);

			sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
				__stringify(SYS_ATTR_NAME_RBPK));

			snprintf(event_string, MAX_EVENT_STRING_LEN,
				"MHLEVENT=received_RBPK button code=0x%02x",
				event_param);
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		} else {
			MHL_TX_DBG_ERR(
				"Unexpected RBPK rcvd event, button code: 0x%02x\n",
				event_param);
		}
		break;

	case MHL_TX_EVENT_RBPE_RECEIVED:
		MHL_TX_DBG_INFO("RBPE received\n");
		if (input_dev_rbp) {
			/* do nothing */
		} else {
			if (dev_context->mhl_flags & MHL_STATE_FLAG_RBP_SENT) {

				dev_context->rbp_err_code = event_param;
				dev_context->mhl_flags |=
					MHL_STATE_FLAG_RBP_NAK;

				MHL_TX_DBG_INFO(
					"Generating RBPE received event, "
					"error code: 0x%02x\n", event_param);

				sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
					__stringify(SYS_ATTR_NAME_RBPK));

				snprintf(event_string, MAX_EVENT_STRING_LEN,
					"MHLEVENT=rcvd_RBPE error code=0x%02x",
					event_param);
				kobject_uevent_env(&dev_context->mhl_dev->kobj,
					KOBJ_CHANGE, envp);
			} else {
				MHL_TX_DBG_ERR(
					"Ignoring unexpected RBPE received "
					"event, error code: 0x%02x\n",
					event_param);
			}
		}
		break;
#endif
	case MHL_TX_EVENT_SPAD_RECEIVED:
		length = event_param;
		buf = data;

		sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
			__stringify(SYS_ATTR_NAME_SPAD));

		idx = snprintf(event_string, MAX_EVENT_STRING_LEN,
			"MHLEVENT=SPAD_CHG length=0x%02x data=", length);

		count = 0;
		while (buf && idx < MAX_EVENT_STRING_LEN) {
			if (count >= length)
				break;

			idx += snprintf(&event_string[idx],
				MAX_EVENT_STRING_LEN - idx, "0x%02x ",
				buf[count]);
			count++;
		}

		if (idx < MAX_EVENT_STRING_LEN) {
			kobject_uevent_env(&dev_context->mhl_dev->kobj,
				KOBJ_CHANGE, envp);
		} else {
			MHL_TX_DBG_ERR(
				"Buffer too small for scratch pad data!\n");
		}
		break;

	case MHL_TX_EVENT_POW_BIT_CHG:
		MHL_TX_DBG_INFO("Generating VBUS power bit change "
				"event, POW bit is %s\n",
				event_param ? "ON" : "OFF");
		snprintf(event_string, MAX_EVENT_STRING_LEN,
			 "MHLEVENT=MHL VBUS power %s",
			 event_param ? "ON" : "OFF");
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
				   envp);
		break;

	case MHL_TX_EVENT_RAP_RECEIVED:
		MHL_TX_DBG_INFO("Generating RAP received event, "
				"action code: 0x%02x\n", event_param);

		sysfs_notify(&dev_context->mhl_dev->kobj, NULL,
			     __stringify(SYS_ATTR_NAME_RAP_IN));

		snprintf(event_string, MAX_EVENT_STRING_LEN,
			 "MHLEVENT=received_RAP action code=0x%02x",
			 event_param);
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
				   envp);
		break;

	case MHL_TX_EVENT_BIST_READY_RECEIVED:
		MHL_TX_DBG_INFO("Received BIST_READY 0x%02X\n", event_param);
		snprintf(event_string, MAX_EVENT_STRING_LEN,
			 "MHLEVENT=received_BIST_READY code=0x%02x",
			 event_param);
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
				   envp);
		break;

	case MHL_TX_EVENT_BIST_TEST_DONE:
		MHL_TX_DBG_INFO("Triggered BIST test completed.\n");
		snprintf(event_string, MAX_EVENT_STRING_LEN,
			 "MHLEVENT=BIST_complete");
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
				   envp);
		break;

	case MHL_TX_EVENT_BIST_STATUS_RECEIVED:
		{
			struct bist_stat_info *stat =
			    (struct bist_stat_info *)data;
			MHL_TX_DBG_INFO("Received BIST_RETURN_STAT\n");
			if (stat != NULL) {
				MHL_TX_DBG_INFO
				    ("eCBUS Rx: 0x%04X,"
					" eCBUS Tx: 0x04X,"
					" AV_LINK: 0x%04X\n",
					stat->e_cbus_local_stat,
					stat->e_cbus_remote_stat,
					stat->avlink_stat);
				snprintf(event_string, MAX_EVENT_STRING_LEN,
					"MHLEVENT=received_BIST_STATUS "\
					"e_cbus_rx=0x%04x"
					"e_cbus_tx=0x%04x"
					" av_link=0x%04x",
					stat->e_cbus_local_stat,
					stat->e_cbus_remote_stat,
					stat->avlink_stat);
				kobject_uevent_env(&dev_context->
					mhl_dev->kobj, KOBJ_CHANGE, envp);
			} else {
				MHL_TX_DBG_INFO("\n");
			}
		}
		break;
	case MHL_TX_EVENT_T_RAP_MAX_EXPIRED:
		MHL_TX_DBG_INFO("T_RAP_MAX expired\n");
		snprintf(event_string, MAX_EVENT_STRING_LEN,
			 "MHLEVENT=T_RAP_MAX_EXPIRED");
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
				   envp);

		break;
	case MHL_EVENT_AUD_DELAY_RCVD:
		MHL_TX_DBG_INFO("Audio Delay burst received\n");
		snprintf(event_string, MAX_EVENT_STRING_LEN,
			 "MHLEVENT=AUD_DELAY_RCVD");
		kobject_uevent_env(&dev_context->mhl_dev->kobj, KOBJ_CHANGE,
				   envp);

		break;
	default:
		MHL_TX_DBG_ERR("called with unrecognized event code!\n");
		break;
	}
}

void process_si_adopter_id(struct mhl_dev_context *dev_context,
			struct si_adopter_id_data *p_burst)
{
	uint8_t opcode = p_burst->hdr.op_code;
	uint8_t checksum;
	size_t total_size;
	total_size = SII_OFFSETOF(struct si_adopter_id_data, hdr.checksum)
			+  p_burst->hdr.remaining_length;
	checksum = calculate_generic_checksum(p_burst, 0, total_size);
	MHL_TX_DBG_INFO("total_size: 0x%02X"
		" id: %02X%02X"
		" rem_len: 0x%02X"
		" cksum: 0x%02X"
		" opcode: 0x%02X\n",
		total_size,
		p_burst->hdr.burst_id.high,
		p_burst->hdr.burst_id.low,
		p_burst->hdr.remaining_length,
		p_burst->hdr.checksum,
		p_burst->hdr.op_code)
	if (checksum) {
		MHL_TX_DBG_ERR("%schecksum error 0x%02X %s\n",
			ANSI_ESC_RED_TEXT,
			checksum,
			ANSI_ESC_RESET_TEXT)
	} else
	switch (opcode) {
	case EDID_BLOCK: {
		struct edid_3d_data_t *edid_context;
		edid_context = dev_context->edid_parser_context;
		MHL_TX_DBG_INFO("EDID_BLOCK "
			"blk_num: %02X\n",
			p_burst->opcode_data.edid_blk.block_num)
			process_emsc_edid_sub_payload(edid_context, p_burst);
		}
		break;
	case EDID_STOP:
		MHL_TX_DBG_ERR("EDID_STOP: %d\n", total_size)
		break;
	}

}

/*
 * Called from the Titan interrupt handler to parse data
 * received from the eSMC BLOCK hardware. Process all waiting input
 * buffers. If multiple messages are sent in the same BLOCK Command buffer,
 * they are sorted out here also.
 */
static bool emsc_burst_valid(u8 *data, int available)
{
	u16 id;
	int size;

	if (available < 2)
		return false;
	id = (data[0] << 8) | data[1];
	switch (id) {
	case burst_id_HID_PAYLOAD:
		if (available < 3)
			return false;
		size = 3 + data[2];
		break;
	case burst_id_BLK_RCV_BUFFER_INFO:
		size = 4;
		break;
	case burst_id_BITS_PER_PIXEL_FMT:
	case burst_id_HEV_VIC:
	case burst_id_3D_VIC:
	case burst_id_3D_DTD:
	case burst_id_EMSC_SUPPORT:
		if (available < 6)
			return false;
		size = 6 + 2 * data[5];
		break;
	case burst_id_VC_ASSIGN:
	case burst_id_VC_CONFIRM:
		if (available < 6)
			return false;
		size = 6 + 3 * data[5];
		break;
	case burst_id_HEV_DTDA:
	case burst_id_HEV_DTDB:
	case burst_id_ADT_BURSTID:
	case burst_id_BIST_SETUP:
	case burst_id_BIST_RETURN_STAT:
		size = 16;
		break;
	case burst_id_AUD_DELAY:
		size = 6;
		break;
	default:
		if (id != SILICON_IMAGE_ADOPTER_ID &&
		    id != MHL_TEST_ADOPTER_ID && id < adopter_id_RANGE_START)
			return false;
		if (available < 3)
			return false;
		size = 3 + data[2];
		if (id == SILICON_IMAGE_ADOPTER_ID &&
		    (size < 5 || (available >= 5 && data[4] == EDID_BLOCK &&
				 size != sizeof(struct si_adopter_id_data))))
			return false;
	}
	return size <= available;
}

static void si_mhl_tx_emsc_received(struct mhl_dev_context *context)
{
	struct drv_hw_context *hw_context =
		(struct drv_hw_context *)(&context->drv_context);
	int length, index, pass;
	uint16_t burst_id;
	uint8_t *prbuf, *pmsg;

	while (si_mhl_tx_drv_peek_block_input_buffer(context,
		&prbuf, &length) == 0) {
		index = 0;
		pass = 0;
/*		dump_array(DBG_MSG_LEVEL_INFO, "si_mhl_tx_emsc_received",
			prbuf, length); */
		do {
			struct MHL_burst_id_t *p_burst_id;
			pmsg = &prbuf[index];
			if (!emsc_burst_valid(pmsg, length - index))
				break;
			p_burst_id = (struct MHL_burst_id_t *)pmsg;
			burst_id = (((uint16_t)pmsg[0]) << 8) | pmsg[1];
			/* Move past the BURST ID */
			index += sizeof(*p_burst_id);

			switch (burst_id) {
			case burst_id_HID_PAYLOAD:
#if (INCLUDE_HID == 1)
				build_received_hid_message(context,
					&pmsg[3], pmsg[2]);
#endif
				index += (1 + pmsg[2]);
				break;

			case burst_id_BLK_RCV_BUFFER_INFO:
				hw_context->block_protocol.peer_blk_rx_buf_max
					= pmsg[3];
				hw_context->block_protocol.peer_blk_rx_buf_max
					<<= 8;
				hw_context->block_protocol.peer_blk_rx_buf_max
					|= pmsg[2];

				hw_context->block_protocol.
					peer_blk_rx_buf_avail =
					hw_context->block_protocol.
					peer_blk_rx_buf_max -
					(EMSC_RCV_BUFFER_DEFAULT -
						hw_context->block_protocol.
						peer_blk_rx_buf_avail);
				MHL_TX_DBG_INFO(
					"Total PEER Buffer Size: %d\n",
					hw_context->block_protocol.
					peer_blk_rx_buf_max);

				index += 2;
				break;

			case burst_id_BITS_PER_PIXEL_FMT:
				index += (4 + (2 * pmsg[5]));
				/*
				 * Inform the rest of the code of the
				 * acknowledgment
				 */
				break;
				/*
				 * Any BURST_ID reported by EMSC_SUPPORT can be
				 * sent using eMSC BLOCK
				 */
			case burst_id_HEV_DTDA:{
				struct MHL3_hev_dtd_a_data_t *p_dtda =
					(struct MHL3_hev_dtd_a_data_t *)
					p_burst_id;
				index += sizeof(*p_dtda) - sizeof(*p_burst_id);
				}
				break;
			case burst_id_HEV_DTDB:{
				struct MHL3_hev_dtd_b_data_t *p_dtdb =
				    (struct MHL3_hev_dtd_b_data_t *) p_burst_id;
				index +=
				    sizeof(*p_dtdb) -
				    sizeof(*p_burst_id);
				}
				/* for now, as a robustness excercise, adjust
				 * index according to the this burst field
				 */
				break;
			case burst_id_HEV_VIC:{
				struct MHL3_hev_vic_data_t *p_burst;
				p_burst = (struct MHL3_hev_vic_data_t *)
					p_burst_id;
				index += sizeof(*p_burst) -
					sizeof(*p_burst_id) -
					sizeof(p_burst->video_descriptors) +
					sizeof(p_burst->video_descriptors[0]) *
					p_burst->num_entries_this_burst;
				}
				break;
			case burst_id_3D_VIC:{
				struct MHL3_hev_vic_data_t *p_burst =
					(struct MHL3_hev_vic_data_t *)
					p_burst_id;
				index += sizeof(*p_burst) -
					sizeof(*p_burst_id) -
					sizeof(p_burst->video_descriptors) +
					sizeof(p_burst->video_descriptors[0]) *
					p_burst->num_entries_this_burst;
				}
				break;
			case burst_id_3D_DTD:{
				struct MHL2_video_format_data_t *p_burst =
					(struct MHL2_video_format_data_t *)
					p_burst_id;
				index += sizeof(*p_burst) -
					sizeof(*p_burst_id) -
					sizeof(p_burst->video_descriptors) +
					sizeof(p_burst->video_descriptors[0]) *
					p_burst->num_entries_this_burst;
				}
				break;
			case burst_id_VC_ASSIGN:{
				struct SI_PACK_THIS_STRUCT tdm_alloc_burst
					*p_burst;
				p_burst = (struct SI_PACK_THIS_STRUCT
					tdm_alloc_burst*)p_burst_id;
				index += sizeof(*p_burst) -
					sizeof(*p_burst_id) -
					sizeof(p_burst->reserved) -
					sizeof(p_burst->vc_info) +
					sizeof(p_burst->vc_info[0]) *
					p_burst->num_entries_this_burst;
				}
				break;
			case burst_id_VC_CONFIRM:{
				struct SI_PACK_THIS_STRUCT tdm_alloc_burst
					*p_burst;
				p_burst = (struct SI_PACK_THIS_STRUCT
					tdm_alloc_burst*)p_burst_id;
				index += sizeof(*p_burst) -
					sizeof(*p_burst_id) -
					sizeof(p_burst->reserved) -
					sizeof(p_burst->vc_info) +
					sizeof(p_burst->vc_info[0]) *
					p_burst->num_entries_this_burst;
				}
				break;
			case burst_id_AUD_DELAY:{
				struct MHL3_audio_delay_burst_t *p_burst =
					(struct MHL3_audio_delay_burst_t *)
					p_burst_id;
				index += sizeof(*p_burst) -
					sizeof(*p_burst_id) -
					sizeof(p_burst->reserved);
				}
				break;
			case burst_id_ADT_BURSTID:{
				struct MHL3_adt_data_t *p_burst =
					(struct MHL3_adt_data_t *)p_burst_id;
				index += sizeof(*p_burst) - sizeof(*p_burst_id);
				}
				break;
			case burst_id_BIST_SETUP:{
				struct SI_PACK_THIS_STRUCT
					bist_setup_burst * p_bist_setup;
				p_bist_setup = (struct SI_PACK_THIS_STRUCT
					bist_setup_burst*)p_burst_id;
				index += sizeof(*p_bist_setup) -
					sizeof(*p_burst_id);
				}
				break;
			case burst_id_BIST_RETURN_STAT:{
				struct SI_PACK_THIS_STRUCT
					bist_return_stat_burst
					*p_bist_return;
				p_bist_return = (struct SI_PACK_THIS_STRUCT
					bist_return_stat_burst*)p_burst_id;
				index += sizeof(*p_bist_return) -
					sizeof(*p_burst_id);
				}
				break;
			case burst_id_EMSC_SUPPORT:{
				/*
				 * For robustness, adjust index
				 * according to the num-entries this
				 * burst field
				 */
				struct MHL3_emsc_support_data_t *p_burst =
					(struct MHL3_emsc_support_data_t *)
					p_burst_id;
				int i;
				index += sizeof(*p_burst) -
					sizeof(*p_burst_id) -
					sizeof(p_burst->payload) +
					sizeof(p_burst->payload.burst_ids[0]) *
					p_burst->num_entries_this_burst;
				for (i = 0;
					i < p_burst->num_entries_this_burst;
					++i) {
					enum BurstId_e burst_id =
					BURST_ID(p_burst->payload.burst_ids[i]);
					MHL_TX_DBG_INFO("BURST_ID: %04X\n",
						burst_id)
					switch (burst_id) {
					case SILICON_IMAGE_ADOPTER_ID:
						context->sii_adopter_id = true;
						break;
					case burst_id_HID_PAYLOAD:
						break;
					default:
						MHL_TX_DBG_ERR("%sunexpected"
							" burst/adopter id:%04X%s\n",
							ANSI_ESC_RED_TEXT,
							burst_id,
							ANSI_ESC_RESET_TEXT)
					}
				}
				}
				break;
			case SILICON_IMAGE_ADOPTER_ID:
				{
				struct si_adopter_id_data *p_burst =
					(struct si_adopter_id_data *)
						p_burst_id;

				index +=
				sizeof(p_burst->hdr.remaining_length)
					+ p_burst->hdr.remaining_length;
				process_si_adopter_id(context, p_burst);

				}
				break;
			default:
				if ((MHL_TEST_ADOPTER_ID == burst_id) ||
					(burst_id >= adopter_id_RANGE_START)) {
					struct EMSC_BLK_ADOPT_ID_PAYLD_HDR*
					p_adopter_id = (struct
						EMSC_BLK_ADOPT_ID_PAYLD_HDR*)
						pmsg;
					MHL_TX_DBG_ERR(
						"Bad ADOPTER_ID: %04X\n",
						burst_id);
					index += 1 + p_adopter_id->remaining_length;
				} else {
					MHL_TX_DBG_ERR(
						"Bad BURST ID: %04X\n",
						burst_id);
					index = length;
				}
				break;
			}

			length -= index;
		} while (length > 0);
		si_mhl_tx_drv_free_block_input_buffer(context);
	}
}

static void check_drv_intr_flags(struct mhl_dev_context *dev_context)
{
	enum cbus_mode_e cbus_mode;
	cbus_mode = si_mhl_tx_drv_get_cbus_mode(dev_context);
	if (dev_context->intr_info.flags & DRV_INTR_CBUS_ABORT)
		process_cbus_abort(dev_context);

	if (dev_context->intr_info.flags & DRV_INTR_WRITE_BURST)
		si_mhl_tx_process_write_burst_data(dev_context);

	/* sometimes SET_INT and WRITE_STAT come at the
	 * same time. Always process WRITE_STAT first.
	 */
	if (dev_context->intr_info.flags & DRV_INTR_WRITE_STAT)
		si_mhl_tx_got_mhl_status(dev_context,
			&dev_context->intr_info.dev_status);

	if (dev_context->intr_info.flags & DRV_INTR_SET_INT)
		si_mhl_tx_got_mhl_intr(dev_context,
			dev_context->intr_info.int_msg[0],
			dev_context->intr_info.int_msg[1]);

	if (dev_context->intr_info.flags & DRV_INTR_MSC_DONE)
		si_mhl_tx_msc_command_done(dev_context,
			dev_context->intr_info.msc_done_data);

	if (dev_context->intr_info.flags & DRV_INTR_EMSC_INCOMING)
		si_mhl_tx_emsc_received(dev_context);

	if (dev_context->intr_info.flags & DRV_INTR_HPD_CHANGE)
		si_mhl_tx_notify_downstream_hpd_change
		    (dev_context, dev_context->intr_info.hpd_status);

	if (dev_context->intr_info.flags & DRV_INTR_MSC_RECVD) {
		dev_context->msc_msg_arrived = true;
		dev_context->msc_msg_sub_command =
		    dev_context->intr_info.msc_msg[0];
		dev_context->msc_msg_data =
		    dev_context->intr_info.msc_msg[1];
		si_mhl_tx_process_events(dev_context);
	}

	if (DRV_INTR_COC_CAL & dev_context->intr_info.flags) {
		bool do_test_now = false;
		switch (cbus_mode) {
		case CM_TRANSITIONAL_TO_eCBUS_S_CAL_BIST:
			if (0 == dev_context->bist_setup.avlink_duration)  {
				MHL_TX_DBG_ERR("infinite av_link duration\n")
				do_test_now = false;
			} else if (BIST_TRIGGER_ECBUS_AV_LINK_MASK &
				dev_context->bist_trigger_info) {
				MHL_TX_DBG_ERR("%sAV_LINK BIST start%s\n",
					ANSI_ESC_GREEN_TEXT,
					ANSI_ESC_RESET_TEXT)
				do_test_now = true;
			}

			if (BIST_TRIGGER_ECBUS_TX_RX_MASK &
				dev_context->bist_trigger_info) {
				MHL_TX_DBG_ERR("%seCBUS BIST start%s\n",
					ANSI_ESC_GREEN_TEXT,
					ANSI_ESC_RESET_TEXT)
				do_test_now = true;
			}
			if (!do_test_now)
				;
			else if (dev_context->misc_flags.flags.bist_role_TE)
				start_bist_initiator_test(dev_context);
			else
				initiate_bist_test(dev_context);
			break;
		default:
			;
		}
	}
	if (DRV_INTR_TDM_SYNC & dev_context->intr_info.flags) {
		switch (cbus_mode) {
		case CM_eCBUS_S_BIST:
		case CM_eCBUS_D_BIST:
			break;
		case CM_eCBUS_S_AV_BIST:
		case CM_eCBUS_D_AV_BIST:
			if (dev_context->bist_setup.avlink_duration)  {
				MHL_TX_DBG_ERR("finite av_link duration\n")
			} else if (BIST_TRIGGER_ECBUS_AV_LINK_MASK &
					dev_context->bist_trigger_info) {
				if (dev_context->
					misc_flags.flags.bist_role_TE) {
					MHL_TX_DBG_ERR("\n")
					start_bist_initiator_test(dev_context);
				} else {
					MHL_TX_DBG_ERR("\n")
					initiate_bist_test(dev_context);
				}
			} else {
				MHL_TX_DBG_ERR("infinite av_link duration\n")
			}
			break;
		case CM_eCBUS_S:
		case CM_eCBUS_D:
			/*
			   MHL3 spec requires the process of reading
			   the remainder of XDEVCAP and all of DEVCAP
			   to be deferred until after the first
			   transition to eCBUS mode.
			   Check DCAP_RDY, initiate XDEVCAP read here.
			 */
			if (MHL_STATUS_DCAP_RDY & dev_context->status_0)
				si_mhl_tx_ecbus_started(dev_context);
			if (dev_context->
				misc_flags.flags.have_complete_devcap) {
				if (dev_context->misc_flags.flags.mhl_hpd &&
				   !dev_context->edid_valid) {
					si_mhl_tx_initiate_edid_sequence(
					    dev_context->edid_parser_context);
				}
			}
			break;
		default:
			MHL_TX_DBG_ERR("%sunexpected CBUS mode%s:%s\n",
				ANSI_ESC_RED_TEXT,
				si_mhl_tx_drv_get_cbus_mode_str(cbus_mode),
				ANSI_ESC_RESET_TEXT)

		}
	}
}
/*
 * Interrupt handler for MHL transmitter interrupts.
 *
 * @irq:	The number of the asserted IRQ line that caused
 *		this handler to be called.
 * @data:	Data pointer passed when the interrupt was enabled,
 *		which in this case is a pointer to an mhl_dev_context struct.
 *
 * Always returns IRQ_HANDLED.
 */
static irqreturn_t mhl_irq_handler(int irq, void *data)
{
	struct mhl_dev_context *dev_context = (struct mhl_dev_context *)data;

	MHL_TX_DBG_INFO("called\n");

	if (!down_interruptible(&dev_context->isr_lock)) {
		int loop_limit = 5;
		if (dev_context->dev_flags & DEV_FLAG_SHUTDOWN)
			goto irq_done;
		if (dev_context->dev_flags & DEV_FLAG_COMM_MODE)
			goto irq_done;

		do {
			memset(&dev_context->intr_info, 0,
				sizeof(*(&dev_context->intr_info)));

			dev_context->intr_info.edid_parser_context =
				dev_context->edid_parser_context;

			dev_context->drv_info->
				mhl_device_isr((struct drv_hw_context *)
				(&dev_context->drv_context),
				&dev_context->intr_info);

			/* post process events raised by interrupt handler */
			if (dev_context->intr_info.flags &
				DRV_INTR_DISCONNECT) {
				dev_context->misc_flags.flags.rap_content_on =
					false;
				dev_context->misc_flags.flags.mhl_rsen = false;
				dev_context->mhl_connection_event = true;
				dev_context->mhl_connected =
					MHL_TX_EVENT_DISCONNECTION;
				si_mhl_tx_process_events(dev_context);
			} else {
				if (dev_context->intr_info.flags &
					DRV_INTR_CONNECT) {
					dev_context->misc_flags.flags.
						rap_content_on = true;
					dev_context->rap_in_sub_command =
						MHL_RAP_CONTENT_ON;
					dev_context->misc_flags.flags.mhl_rsen =
						true;
					dev_context->mhl_connection_event =
						true;
					dev_context->mhl_connected =
						MHL_TX_EVENT_CONNECTION;
					si_mhl_tx_process_events(dev_context);
				}


				check_drv_intr_flags(dev_context);
			}
			/*
			 * Send any messages that may have been queued up
			 * as the result of interrupt processing.
			 */
			si_mhl_tx_drive_states(dev_context);
			if (si_mhl_tx_drv_get_cbus_mode(dev_context) >=
			    CM_eCBUS_S) {
				si_mhl_tx_push_block_transactions(dev_context);
			}
		} while ((--loop_limit > 0) && is_interrupt_asserted());
irq_done:
		up(&dev_context->isr_lock);
	}

	return IRQ_HANDLED;
}

/* APIs provided by the MHL layer to the lower level driver */

int mhl_handle_power_change_request(struct device *parent_dev, bool power_up)
{
	struct mhl_dev_context *dev_context;
	int status;

	dev_context = dev_get_drvdata(parent_dev);

	MHL_TX_DBG_INFO("\n");

	/* Power down the MHL transmitter hardware. */
	status = down_interruptible(&dev_context->isr_lock);
	if (status) {
		MHL_TX_DBG_ERR("failed to acquire ISR semaphore,"
			       "status: %d\n", status);
		goto done;
	}

	if (power_up) {
		dev_context->dev_flags &= ~DEV_FLAG_POWER_OFF;
		status = si_mhl_tx_initialize(dev_context);
		if (status)
			dev_context->dev_flags |= DEV_FLAG_POWER_OFF;
	} else {
		dev_context->dev_flags |= DEV_FLAG_POWER_OFF;
		status = si_mhl_tx_shutdown(dev_context);
	}

	up(&dev_context->isr_lock);
	/* Drain old callbacks before a subsequent power-up clears POWER_OFF. */
	if (!power_up)
		flush_workqueue(dev_context->timer_work_queue);
done:
	return status;

}


static ssize_t connection_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mhl_dev_context *context = dev_get_drvdata(dev);
	int connected;

	down(&context->isr_lock);
	connected = !!(context->mhl_flags & MHL_STATE_FLAG_CONNECTED);
	up(&context->isr_lock);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			connected ? "connected" : "disconnected");
}

static struct device_attribute mhl_attributes[] = {
	__ATTR(connection_state, 0444, connection_state_show, NULL),
	__ATTR_NULL,
};

int mhl_tx_init(const struct mhl_drv_info *info, struct device *parent)
{
	struct mhl_dev_context *context;
	int ret;

	context = kzalloc(sizeof(*context) + info->drv_context_size, GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	context->signature = MHL_DEV_CONTEXT_SIGNATURE;
	context->drv_info = info;
	sema_init(&context->isr_lock, 1);
	INIT_LIST_HEAD(&context->timer_list);
	context->timer_work_queue = create_singlethread_workqueue(MHL_DRIVER_NAME);
	if (!context->timer_work_queue) {
		ret = -ENOMEM;
		goto free_context;
	}
#if INCLUDE_HID
	context->hid_work_queue = create_singlethread_workqueue("mhl_hid");
	if (!context->hid_work_queue) {
		ret = -ENOMEM;
		goto free_timers;
	}
#endif
	mhl_class = class_create(THIS_MODULE, "mhl");
	if (IS_ERR(mhl_class)) {
		ret = PTR_ERR(mhl_class);
		mhl_class = NULL;
		goto free_hid;
	}
	mhl_class->dev_attrs = mhl_attributes;
	context->mhl_dev = device_create(mhl_class, parent, 0, context,
					MHL_DEVICE_NAME);
	if (IS_ERR(context->mhl_dev)) {
		ret = PTR_ERR(context->mhl_dev);
		goto free_class;
	}
	context->edid_parser_context = si_edid_create_context(context,
			(struct drv_hw_context *)&context->drv_context);
	if (!context->edid_parser_context) {
		ret = -ENOMEM;
		goto free_device;
	}
	rcp_input_dev_one_time_init(context);
	ret = si_mhl_tx_reserve_resources(context);
	if (ret)
		goto free_edid;
	ret = si_mhl_tx_initialize(context);
	if (ret)
		goto free_edid;
	/* Publish the context before the IRQ can schedule disconnect work. */
	dev_set_drvdata(parent, context);
	ret = request_threaded_irq(info->irq, NULL, mhl_irq_handler,
		IRQF_TRIGGER_LOW | IRQF_ONESHOT, MHL_DEVICE_NAME, context);
	if (ret) {
		dev_set_drvdata(parent, NULL);
		goto free_edid;
	}
	return 0;
free_edid:
	context->dev_flags |= DEV_FLAG_SHUTDOWN;
	si_mhl_tx_shutdown(context);
	si_edid_destroy_context(context->edid_parser_context);
free_device:
	device_unregister(context->mhl_dev);
free_class:
	class_destroy(mhl_class);
	mhl_class = NULL;
free_hid:
#if INCLUDE_HID
	destroy_workqueue(context->hid_work_queue);
#endif
free_timers:
	mhl_tx_destroy_timer_support(context);
free_context:
	kfree(context);
	return ret;
}

int mhl_tx_remove(struct device *parent)
{
	struct mhl_dev_context *context = dev_get_drvdata(parent);

	if (!context)
		return 0;
	context->dev_flags |= DEV_FLAG_SHUTDOWN;
	free_irq(context->drv_info->irq, context);
#if INCLUDE_HID
	mhl3_hid_remove_all(context);
	destroy_workqueue(context->hid_work_queue);
#endif
	down(&context->isr_lock);
	si_mhl_tx_shutdown(context);
	up(&context->isr_lock);
	mhl_tx_destroy_timer_support(context);
	destroy_rcp_input_dev(context);
	si_edid_destroy_context(context->edid_parser_context);
	device_unregister(context->mhl_dev);
	class_destroy(mhl_class);
	mhl_class = NULL;
	dev_set_drvdata(parent, NULL);
	kfree(context);
	return 0;
}

void mhl_tx_stop_all_timers(struct mhl_dev_context *dev_context)
{
	struct timer_obj *timer = NULL;

	list_for_each_entry(timer, &dev_context->timer_list, list_link) {
		mhl_tx_stop_timer(dev_context, timer);
	}
}
static void mhl_tx_destroy_timer_support(struct mhl_dev_context *dev_context)
{
	struct timer_obj *mhl_timer;

	/*
	 * Make sure all outstanding timer objects are canceled and the
	 * memory allocated for them is freed.
	 */
	while (!list_empty(&dev_context->timer_list)) {
		mhl_timer = list_first_entry(&dev_context->timer_list,
					     struct timer_obj, list_link);
		hrtimer_cancel(&mhl_timer->hr_timer);
		cancel_work_sync(&mhl_timer->work_item);
		list_del(&mhl_timer->list_link);
		kfree(mhl_timer);
	}

	destroy_workqueue(dev_context->timer_work_queue);
	dev_context->timer_work_queue = NULL;
}

static void mhl_tx_timer_work_handler(struct work_struct *work)
{
	struct timer_obj *mhl_timer;

	mhl_timer = container_of(work, struct timer_obj, work_item);

	mhl_timer->flags |= TIMER_OBJ_FLAG_WORK_IP;
	if (!down_interruptible(&mhl_timer->dev_context->isr_lock)) {

		if (!(mhl_timer->dev_context->dev_flags &
		      (DEV_FLAG_SHUTDOWN | DEV_FLAG_POWER_OFF)))
			mhl_timer->timer_callback_handler(mhl_timer->callback_param);

		up(&mhl_timer->dev_context->isr_lock);
	}
	mhl_timer->flags &= ~TIMER_OBJ_FLAG_WORK_IP;

	if (mhl_timer->flags & TIMER_OBJ_FLAG_DEL_REQ) {
		/*
		 * Deletion of this timer was requested during the execution of
		 * the callback handler so go ahead and delete it now.
		 */
		kfree(mhl_timer);
	}
}

static enum hrtimer_restart mhl_tx_timer_handler(struct hrtimer *timer)
{
	struct timer_obj *mhl_timer;

	mhl_timer = container_of(timer, struct timer_obj, hr_timer);

	queue_work(mhl_timer->dev_context->timer_work_queue,
		   &mhl_timer->work_item);

	return HRTIMER_NORESTART;
}

static int is_timer_handle_valid(struct mhl_dev_context *dev_context,
				 void *timer_handle)
{
	struct timer_obj *timer = timer_handle; /* Set to avoid lint warning. */

	list_for_each_entry(timer, &dev_context->timer_list, list_link) {
		if (timer == timer_handle)
			break;
	}

	if (timer != timer_handle) {
		MHL_TX_DBG_WARN("Invalid timer handle %p received\n",
				timer_handle);
		return -EINVAL;
	}
	return 0;
}

int mhl_tx_create_timer(void *context,
			void (*callback_handler) (void *callback_param),
			void *callback_param, void **timer_handle)
{
	struct mhl_dev_context *dev_context;
	struct timer_obj *new_timer;

	dev_context = get_mhl_device_context(context);

	if (callback_handler == NULL)
		return -EINVAL;

	if (dev_context->timer_work_queue == NULL)
		return -ENOMEM;

	new_timer = kmalloc(sizeof(*new_timer), GFP_KERNEL);
	if (new_timer == NULL)
		return -ENOMEM;

	new_timer->timer_callback_handler = callback_handler;
	new_timer->callback_param = callback_param;
	new_timer->flags = 0;

	new_timer->dev_context = dev_context;
	INIT_WORK(&new_timer->work_item, mhl_tx_timer_work_handler);

	list_add(&new_timer->list_link, &dev_context->timer_list);

	hrtimer_init(&new_timer->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	new_timer->hr_timer.function = mhl_tx_timer_handler;
	*timer_handle = new_timer;
	return 0;
}

int mhl_tx_delete_timer(void *context, void **timer_handle)
{
	struct mhl_dev_context *dev_context;
	struct timer_obj *timer;
	int status;

	dev_context = get_mhl_device_context(context);

	status = is_timer_handle_valid(dev_context, *timer_handle);
	if (status == 0) {
		timer = *timer_handle;

		list_del(&timer->list_link);

		hrtimer_cancel(&timer->hr_timer);

		if (timer->flags & TIMER_OBJ_FLAG_WORK_IP) {
			/*
			 * Request to delete timer object came from within the
			 * timer's callback handler. If we were to proceed with
			 * the timer deletion we would deadlock at
			 * cancel_work_sync(). Instead, just flag that the user
			 * wants the timer deleted. Later when the timer
			 * callback completes the timer's work handler will
			 * complete the process of deleting this timer.
			 */
			timer->flags |= TIMER_OBJ_FLAG_DEL_REQ;
		} else {
			cancel_work_sync(&timer->work_item);
			*timer_handle = NULL;
			kfree(timer);
		}
	}

	return status;
}

int mhl_tx_start_timer(void *context, void *timer_handle, uint32_t time_msec)
{
	struct mhl_dev_context *dev_context;
	struct timer_obj *timer;
	ktime_t timer_period;
	int status;

	dev_context = get_mhl_device_context(context);

	status = is_timer_handle_valid(dev_context, timer_handle);
	if (status == 0) {
		long secs = 0;
		timer = timer_handle;

		secs = time_msec / 1000;
		time_msec %= 1000;
		timer_period = ktime_set(secs, MSEC_TO_NSEC(time_msec));
		hrtimer_start(&timer->hr_timer, timer_period, HRTIMER_MODE_REL);
	}

	return status;
}

int mhl_tx_stop_timer(void *context, void *timer_handle)
{
	struct mhl_dev_context *dev_context;
	struct timer_obj *timer;
	int status;

	dev_context = get_mhl_device_context(context);

	status = is_timer_handle_valid(dev_context, timer_handle);
	if (status == 0) {
		timer = timer_handle;
		hrtimer_cancel(&timer->hr_timer);
	}
	return status;
}
