//=============================================================================
// Telechips Confidential Proprietary
// Copyright (C) 2005-2006 Telechips Corporation
// All rights are reserved by Telechips Corporation

// File       : fc8080_bb.h
// Description:
//
// Revision History:
// Version   Date         Author       Description of Changes
//-----------------------------------------------------------------------------
// 1.0.0     2007/08/16   hkyu       Draft
//=============================================================================

#ifndef _FC8080_H_
#define _FC8080_H_
#include "../tdmb_bb.h"
#include "fci_types.h"
#include "bbm.h"
#include "fc8080_regs.h"
#include "fci_tun.h"

#include "fc8080_spi.h"

#define FC8080_VHFBAND_TUNER  110
#define FC8080_LBAND_TUNER    111

#define ARM_I2C_CTRL      1
#define FPGA_I2C_CTRL     2
#define ITD_I2C_CTRL      3

#define  TUNER_NOK    -1
#define  TUNER_OK     0
#define  TUNER_RETRY  10

#define BBM_BASE_ADDRESS  EBI2_GP1_BASE /* EBI2_CS1 */
#define BBM_BASE_OFFSET      0x2

typedef enum {
  FC8080_L_BAND,
  FC8080_VHF_BAND
} fc8080_band_type;

typedef struct _tagCHAN_INFO
{
  uint32  ulRFNum;
  uint16  uiEnsumbleID;
  uint16  uiSubChID;
  uint16  uiServiceType;
  uint16  uiStarAddr;
  uint16  uiBitRate;
  uint16  uiTmID;

  uint16  uiSlFlag;
  uint16  ucTableIndex;
  uint16  ucOption;
  uint16  uiProtectionLevel;
  uint16  uiDifferentRate;
  uint16  uiSchSize;
}chan_info;

boolean tdmb_bb_fc8080_init(tdmb_bb_function_table_type *function_table_ptr);

int fc8080_power_on(void);

void fc8080_power_off(void);

uint8 fc8080_drv_init(void);

void fc8080_ch_scan_start(int freq, int band, unsigned char for_air);

void fc8080_bb_resync(unsigned char imr);

int fc8080_subch_start(uint8 *regs, uint32 data_rate);

tdmb_bb_int_type fc8080_read_int(void);

tBBStatus fc8080_get_sync_status(void);

int fc8080_read_fib(uint8 *fibs);

void fc8080_set_subchannel_info(void *sub_ch_info);

int fc8080_read_msc(uint8 *msc_buffer);

void fc8080_get_ber(tdmb_bb_sig_type *);

uint8 fc8080_stop(void);

int fc8080_Ant_Level(int RSSI, uint32 pcber);

void fc8050_test(int ch);

#endif /* _FC8080_H_ */
