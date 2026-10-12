//=============================================================================

// File       : fc8080_wrapper.c
// Description:
//
// Revision History:
// Version   Date         Author       Description of Changes
//-----------------------------------------------------------------------------
//  1.0.0       2009             jaksal
//  1.1.0       2010/12/06       yschoi         add tsif, i2c drivers
//=============================================================================

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>

#include <linux/mutex.h>
#include <linux/spinlock.h>

#include "../../dmb_hw.h"
#include "../../dmb_interface.h"

#include "../tdmb_comdef.h"
#include "../tdmb_dev.h"
#include "../tdmb_test.h"

#include "fc8080_wrapper.h"
#include "fc8080_isr.h"
#include "fci_types.h"
#include "fci_oal.h"

/*                      DEFINITION                                  */
#define FC8080_MSC_BUFFERS        16
#define FC8080_MSC_BUFFER_SIZE    (CH0_BUF_THR + 1)

/*                      TYPE DEFINITION                             */
typedef struct {
  uint32 freq;
  uint8  subChId;
  uint8  servicetype;
  uint16 bitrate;
} REGS;

tdmb_bb_int_type g_Int_type;

typedef struct _INT_TYPE
{
  uint8   valid;
  uint32  address;
  uint32  length;
  uint8   subch_id;
  uint8   svc_type;
}INT_TYPE;

INT_TYPE fic_type;
INT_TYPE msc_type[FC8080_MSC_BUFFERS];
int gInputBuffer = 0;
int gOutputBuffer = 0;

REGS fci_subch_info;
s8 rssi_value;

/*                      VARIABLE                                    */
static int subChId;
u8 msc_data[FC8080_MSC_BUFFER_SIZE * FC8080_MSC_BUFFERS];
u8 fc8080_fic_data[188*8*8];

tdmb_bb_service_type serviceType;
boolean tdmb_power_on = FALSE;

uint8 tune_res;
u16 g_mfIntStatus;
extern tdmb_mode_type dmb_mode;

static boolean fc8080_function_register(tdmb_bb_function_table_type *ftable_ptr)
{
  ftable_ptr->tdmb_bb_drv_init          = fc8080_drv_init;
  ftable_ptr->tdmb_bb_power_on          = fc8080_power_on;
  ftable_ptr->tdmb_bb_power_off         = fc8080_power_off;
  ftable_ptr->tdmb_bb_ch_scan_start     = fc8080_ch_scan_start;
  ftable_ptr->tdmb_bb_resync            = fc8080_bb_resync;
  ftable_ptr->tdmb_bb_subch_start       = fc8080_subch_start;
  ftable_ptr->tdmb_bb_read_int          = fc8080_read_int;
  ftable_ptr->tdmb_bb_get_sync_status   = fc8080_get_sync_status;
  ftable_ptr->tdmb_bb_read_fib          = fc8080_read_fib;
  ftable_ptr->tdmb_bb_set_subchannel_info = fc8080_set_subchannel_info;
  ftable_ptr->tdmb_bb_read_msc          = fc8080_read_msc;
  ftable_ptr->tdmb_bb_get_ber           = fc8080_get_ber;
  ftable_ptr->tdmb_bb_ch_stop           = fc8080_stop;
  ftable_ptr->tdmb_bb_ch_test           = fc8050_test;

  return TRUE;
}

int TDMB_FIC_CALLBACK(u32 userdata, u8 *data, int length)
{
  g_Int_type = TDMB_BB_INT_FIC;

  if (length <= 0 || length > BB_MAX_FIC_SIZE)
    return FALSE;

  if((fic_type.address == 0) && (fic_type.length == 0))
    fic_type.address = (uint32)&fc8080_fic_data[0];

  memcpy((void*)&fc8080_fic_data[0], data, length);
  fic_type.length = length;

  return TRUE;
}

int TDMB_MSC_CALLBACK(u32 userdata, u8 subChId, u8 *data, int length)
{
  if (length <= 0 || length > FC8080_MSC_BUFFER_SIZE)
    return FALSE;

  g_Int_type = TDMB_BB_INT_MSC;

  if(((gInputBuffer + 1) % FC8080_MSC_BUFFERS) == gOutputBuffer)
  {
    TDMB_MSG_FCI_BB("[%s] BUFFER ALLOCATION ERRROR\n", __func__);
    return 0;
  }

  memcpy((void*)&msc_data[gInputBuffer * FC8080_MSC_BUFFER_SIZE], data, length);

  msc_type[gInputBuffer].length = length;
  msc_type[gInputBuffer].subch_id = subChId;
  msc_type[gInputBuffer].address = (uint32)&msc_data[gInputBuffer * FC8080_MSC_BUFFER_SIZE];

  gInputBuffer = (gInputBuffer + 1) % FC8080_MSC_BUFFERS;

  return TRUE;
}

