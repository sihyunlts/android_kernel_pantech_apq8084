/* Pantech EF67 FC8080 receiver and stock Android TDMB interface. */
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <mach/gpio.h>

#include "dmb_hw.h"
#include "dmb_interface.h"
#include "tdmb/tdmb_dev.h"
#include "tdmb/tdmb_bb.h"
#include "tdmb/fc8080/fc8080_wrapper.h"
#include "tdmb/tdmb_test.h"

/* The FCI library is a singleton; serialize IRQ, ioctl and stream access. */
static DEFINE_MUTEX(tdmb_lock);
static struct spi_device *tdmb_spi;
static struct clk *tdmb_xo;
static struct class *tdmb_class;
static struct cdev tdmb_cdev;
static dev_t tdmb_devt;
static struct fasync_struct *tdmb_async;
static int tdmb_power_gpio, tdmb_irq_gpio;
static bool tdmb_bound;
static int tdmb_io_error;
static int tdmb_receive_error;
static DECLARE_WAIT_QUEUE_HEAD(tdmb_read_wait);
static bool tdmb_packet_ready;
static bool tdmb_opened, tdmb_irq_enabled, tdmb_initialized;
static bool tdmb_clock_enabled, tdmb_playing, tdmb_channel_set;
bool power_on_flag;
static ts_data_type ts_data;
static fic_data_type fic_data;

extern tdmb_mode_type dmb_mode;
extern tdmb_autoscan_state autoscan_state;
extern tSignalQuality g_tSigQual;
extern uint8 gFrequencyBand, gFreqTableNum;
extern const uint32 KOREA_BAND_TABLE[21];

void dmb_spi_record_error(int error)
{
	if (error && !tdmb_io_error)
		tdmb_io_error = error;
}

void dmb_spi_clear_error(void)
{
	tdmb_io_error = 0;
}

struct spi_device *dmb_spi_setup(void)
{
	return tdmb_spi;
}

void dmb_power_on_chip(void)
{
	gpio_set_value_cansleep(tdmb_power_gpio, 1);
}

int dmb_power_on(void)
{
	int ret;

	if (tdmb_clock_enabled)
		return 0;
	ret = clk_prepare_enable(tdmb_xo);
	if (!ret)
		tdmb_clock_enabled = true;
	return ret;
}

void dmb_power_off(void)
{
	gpio_set_value_cansleep(tdmb_power_gpio, 0);
	msleep(1);
	if (tdmb_clock_enabled) {
		clk_disable_unprepare(tdmb_xo);
		tdmb_clock_enabled = false;
	}
}

boolean tdmb_set_isr(int enabled)
{
	/* The sleeping IRQ handler takes tdmb_lock before touching the chip. */
	tdmb_irq_enabled = !!enabled;
	return TRUE;
}

static void tdmb_shutdown(void)
{
	tdmb_irq_enabled = false;
	tdmb_bb_power_off();
	/* Also unwind a failed initialization or reference probe retry. */
	if (tdmb_clock_enabled)
		dmb_power_off();
	power_on_flag = false;
	tdmb_initialized = false;
	tdmb_playing = false;
	tdmb_packet_ready = false;
	tdmb_receive_error = 0;
	wake_up_interruptible(&tdmb_read_wait);
	tdmb_channel_set = false;
	memset(&ts_data, 0, sizeof(ts_data));
	memset(&fic_data, 0, sizeof(fic_data));
}

static void tdmb_receive_packet(void)
{
	memset(&ts_data, 0, sizeof(ts_data));
	ts_data.msc_size = tdmb_bb_read_msc((uint8 *)&ts_data);
	tdmb_packet_ready = ts_data.type != TYPE_NONE;
	if (tdmb_packet_ready)
		wake_up_interruptible(&tdmb_read_wait);
}

static irqreturn_t tdmb_interrupt(int irq, void *data)
{
	mutex_lock(&tdmb_lock);
	if (tdmb_spi && power_on_flag && tdmb_initialized && tdmb_irq_enabled) {
		dmb_spi_clear_error();
		tdmb_bb_read_int();
		/* Leave scan FIC available to READ_FIB; only cache playback data. */
		if (tdmb_playing && !tdmb_packet_ready)
			tdmb_receive_packet();
		if (tdmb_io_error) {
			tdmb_receive_error = tdmb_io_error;
			wake_up_interruptible(&tdmb_read_wait);
		}
		kill_fasync(&tdmb_async, SIGIO, POLL_IN);
	}
	mutex_unlock(&tdmb_lock);
	return IRQ_HANDLED;
}

