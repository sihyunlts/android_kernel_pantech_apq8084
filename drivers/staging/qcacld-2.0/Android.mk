LOCAL_PATH := $(call my-dir)

# Current integration is validated for EF67S only.
ifeq ($(TARGET_DEVICE),ef67)
ifeq ($(TARGET_BOARD_PLATFORM),apq8084)

QCACLD_SOURCE_PATH := $(LOCAL_PATH)
EF67_WLAN_MODULE_OUT := $(call intermediates-dir-for,ETC,qca_cld_wlan.ko)/qcacld-2.0
EF67_WLAN_BUILT_MODULE := $(EF67_WLAN_MODULE_OUT)/wlan.ko
EF67_WLAN_SOURCE_FILES := $(LOCAL_PATH)/Kbuild $(LOCAL_PATH)/Makefile \
    $(shell find $(LOCAL_PATH)/CORE $(LOCAL_PATH)/wcnss -type f)

include $(CLEAR_VARS)
LOCAL_MODULE := qca_cld_wlan.ko
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT)/lib/modules/qca_cld
LOCAL_PREBUILT_MODULE_FILE := $(EF67_WLAN_BUILT_MODULE)
# Retain the stock module and firmware-directory links without a boot service.
LOCAL_POST_INSTALL_CMD := \
    mkdir -p $(TARGET_OUT)/etc/firmware/wlan/qca_cld && \
    ln -sf /system/lib/modules/qca_cld/qca_cld_wlan.ko $(TARGET_OUT)/lib/modules/wlan.ko && \
    ln -sf /data/misc/wifi/WCNSS_qcom_cfg.ini $(TARGET_OUT)/etc/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini && \
    ln -sf /data/misc/wifi/wlan_mac.bin $(TARGET_OUT)/etc/firmware/wlan/qca_cld/wlan_mac.bin
include $(BUILD_PREBUILT)

# CM11's kernel target produces the .config, generated headers and symbol CRCs.
# Do not reuse a diagnostic prebuilt compiled for a different kernel.
$(EF67_WLAN_BUILT_MODULE): TARGET_KERNEL_BINARIES $(EF67_WLAN_SOURCE_FILES) $(QCACLD_SOURCE_PATH)/Makefile
	$(MAKE) -C $(QCACLD_SOURCE_PATH) \
		KERNEL_SRC=$(abspath $(TARGET_KERNEL_SOURCE)) \
		KERNEL_OUT=$(abspath $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ) \
		MODULE_OUT=$(abspath $(EF67_WLAN_MODULE_OUT)) \
		ARCH=$(TARGET_ARCH) $(ARM_CROSS_COMPILE)

endif # apq8084
endif # ef67
