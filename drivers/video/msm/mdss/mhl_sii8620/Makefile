ccflags-y += -I$(src) -I$(srctree)/drivers/hid -DMHL_DRIVER_NAME=\"sii8620drv\"
ccflags-y += -DMHL_DEVICE_NAME=\"sii-8620\" -DMHL_PRODUCT_NUM=8620
ccflags-y += -DMANUAL_EDID_FETCH -DSIMG_USE_DTS
ccflags-y += -DINCLUDE_HID=1 -DINCLUDE_RBP=0 -DINCLUDE_SII6031=0
ccflags-y += -DLINUX_KERNEL_VER=310

sii8620-y := platform.o mhl_linux_tx.o mhl_rcp_inputdev.o mhl_supp.o
sii8620-y += si_8620_drv.o si_mhl2_edid_3d.o si_emsc_hid.o si_emsc_hid-mt.o
obj-$(CONFIG_SII8620_MHL_TX) += sii8620.o