boolean tdmb_bb_fc8080_init(tdmb_bb_function_table_type *function_table_ptr)
{
    boolean bb_initialized;

    bb_initialized = fc8080_function_register(function_table_ptr);

    return bb_initialized;
}

int fc8080_power_on(void)
{
  TDMB_MSG_FCI_BB("[%s] start!!!\n", __func__);

  if(tdmb_power_on == TRUE)
  {
    TDMB_MSG_FCI_BB("[%s] return %d \n", __func__,tdmb_power_on);
    return 0;
  }

  //TDMB_EN first
  dmb_power_on_chip();
  msleep(5);

  if (dmb_power_on()) {
    dmb_power_off();
    return -EIO;
  }
  msleep(30);

  tdmb_power_on = TRUE;
  return 0;
}

void fc8080_power_off(void)
{
  TDMB_MSG_FCI_BB("[%s] start!!!\n", __func__);

  if(tdmb_power_on == FALSE)
  {
    TDMB_MSG_FCI_BB("[%s] return %d \n", __func__,tdmb_power_on);
    return;
  }

  dmb_power_off();

  tdmb_power_on = FALSE;
}

int8 fc8080_control_fic(uint8 enable, uint8 mode) //mode : 0(ts_mode), 1(i2c_mode)
{
  unsigned short mask;

  bbm_com_word_read(NULL, BBM_BUF_INT, &mask);

  if(enable == 1)
  {
    mask |= 0x100;
  }
  else
  {
    mask &= ~0x100;
  }

  bbm_com_word_write(NULL, BBM_BUF_INT, mask);
  bbm_com_word_write(NULL, BBM_BUF_ENABLE, mask);

  return TRUE;
}

void fc8080_ch_scan_start(int freq, int band, unsigned char for_air)
{
  TDMB_MSG_FCI_BB("[%s] Channel Frequency[%d] \n", __func__, freq);

  fc8080_control_fic(0, 1);

  if(bbm_com_tuner_set_freq(NULL, freq)){
    tune_res = BBM_NOK;
    return;
  }

  tune_res = bbm_com_scan_status(NULL);

  fc8080_control_fic(1, 1);

  TDMB_MSG_FCI_BB("[%s] tune_res[%d]\n", __func__, tune_res);
}

uint8 fc8080_drv_init(void)
{
  int res = BBM_NOK;
  int i;

  TDMB_MSG_FCI_BB("[%s] [SDK VER : 140410](%s)\n", __func__, DRIVER_VER);

  if(bbm_com_hostif_select(NULL, BBM_SPI))
  {
    TDMB_MSG_FCI_BB("[%s] hostif select fail!!! \n", __func__);
    return FALSE;
  }

  bbm_com_fic_callback_register(0, TDMB_FIC_CALLBACK);
  bbm_com_msc_callback_register(0, TDMB_MSC_CALLBACK);

  if(bbm_com_probe(NULL) == BBM_NOK)
  {
    for(i=0; i<10; i++)
    {
      dmb_power_off();
      msleep(250);
      dmb_power_on_chip();
      if (dmb_power_on())
        return FALSE;
      msleep(30);
      dmb_spi_clear_error();
      if(bbm_com_probe(NULL) == BBM_OK)
      {
        TDMB_MSG_FCI_BB("[%s] fc8080 probe ReTry %d OK\n",__func__, (i+1));
        break;
      }
    }
    if (i == 10)
      return FALSE;
  }

  res = bbm_com_init(NULL);
  res |= bbm_com_tuner_select(NULL, FC8080_TUNER, BAND3_TYPE);

  if(res)
    return FALSE;

  gInputBuffer = 0;
  gOutputBuffer = 0;

  memset((void*)&msc_type[0], 0x00, sizeof(msc_type));
  memset((void*)&fic_type, 0x00, sizeof(INT_TYPE));

  TDMB_MSG_FCI_BB("[%s] MULTI_CHANNEL \n", __func__);

  return TRUE;
}

void fc8080_bb_resync(unsigned char imr)
{
}

uint8 fc8080_stop(void)
{
  int res = BBM_NOK;

  TDMB_MSG_FCI_BB("[%s] \n", __func__);

  if(serviceType == TDMB_BB_SVC_DAB)
  {
    res=bbm_com_audio_deselect(NULL, 0, 2);
  }
  else if(serviceType == TDMB_BB_SVC_DMB)
  {
    res=bbm_com_video_deselect(NULL, 0, 0, 0);
  }
  else if(serviceType == TDMB_BB_SVC_DATA)
  {
    res=bbm_com_data_deselect(NULL, 0, 1);
  }
  else
  {
    return FALSE;
  }

  bbm_com_word_write(NULL, BBM_BUF_ENABLE, 0x0000); //FIC BUF clear

  ms_wait(60);

  if(res)
    return FALSE;

  gInputBuffer = 0;
  gOutputBuffer = 0;

  memset((void*)&msc_type[0], 0x00, sizeof(msc_type));
  memset((void*)&fic_type, 0x00, sizeof(INT_TYPE));

  return TRUE;
}

