#ifndef _DMB_INTERFACE_H_
#define _DMB_INTERFACE_H_
#include <linux/spi/spi.h>
struct spi_device *dmb_spi_setup(void);
void dmb_spi_record_error(int error);
void dmb_spi_clear_error(void);
#endif
