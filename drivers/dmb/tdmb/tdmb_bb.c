//=============================================================================
// File       : tdmb_bb.c
//
// Description:
//
// Revision History:
//
// Version         Date           Author        Description of Changes
//-----------------------------------------------------------------------------
//  1.0.0       2007/08/08       hkyu           drafty
//  1.1.0       2009/04/28       yschoi         Android Porting
//=============================================================================

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <mach/gpio.h>
#include <asm/irq.h>

#include "tdmb_comdef.h"
#include "tdmb_dev.h"
#include "tdmb_chip.h"
#include "tdmb_bb.h"
#include "tdmb_test.h"

#define TDMB_BB_DRIVE_INIT(x)  \
      tdmb_bb_fc8080_init(x); \
      TDMB_MSG_BB("TDMB BB ---> [FC8080]");

tdmb_bb_function_table_type tdmb_bb_function_table;
static boolean tdmb_bb_initialized = FALSE;
tBBSYNCStage bb_sync_stage;
tSignalQuality g_tSigQual;

tdmb_mode_type dmb_mode = TDMB_MODE_AIR_PLAY;
tdmb_autoscan_state autoscan_state = AUTOSCAN_EXIT_STATE;

const uint32 KOREA_BAND_TABLE[] = {
  175280,177008,178736,
  181280,183008,184736,
  187280,189008,190736,
  193280,195008,196736,
  199280,201008,202736,
  205280,207008,208736,
  211280,213008,214736
};

const uint16 BAND_TABLE_MAX[]= {21, 38, 23, 23, 31};//{KOREA, BAND-3, L-BAND, CANADA, CHINA_BAND_III}

const uint32 KOREA_BAND_TABLE_METRO[] = {
  181280,183008,184736,
  205280,207008,208736,
};

uint8 gFrequencyBand;
uint8 gFreqTableNum;

tBBStatus ch_verify_result = BB_SUCCESS;

int tdmb_bb_set_freq_band(int, int);

boolean tdmb_bb_func_tbl_init(void)
{
  TDMB_MSG_BB("[%s]~!!!\n", __func__);

  if(tdmb_bb_initialized)
    return TRUE;

  memset((void*)&tdmb_bb_function_table, 0, sizeof(tdmb_bb_function_table_type));

  tdmb_bb_initialized = TDMB_BB_DRIVE_INIT(&tdmb_bb_function_table);

  return tdmb_bb_initialized;
}

int tdmb_bb_set_freq_band(int which_Band, int scan_mode)
{
  int ret = 1;

  switch(which_Band)
  {
    case KOREA_BAND:
      gFrequencyBand = KOREA_BAND;
      if(scan_mode == TDMB_MODE_SCAN_METRO)
      {
        gFreqTableNum = (sizeof(KOREA_BAND_TABLE_METRO) / sizeof(KOREA_BAND_TABLE_METRO[0]));
      }
      else if(scan_mode == TDMB_MODE_AUTOSCAN)
      {
        gFreqTableNum = BAND_TABLE_MAX[(KOREA_BAND-1)];
      }
    break;

    case BAND_III:
    case L_BAND:
    case CANADA_BAND:
    default:
      ret = 0;
    break;
  }

  return ret;
}

int tdmb_bb_drv_init(void)
{
  int ret = tdmb_bb_power_on();
  if (ret)
    return ret;
  return tdmb_bb_init();
}

int tdmb_bb_init(void)
{
  return tdmb_bb_function_table.tdmb_bb_drv_init() ? 0 : -EIO;
}

int tdmb_bb_power_on(void)
{
  if (!tdmb_bb_func_tbl_init())
    return -ENODEV;
  return tdmb_bb_function_table.tdmb_bb_power_on();
}

void tdmb_bb_power_off(void)
{
  TDMB_MSG_BB("[%s]~!!!\n", __func__);

  tdmb_bb_function_table.tdmb_bb_power_off();

}

void tdmb_bb_ch_scan_start(int freq, int band, unsigned char freq_offset)
{
  TDMB_MSG_BB("[%s]~!!!\n", __func__);

  tdmb_bb_function_table.tdmb_bb_ch_scan_start(freq, band, freq_offset);

}

void tdmb_bb_get_frequency(unsigned long *freq, unsigned char band,unsigned int index)
{

  switch(band)
  {
    case KOREA_BAND :
      if(dmb_mode == TDMB_MODE_SCAN_METRO)
      {
        *freq = KOREA_BAND_TABLE_METRO[index];
      }
      else if(dmb_mode == TDMB_MODE_AUTOSCAN)
      {
        *freq = KOREA_BAND_TABLE[index];
      }

    break;

    case BAND_III :
    break;

    case L_BAND :
    break;

    case CANADA_BAND :
    break;

    default:
    break;
  }

}

void tdmb_bb_resync(unsigned char ucIMR)
{
  TDMB_MSG_BB("[%s]~!!!\n", __func__);

  tdmb_bb_function_table.tdmb_bb_resync(ucIMR);
}

int tdmb_bb_subch_start (uint8 *Regs, uint32 data_rate)
{
  int ret;

  ret = tdmb_bb_function_table.tdmb_bb_subch_start(Regs, data_rate);

  return ret;
}

tBBSYNCStage tdmb_bb_get_tuning_status(void)
{

  if(tdmb_bb_get_sync_status() == BB_SUCCESS)
  {
      bb_sync_stage = BB_SYNC_STAGE_3;
  }
  else
  {
      bb_sync_stage = BB_SYNC_STAGE_0;
    TDMB_MSG_BB("[%s] SYNC_LOCK fail %d\n", __func__, bb_sync_stage);
  }

  return bb_sync_stage;
}

void tdmb_bb_set_fic_ch_result(tBBStatus result)
{
  TDMB_MSG_BB("[%s]~!!!\n", __func__);

  ch_verify_result = result;
}

tBBStatus tdmb_bb_get_fic_ch_result(void)
{
  TDMB_MSG_BB("[%s]~!!!\n", __func__);

  return ch_verify_result;
}

tdmb_bb_int_type tdmb_bb_read_int(void)
{
  return tdmb_bb_function_table.tdmb_bb_read_int();
}

tBBStatus tdmb_bb_get_sync_status(void)
{
  return tdmb_bb_function_table.tdmb_bb_get_sync_status();
}

dword tdmb_bb_read_fib(byte *fib_buf)
{
  return tdmb_bb_function_table.tdmb_bb_read_fib(fib_buf);
}

void tdmb_bb_set_subchannel_info(void *sub_ch_info)
{
  tdmb_bb_function_table.tdmb_bb_set_subchannel_info(sub_ch_info);
}

int tdmb_bb_read_msc(uint8 *msc_buf)
{
  int ret;

  ret = tdmb_bb_function_table.tdmb_bb_read_msc(msc_buf);

  return ret;
}

void tdmb_bb_get_ber(void)
{
  tdmb_bb_sig_type signals = { 0 };

  tdmb_bb_function_table.tdmb_bb_get_ber(&signals);

  g_tSigQual.PCBER = signals.PCBER;
  //antenna bar = RSSI = SNR  change..
  g_tSigQual.RSSI = (unsigned char)signals.SNR;
  g_tSigQual.SNR = (unsigned int)signals.RSSI;
  g_tSigQual.RSBER = signals.RSBER;
  g_tSigQual.RxPowerLevel = signals.RxPowerLevel;
}

void tdmb_bb_ch_stop(void)
{
  tdmb_bb_function_table.tdmb_bb_ch_stop();
}

void tdmb_bb_ch_test(int ch)
{
  tdmb_bb_function_table.tdmb_bb_ch_test(ch);
}
