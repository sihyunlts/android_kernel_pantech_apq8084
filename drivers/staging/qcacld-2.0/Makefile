.DEFAULT_GOAL := wlan

QCACLD_ROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
QCACLD_SOURCE := $(patsubst %/,%,$(QCACLD_ROOT))

# Use a fully built target kernel: modules_prepare alone cannot provide its CRCs.
ifeq ($(strip $(KERNEL_SRC)),)
$(error KERNEL_SRC must name the target kernel source directory)
endif
ifeq ($(strip $(KERNEL_OUT)),)
$(error KERNEL_OUT must name the configured and fully built target kernel output)
endif

KERNEL_SRC := $(abspath $(KERNEL_SRC))
KERNEL_OUT := $(abspath $(KERNEL_OUT))
MODULE_OUT ?= $(KERNEL_OUT)/ef67-wlan
MODULE_OUT := $(abspath $(MODULE_OUT))
ARCH ?= arm

.PHONY: wlan
wlan:
	test -s $(KERNEL_OUT)/.config
	test -s $(KERNEL_OUT)/Module.symvers
	mkdir -p $(MODULE_OUT)
	tar -C $(QCACLD_SOURCE) --exclude=./.git --exclude=./out -cf - . | tar -C $(MODULE_OUT) -xf -
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_OUT) ARCH=$(ARCH) \
		M=$(MODULE_OUT) WLAN_ROOT=$(MODULE_OUT) MODNAME=wlan \
		WLAN_OPEN_SOURCE=1 CONFIG_QCA_CLD_WLAN=m \
		CONFIG_QCA_WIFI_ISOC=0 CONFIG_QCA_WIFI_2_0=1 modules