static bool tdmb_frequency_valid(unsigned long freq)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(KOREA_BAND_TABLE); i++)
		if (freq == KOREA_BAND_TABLE[i])
			return true;
	return false;
}

static int tdmb_global_variable(g_var_type *var)
{
	if (var->type == G_READ) {
		switch (var->name) {
		case G_DMB_MODE: var->data = dmb_mode; break;
		case G_GFREQUENCYBAND: var->data = gFrequencyBand; break;
		case G_GFREQTABLENUM: var->data = gFreqTableNum; break;
		case G_AUTOSCAN_STATE: var->data = autoscan_state; break;
		case G_MICRO_USB_ANT: var->data = 0; break;
		default: return -EINVAL;
		}
	} else if (var->type == G_WRITE) {
		switch (var->name) {
		case G_DMB_MODE:
			if (var->data < 0 || var->data >= TDMB_MODE_MAX)
				return -EINVAL;
			dmb_mode = var->data;
			tdmb_bb_set_freq_band(KOREA_BAND, dmb_mode);
			break;
		case G_GFREQUENCYBAND:
			if (var->data != KOREA_BAND) return -EINVAL;
			gFrequencyBand = var->data;
			break;
		case G_GFREQTABLENUM:
			if (var->data < 0 || var->data > 21) return -EINVAL;
			gFreqTableNum = var->data;
			break;
		case G_AUTOSCAN_STATE:
			if (var->data < 0 || var->data > AUTOSCAN_EXIT_STATE)
				return -EINVAL;
			autoscan_state = var->data;
			break;
		default: return -EINVAL;
		}
	} else {
		return -EINVAL;
	}
	return 0;
}

