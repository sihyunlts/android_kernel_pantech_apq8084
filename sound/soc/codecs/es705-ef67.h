/* EF67 ES705 connection to the WCD9330 sound card. */
#ifndef ES705_EF67_H
#define ES705_EF67_H

#include <sound/soc.h>

#ifdef CONFIG_SND_SOC_ES705_EF67
int ef67_es705_add_controls(struct snd_soc_codec *codec);
bool ef67_es705_route_enabled(unsigned int id);
int ef67_es705_get_channel_map(unsigned int id, unsigned int *tx_num,
	unsigned int *tx_slot, unsigned int *rx_num, unsigned int *rx_slot);
int ef67_es705_set_channel_map(unsigned int id, unsigned int tx_num,
	unsigned int *tx_slot, unsigned int rx_num, unsigned int *rx_slot);
int ef67_es705_hw_params(unsigned int id, struct snd_pcm_hw_params *params);
int ef67_es705_stream_start(unsigned int id);
int ef67_es705_stream_stop(unsigned int id);
#else
static inline int ef67_es705_add_controls(struct snd_soc_codec *codec)
{
	return 0;
}
static inline bool ef67_es705_route_enabled(unsigned int id)
{
	return false;
}
static inline int ef67_es705_get_channel_map(unsigned int id,
	unsigned int *tx_num, unsigned int *tx_slot,
	unsigned int *rx_num, unsigned int *rx_slot)
{
	return -ENODEV;
}
static inline int ef67_es705_set_channel_map(unsigned int id,
	unsigned int tx_num, unsigned int *tx_slot,
	unsigned int rx_num, unsigned int *rx_slot)
{
	return -ENODEV;
}
static inline int ef67_es705_hw_params(unsigned int id,
	struct snd_pcm_hw_params *params)
{
	return 0;
}
static inline int ef67_es705_stream_start(unsigned int id)
{
	return 0;
}
static inline int ef67_es705_stream_stop(unsigned int id)
{
	return 0;
}
#endif
#endif