int fc8080_subch_start(uint8 *regs, uint32 data_rate)
{
  uint32 freq = 0;
  REGS* p_regs;
  int res = BBM_NOK;
  int fic_mode = 0;
  p_regs = &fci_subch_info;

  fc8080_stop();

  if(dmb_mode == TDMB_MODE_NETBER)
  {
    fc8080_control_fic(0, fic_mode); // FIC OFF
  }
  else
  {
    fc8080_control_fic(1, fic_mode); // FIC ON
  }

  freq = p_regs->freq;
  subChId = p_regs->subChId;
  serviceType = p_regs->servicetype;
  TDMB_MSG_FCI_BB("[%s] subchid[%x] type[%x] freq[%d]\n", __func__, subChId, serviceType, (int)freq);

  res = bbm_com_tuner_set_freq(NULL, freq);

  if(serviceType == TDMB_BB_SVC_DAB)
  {
    bbm_com_word_write(NULL, BBM_BUF_CH2_END, CH2_BUF_START + (fci_subch_info.bitrate*12*2)-1);
    bbm_com_word_write(NULL, BBM_BUF_CH2_THR, (fci_subch_info.bitrate*6*2)-1);
    res = bbm_com_audio_select(NULL, subChId, 2);
  }
  else if(serviceType == TDMB_BB_SVC_DMB)
  {
    res = bbm_com_video_select(NULL, subChId, 0, 0);
  }
  else if(serviceType == TDMB_BB_SVC_DATA)
  {
    res = bbm_com_data_select(NULL, subChId, 1);
  }

  if(res)
    return FALSE;

  return TRUE;
}

tdmb_bb_int_type fc8080_read_int(void)
{

  fc8080_isr(NULL);
  return g_Int_type;
}

tBBStatus fc8080_get_sync_status(void)
{
  uint8  sync_status = 0;

  bbm_com_read(NULL, BBM_SYNC_STAT, &sync_status);

  if(sync_status & 0x20)
  {
    TDMB_MSG_FCI_BB("[%s] SYNC SUCESS\n", __func__);
    return BB_SUCCESS;
  }
  else
  {
    TDMB_MSG_FCI_BB("[%s] SYNC FAIL\n", __func__);
    return BB_FAIL;
  }
}

int fc8080_read_fib(uint8 *fibs)
{
  uint32 size=0;
  size = min_t(uint32, fic_type.length, BB_MAX_FIC_SIZE);

  if(fic_type.address!=0)
  {
    memcpy(&fibs[0], (void*)fic_type.address, size);

  TDMB_MSG_FCI_BB("[%s] size[%d] data[%x]\n", __func__, (int)size,fibs[0]);
  }

  fic_type.length=0;
  fic_type.address=0;

  return size/32;
}

void fc8080_set_subchannel_info(void * sub_ch_info)
{
  chan_info *fci_ch_info;

  fci_ch_info = (chan_info *)sub_ch_info;

  fci_subch_info.freq = fci_ch_info->ulRFNum;
  fci_subch_info.servicetype = fci_ch_info->uiServiceType;
  fci_subch_info.subChId = (uint8)fci_ch_info->uiSubChID;
  fci_subch_info.bitrate = fci_ch_info->uiBitRate;
}

int fc8080_read_msc(uint8 *buffer)
{
  ts_data_type *packet = (ts_data_type *)buffer;
  unsigned int fic_size = min_t(unsigned int, fic_type.length, BB_MAX_FIC_SIZE);
  unsigned int msc_size;

  fic_type.length = 0;
  if (fic_size && fic_type.address) {
    memcpy(packet->fic_buf, (void *)fic_type.address, fic_size);
    packet->type = FIC_DATA;
    packet->fic_size = fic_size;
  }
  fic_type.address = 0;
  if (gOutputBuffer == gInputBuffer)
    return 0;

  msc_size = msc_type[gOutputBuffer].length;
  if (!msc_type[gOutputBuffer].address || !msc_size ||
      msc_size > FC8080_MSC_BUFFER_SIZE)
    return 0;
  memcpy(packet->msc_buf, &msc_data[gOutputBuffer * FC8080_MSC_BUFFER_SIZE], msc_size);
  packet->type = packet->type == FIC_DATA ? FIC_MSC_DATA : MSC_DATA;
  memset(&msc_type[gOutputBuffer], 0, sizeof(msc_type[gOutputBuffer]));
  gOutputBuffer = (gOutputBuffer + 1) % FC8080_MSC_BUFFERS;
  return msc_size;
}