static long tdmb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	get_frequency_type freq;
	ch_scan_type scan;
	chan_info channel;
	g_var_type var;
	u32 value = 0;
	long ret = 0;

	if (_IOC_TYPE(cmd) != IOCTL_TDMB_MAGIC ||
	    _IOC_NR(cmd) >= IOCTL_TDMB_MAXNR)
		return -EINVAL;
	mutex_lock(&tdmb_lock);
	if (!tdmb_spi) {
		ret = -ENODEV;
		goto out;
	}
	dmb_spi_clear_error();
	/* Stock declares one-byte ioctl sizes; its actual payloads are larger. */
	switch (cmd) {
	case IOCTL_TDMB_BB_DRV_INIT:
	case IOCTL_TDMB_BB_INIT:
		ret = tdmb_bb_drv_init();
		if (ret || tdmb_io_error) {
			if (!ret) ret = tdmb_io_error;
			tdmb_shutdown();
			break;
		}
		power_on_flag = true;
		tdmb_initialized = true;
		tdmb_playing = false;
		tdmb_packet_ready = false;
		tdmb_receive_error = 0;
		wake_up_interruptible(&tdmb_read_wait);
		tdmb_channel_set = false;
		memset(&ts_data, 0, sizeof(ts_data));
		break;
	case IOCTL_TDMB_BB_POWER_ON:
		ret = tdmb_bb_power_on();
		if (!ret) power_on_flag = true;
		break;
	case IOCTL_TDMB_BB_POWER_OFF:
		tdmb_shutdown();
		break;
	case IOCTL_TDMB_BB_SET_FIC_ISR:
		if (copy_from_user(&value, argp, sizeof(value))) { ret = -EFAULT; break; }
		value = tdmb_set_isr(value);
		goto copy_value;
	case IOCTL_TDMB_BB_SET_ANTENNA_PATH:
		if (copy_from_user(&value, argp, sizeof(value))) ret = -EFAULT;
		/* EF67 stock has no antenna switch operation. */
		break;
	case IOCTL_TDMB_BB_GET_FREQUENCY:
		if (copy_from_user(&freq, argp, sizeof(freq))) { ret = -EFAULT; break; }
		if (freq.band != KOREA_BAND ||
		    freq.index >= (dmb_mode == TDMB_MODE_SCAN_METRO ? 6 : 21)) {
			ret = -EINVAL; break;
		}
		tdmb_bb_get_frequency(&freq.freq, freq.band, freq.index);
		if (copy_to_user(argp, &freq, sizeof(freq))) ret = -EFAULT;
		break;
	case IOCTL_TDMB_GVAR_RW:
		if (copy_from_user(&var, argp, sizeof(var))) { ret = -EFAULT; break; }
		ret = tdmb_global_variable(&var);
		if (!ret && var.type == G_READ && copy_to_user(argp, &var, sizeof(var)))
			ret = -EFAULT;
		break;
	case IOCTL_TDMB_BB_SET_FIC_CH_RESULT:
		if (copy_from_user(&value, argp, sizeof(value))) ret = -EFAULT;
		else tdmb_bb_set_fic_ch_result(value);
		break;
	case IOCTL_TDMB_BB_GET_FIC_CH_RESULT:
		value = tdmb_bb_get_fic_ch_result();
		goto copy_value;
	case IOCTL_TDMB_BB_FIC_PROCESS:
	case IOCTL_TDMB_BB_CHW_INTHANDLER2:
	case IOCTL_TDMB_BB_DRV_START:
	case IOCTL_TDMB_BB_DRV_STOP:
	case IOCTL_TDMB_BB_REPORT_DEBUG_INFO:
	case IOCTL_TDMB_BB_READ_MSC:
	case IOCTL_TDMB_BB_I2C_TEST:
		/* These commands also have no operation in the stock SPI driver. */
		break;
	default:
		if (!power_on_flag || !tdmb_initialized) { ret = -EPIPE; break; }
		switch (cmd) {
		case IOCTL_TDMB_BB_CH_SCAN_START:
			if (copy_from_user(&scan, argp, sizeof(scan))) { ret = -EFAULT; break; }
			if (!tdmb_frequency_valid(scan.freq)) {
				ret = -EINVAL; break;
			}
			tdmb_bb_ch_scan_start(scan.freq, scan.band, scan.freq_offset);
			break;
		case IOCTL_TDMB_BB_RESYNC:
			if (copy_from_user(&value, argp, sizeof(value))) ret = -EFAULT;
			else tdmb_bb_resync(value);
			break;
		case IOCTL_TDMB_BB_SUBCH_START:
			if (!tdmb_channel_set) { ret = -EINVAL; break; }
			tdmb_packet_ready = false;
			tdmb_receive_error = 0;
			memset(&ts_data, 0, sizeof(ts_data));
			value = tdmb_bb_subch_start(NULL, 0);
			tdmb_playing = value && !tdmb_io_error;
			if (!tdmb_playing)
				wake_up_interruptible(&tdmb_read_wait);
			if (tdmb_playing && dmb_mode == TDMB_MODE_NETBER)
				netber_init();
			goto copy_value;
		case IOCTL_TDMB_BB_GET_TUNING_STATUS:
			value = tdmb_bb_get_tuning_status(); goto copy_value;
		case IOCTL_TDMB_BB_READ_INT:
			value = tdmb_bb_read_int(); goto copy_value;
		case IOCTL_TDMB_BB_GET_SYNC_STATUS:
			value = tdmb_bb_get_sync_status(); goto copy_value;
		case IOCTL_TDMB_BB_READ_FIB:
			memset(&fic_data, 0, sizeof(fic_data));
			fic_data.fib_num = tdmb_bb_read_fib(fic_data.fic_buf);
			if (copy_to_user(argp, &fic_data, sizeof(fic_data))) ret = -EFAULT;
			break;
		case IOCTL_TDMB_BB_SET_SUBCHANNEL_INFO:
			if (copy_from_user(&channel, argp, sizeof(channel))) { ret = -EFAULT; break; }
			if (channel.uiServiceType == TDMB_BB_SVC_VISUALRADIO)
				channel.uiServiceType = TDMB_BB_SVC_DMB;
			if (!tdmb_frequency_valid(channel.ulRFNum) || channel.uiSubChID > 63 ||
			    (channel.uiServiceType != TDMB_BB_SVC_DAB &&
			     channel.uiServiceType != TDMB_BB_SVC_DMB &&
			     channel.uiServiceType != TDMB_BB_SVC_DATA &&
			     channel.uiServiceType != TDMB_BB_SVC_NETBER) ||
			    (channel.uiServiceType == TDMB_BB_SVC_DAB &&
			     (!channel.uiBitRate || channel.uiBitRate > 384))) {
				ret = -EINVAL; break;
			}
			tdmb_bb_set_subchannel_info(&channel);
			tdmb_channel_set = true;
			break;
		case IOCTL_TDMB_BB_GET_BER:
			if (dmb_mode == TDMB_MODE_AIR_PLAY) tdmb_bb_get_ber();
			if (copy_to_user(argp, &g_tSigQual, sizeof(g_tSigQual))) ret = -EFAULT;
			break;
		case IOCTL_TDMB_BB_CH_STOP:
			tdmb_bb_ch_stop(); tdmb_playing = false;
			tdmb_packet_ready = false;
			tdmb_receive_error = 0;
			wake_up_interruptible(&tdmb_read_wait);
			memset(&ts_data, 0, sizeof(ts_data));
			break;
		case IOCTL_TDMB_BB_CH_TEST:
			if (copy_from_user(&value, argp, sizeof(value))) { ret = -EFAULT; break; }
			if (value > TDMB_NETBER) { ret = -EINVAL; break; }
			tdmb_bb_ch_test(value);
			if (!tdmb_io_error) tdmb_channel_set = true;
			break;
		case IOCTL_TDMB_BB_EBI2_CHW_INTHANDLER:
			/* Preserve the legacy pointer payload without dereferencing it. */
			if (copy_from_user(&value, argp, sizeof(value))) { ret = -EFAULT; break; }
			{
				void __user *buffer = (void __user *)(unsigned long)value;
				memset(&ts_data, 0, sizeof(ts_data));
				value = tdmb_bb_read_msc((uint8 *)&ts_data);
				if (copy_to_user(buffer, &ts_data, sizeof(ts_data))) { ret = -EFAULT; break; }
			}
			goto copy_value;
		default: ret = -EINVAL;
		}
	}
	goto out;
copy_value:
	if (copy_to_user(argp, &value, sizeof(value))) ret = -EFAULT;
out:
	if (!ret && tdmb_io_error) ret = tdmb_io_error;
	mutex_unlock(&tdmb_lock);
	return ret;
}

static int tdmb_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	mutex_lock(&tdmb_lock);
	if (!tdmb_spi) ret = -ENODEV;
	else if (tdmb_opened) ret = -EBUSY;
	else tdmb_opened = true;
	mutex_unlock(&tdmb_lock);
	return ret;
}

static int tdmb_fasync(int fd, struct file *file, int on)
{
	return fasync_helper(fd, file, on, &tdmb_async);
}

static int tdmb_release(struct inode *inode, struct file *file)
{
	tdmb_fasync(-1, file, 0);
	mutex_lock(&tdmb_lock);
	if (tdmb_spi) tdmb_shutdown();
	tdmb_opened = false;
	mutex_unlock(&tdmb_lock);
	return 0;
}

static ssize_t tdmb_read(struct file *file, char __user *buffer,
			 size_t length, loff_t *offset)
{
	int ret = 0;
	/* Stock libdmb_service requests 15756 bytes for a 12684-byte packet.
	 * Treat count as the user's capacity, without copying past our packet.
	 */
	length = min_t(size_t, length, sizeof(ts_data));
	if (!length) return 0;
	mutex_lock(&tdmb_lock);
	while (tdmb_spi && power_on_flag && tdmb_initialized &&
	       tdmb_playing && !tdmb_packet_ready && !tdmb_receive_error) {
		mutex_unlock(&tdmb_lock);
		if (file->f_flags & O_NONBLOCK) return -EAGAIN;
		ret = wait_event_interruptible(tdmb_read_wait,
			ACCESS_ONCE(tdmb_packet_ready) || ACCESS_ONCE(tdmb_receive_error) ||
			!ACCESS_ONCE(tdmb_playing) ||
			!ACCESS_ONCE(power_on_flag) || !ACCESS_ONCE(tdmb_spi));
		if (ret) return ret;
		mutex_lock(&tdmb_lock);
	}
	if (!tdmb_spi) ret = -ENODEV;
	else if (!power_on_flag || !tdmb_initialized) ret = -EPIPE;
	else if (tdmb_receive_error) {
		ret = tdmb_receive_error;
		tdmb_receive_error = 0;
	}
	else if (copy_to_user(buffer, &ts_data, length)) ret = -EFAULT;
	else {
		if (dmb_mode == TDMB_MODE_NETBER)
			netber_GetError(ts_data.msc_size, ts_data.msc_buf);
		tdmb_packet_ready = false;
		if (tdmb_playing) tdmb_receive_packet();
	}
	mutex_unlock(&tdmb_lock);
	/* Stock fills ts_data but returns zero, including successful reads. */
	return ret;
}

static ssize_t tdmb_write(struct file *file, const char __user *buffer,
			  size_t length, loff_t *offset)
{
	return 0;
}

static const struct file_operations tdmb_fops = {
	.owner = THIS_MODULE, .open = tdmb_open, .release = tdmb_release,
	.unlocked_ioctl = tdmb_ioctl, .read = tdmb_read, .write = tdmb_write,
	.fasync = tdmb_fasync, .llseek = no_llseek,
};