void fc8080_get_ber(tdmb_bb_sig_type *psigs)
{
  struct dm_st {
        u32 start;

        u16 reserved0;
        u16 reserved1;
        u32 reserved2;

        u32 dmp_ber_rxd_bits;
        u32 dmp_ber_err_bits;

        u16 ber_rxd_rsps;
        u16 ber_err_rsps;
        u32 ber_err_bits;
  };

  struct dm_st dm = { 0 };

  bbm_com_bulk_read(NULL, BBM_DM, (u8*) &dm, sizeof(dm));

  rssi_value = (s8) ((dm.ber_err_bits & 0xff000000) >> 24);

  dm.ber_err_bits &= 0x00ffffff;
  dm.dmp_ber_err_bits &= 0x00ffffff;

  if(dm.dmp_ber_rxd_bits == 0)
        psigs->PCBER = 20000;
  else if(dm.dmp_ber_err_bits == 0)
        psigs->PCBER = 0;
  else
  {
        if(dm.dmp_ber_err_bits > 429490)
                psigs->PCBER = ((dm.dmp_ber_err_bits * 1000)/dm.dmp_ber_rxd_bits)*10;
        else
                psigs->PCBER = (dm.dmp_ber_err_bits*10000)/dm.dmp_ber_rxd_bits;
  }

  if(dm.ber_rxd_rsps == 0)
        psigs->RSBER = 20000;

  else if((dm.ber_err_bits == 0) && (dm.ber_err_rsps == 0))
  {
        psigs->RSBER = 0;
  }
  else
  {
        if(dm.ber_err_bits > 429490)
                psigs->RSBER = ((dm.ber_err_bits * 1000)/(dm.ber_rxd_rsps * 204 * 8))*10;
        else
                psigs->RSBER = (dm.ber_err_bits*10000)/(dm.ber_rxd_rsps * 204 * 8);
  }

  psigs->SNR = fc8080_Ant_Level(rssi_value, psigs->PCBER);
  psigs->RSSI = rssi_value;
  psigs->RxPowerLevel = rssi_value;
}

int fc8080_Ant_Level(int RSSI, uint32 pcber)
{
  uint8 level = 0;
  static uint16 save_level=0;
  uint16 ant_tbl[] = {1000, // 0 <-> 1
                       750,  // 1 <-> 2
                       500,  // 2 <-> 3
                       200};  // 3 <-> 4
  uint16 hystery_value[]= {0, 30, 30, 30};

  if(pcber >= ant_tbl[0])
  {
    save_level = level = 0;
    return save_level;
  }

  if((pcber >= ant_tbl[1] && pcber < ant_tbl[0])) level = 1;
  else if((pcber >= ant_tbl[2] && pcber < ant_tbl[1])) level = 2;
  else if((pcber >= ant_tbl[3] && pcber < ant_tbl[2])) level = 3;
  else if(pcber < ant_tbl[3]) level = 4;

  if(level == save_level + 1) // Level 이 1칸 올라간 경우에만.
  {
    if(pcber < (ant_tbl[level - 1] - hystery_value[level - 1]))
    {
      save_level = level;
    }
  }
  else
  {
    save_level = level;
  }

  return save_level;
}

void fc8050_test(int ch)
{
  uint32 bitrate;
  int result;
  REGS fci_regs;
  chan_info fci_ch_info;

  TDMB_MSG_FCI_BB("[%s] ch[%d]\n", __func__, ch);

  if(tdmb_power_on == FALSE)
  {
    if (fc8080_power_on())
      return;
  }

  if (!fc8080_drv_init())
    return;

  tdmb_get_fixed_chan_info((service_t)ch, &fci_ch_info);

  fci_regs.freq = fci_ch_info.ulRFNum;
  fci_regs.servicetype = (fci_ch_info.uiServiceType == TDMB_BB_SVC_DAB) ? 1 : 2;
  fci_ch_info.uiServiceType = fci_regs.servicetype;
  fci_regs.subChId = (uint8)fci_ch_info.uiSubChID;

  bitrate = (uint32)fci_ch_info.uiBitRate;

  TDMB_MSG_FCI_BB("[%s] freq[%d]  type[%d] subchid[0x%x] bitrate[%d]\n", __func__, (int)fci_regs.freq, fci_regs.servicetype, fci_regs.subChId, (int)bitrate);

  fc8080_set_subchannel_info(&fci_ch_info);

  // cys
  fc8080_ch_scan_start((int)fci_regs.freq, 0, 0);
//return;
  result = fc8080_subch_start((uint8*)&fci_regs, bitrate);

  if(result == FALSE)
  {
    TDMB_MSG_FCI_BB("[%s] fc8080_subch_start fail [%d]\n", __func__, result);
  }
}