static int tdmb_probe(struct spi_device *spi)
{
	bool claimed = false;
	struct device *device;
	int ret;

	mutex_lock(&tdmb_lock);
	if (tdmb_bound || tdmb_opened) { ret = -EBUSY; goto out; }
	tdmb_bound = true;
	claimed = true;
	tdmb_irq_gpio = of_get_named_gpio(spi->dev.of_node, "dmb,irq-gpio", 0);
	tdmb_power_gpio = of_get_named_gpio(spi->dev.of_node, "dmb,power-gpio", 0);
	if (tdmb_irq_gpio < 0) { ret = tdmb_irq_gpio; goto out; }
	if (tdmb_power_gpio < 0) { ret = tdmb_power_gpio; goto out; }
	tdmb_xo = clk_get_sys("dmb_clk", "xo");
	if (IS_ERR(tdmb_xo)) { ret = PTR_ERR(tdmb_xo); goto out; }
	ret = gpio_request(tdmb_power_gpio, "dmb_power");
	if (ret) goto put_clock;
	ret = gpio_request(tdmb_irq_gpio, "dmb_irq");
	if (ret) goto free_power;
	ret = gpio_tlmm_config(GPIO_CFG(tdmb_power_gpio, 0, GPIO_CFG_OUTPUT,
			      GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (ret) goto free_gpio;
	ret = gpio_direction_output(tdmb_power_gpio, 0);
	if (ret) goto free_gpio;
	ret = gpio_tlmm_config(GPIO_CFG(tdmb_irq_gpio, 0, GPIO_CFG_INPUT,
			      GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (ret) goto free_gpio;
	ret = gpio_direction_input(tdmb_irq_gpio);
	if (ret) goto free_gpio;
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) goto free_gpio;
	spi->irq = gpio_to_irq(tdmb_irq_gpio);
	if (spi->irq < 0) { ret = spi->irq; goto free_gpio; }
	ret = request_threaded_irq(spi->irq, NULL, tdmb_interrupt,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "tdmb", spi);
	if (ret) goto free_gpio;
	ret = alloc_chrdev_region(&tdmb_devt, 0, 1, "tdmb");
	if (ret) goto free_irq;
	tdmb_class = class_create(THIS_MODULE, "tdmb");
	if (IS_ERR(tdmb_class)) { ret = PTR_ERR(tdmb_class); goto unregister; }
	cdev_init(&tdmb_cdev, &tdmb_fops);
	ret = cdev_add(&tdmb_cdev, tdmb_devt, 1);
	if (ret) goto destroy_class;
	if (!tdmb_bb_func_tbl_init()) { ret = -ENODEV; goto del_cdev; }
	tdmb_spi = spi;
	device = device_create(tdmb_class, &spi->dev, tdmb_devt, NULL, "tdmb");
	if (IS_ERR(device)) { ret = PTR_ERR(device); tdmb_spi = NULL; goto del_cdev; }
	mutex_unlock(&tdmb_lock);
	return 0;
del_cdev:
	cdev_del(&tdmb_cdev);
destroy_class:
	class_destroy(tdmb_class);
unregister:
	unregister_chrdev_region(tdmb_devt, 1);
free_irq:
	/* The IRQ thread waits on tdmb_lock, so never free it under that lock. */
	mutex_unlock(&tdmb_lock);
	free_irq(spi->irq, spi);
	mutex_lock(&tdmb_lock);
free_gpio:
	gpio_free(tdmb_irq_gpio);
free_power:
	gpio_free(tdmb_power_gpio);
put_clock:
	clk_put(tdmb_xo);
out:
	if (claimed) tdmb_bound = false;
	mutex_unlock(&tdmb_lock);
	return ret;
}

static int tdmb_remove(struct spi_device *spi)
{
	device_destroy(tdmb_class, tdmb_devt);
	cdev_del(&tdmb_cdev);
	class_destroy(tdmb_class);
	unregister_chrdev_region(tdmb_devt, 1);
	mutex_lock(&tdmb_lock);
	tdmb_shutdown();
	tdmb_spi = NULL;
	mutex_unlock(&tdmb_lock);
	free_irq(spi->irq, spi);
	gpio_free(tdmb_irq_gpio);
	gpio_free(tdmb_power_gpio);
	clk_put(tdmb_xo);
	mutex_lock(&tdmb_lock);
	tdmb_bound = false;
	mutex_unlock(&tdmb_lock);
	return 0;
}

static const struct of_device_id tdmb_of_match[] = {
	{ .compatible = "dmb,dmb_spi" }, { },
};
MODULE_DEVICE_TABLE(of, tdmb_of_match);

static struct spi_driver tdmb_driver = {
	.driver = { .name = "tdmb", .owner = THIS_MODULE,
		    .of_match_table = tdmb_of_match },
	.probe = tdmb_probe, .remove = tdmb_remove,
};
module_spi_driver(tdmb_driver);
MODULE_DESCRIPTION("Pantech EF67 FC8080 terrestrial DMB receiver");
MODULE_LICENSE("GPL");
